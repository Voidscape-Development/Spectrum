// Spectrum Color Tools: exposure, white balance, contrast, levels, curves,
// HSL by color range, vibrance/saturation, split toning and 3D LUTs (.cube).

#include "../common/fx-filter.h"

#include <util/platform.h>
#include <util/dstr.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../common/presets.h"

#define CURVE_SIZE 1024
#define CURVE_POINTS 5
#define HSL_RANGES 8
#define MAX_LUT_SIZE 65

static const char *curve_channels[4] = {"r", "g", "b", "m"};

struct color_tools {
	pthread_mutex_t mutex;

	float *curve_pending; // RGBA (r, g, b, master) x CURVE_SIZE
	bool curve_dirty;
	bool curves_enabled;
	gs_texture_t *curve_texture;

	struct dstr lut_path; // loaded path (UI thread)
	float *lut_pending;   // RGBA x size^3, or NULL
	uint32_t lut_size;
	bool lut_dirty;
	gs_texture_t *lut_texture; // graphics thread
	uint32_t lut_texture_size;
};

/* ------------------------------------------------------------------------
 * Curves
 * --------------------------------------------------------------------- */

// Monotone cubic (Fritsch-Carlson) through points at x = 0, .25, ... 1.
static void bake_curve(const float y[CURVE_POINTS], float *out, size_t stride)
{
	const int n = CURVE_POINTS;
	float x[CURVE_POINTS], d[CURVE_POINTS - 1], m[CURVE_POINTS];
	for (int i = 0; i < n; i++)
		x[i] = (float)i / (float)(n - 1);
	for (int i = 0; i < n - 1; i++)
		d[i] = (y[i + 1] - y[i]) / (x[i + 1] - x[i]);
	m[0] = d[0];
	m[n - 1] = d[n - 2];
	for (int i = 1; i < n - 1; i++)
		m[i] = (d[i - 1] * d[i] <= 0.0f) ? 0.0f : (d[i - 1] + d[i]) * 0.5f;
	for (int i = 0; i < n - 1; i++) {
		if (d[i] == 0.0f) {
			m[i] = m[i + 1] = 0.0f;
			continue;
		}
		const float a = m[i] / d[i], b = m[i + 1] / d[i];
		const float s = a * a + b * b;
		if (s > 9.0f) {
			const float t = 3.0f / sqrtf(s);
			m[i] = t * a * d[i];
			m[i + 1] = t * b * d[i];
		}
	}
	for (int k = 0; k < CURVE_SIZE; k++) {
		const float t = (float)k / (float)(CURVE_SIZE - 1);
		int i = (int)(t * (n - 1));
		if (i > n - 2)
			i = n - 2;
		const float h = x[i + 1] - x[i];
		const float u = (t - x[i]) / h;
		const float u2 = u * u, u3 = u2 * u;
		float v = (2 * u3 - 3 * u2 + 1) * y[i] + (u3 - 2 * u2 + u) * h * m[i] + (-2 * u3 + 3 * u2) * y[i + 1] +
			  (u3 - u2) * h * m[i + 1];
		out[k * stride] = v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
	}
}

static void curve_key(char *buf, size_t size, int channel, int point)
{
	snprintf(buf, size, "ct_curve_%s_%d", curve_channels[channel], point);
}

/* ------------------------------------------------------------------------
 * .cube files
 * --------------------------------------------------------------------- */

// Parses an Adobe/Resolve .cube 3D LUT. Returns RGBA floats (r fastest) and
// the size, or NULL.
static float *load_cube(const char *path, uint32_t *size_out)
{
	char *text = os_quick_read_utf8_file(path);
	if (!text) {
		blog(LOG_WARNING, "[Spectrum] Color Tools: unable to read LUT %s", path);
		return NULL;
	}

	uint32_t size = 0;
	float dmin[3] = {0, 0, 0}, dmax[3] = {1, 1, 1};
	float *data = NULL;
	size_t count = 0, expected = 0;

	// Split lines by hand (strtok_r is not portable to MSVC).
	char *next = text;
	while (next && *next) {
		char *line = next;
		next = strpbrk(line, "\r\n");
		if (next)
			*next++ = '\0';
		while (*line == ' ' || *line == '\t')
			line++;
		if (!*line || *line == '#')
			continue;
		if (strncmp(line, "LUT_3D_SIZE", 11) == 0) {
			size = (uint32_t)atoi(line + 11);
			if (size < 2 || size > MAX_LUT_SIZE) {
				blog(LOG_WARNING, "[Spectrum] Color Tools: unsupported LUT size %u in %s", size, path);
				break;
			}
			expected = (size_t)size * size * size;
			data = bzalloc(expected * 4 * sizeof(float));
		} else if (strncmp(line, "LUT_1D_SIZE", 11) == 0) {
			blog(LOG_WARNING, "[Spectrum] Color Tools: 1D LUTs are not supported (%s)", path);
			break;
		} else if (strncmp(line, "DOMAIN_MIN", 10) == 0) {
			sscanf(line + 10, "%f %f %f", &dmin[0], &dmin[1], &dmin[2]);
		} else if (strncmp(line, "DOMAIN_MAX", 10) == 0) {
			sscanf(line + 10, "%f %f %f", &dmax[0], &dmax[1], &dmax[2]);
		} else if ((*line >= '0' && *line <= '9') || *line == '-' || *line == '.') {
			float r, g, b;
			if (data && count < expected && sscanf(line, "%f %f %f", &r, &g, &b) == 3) {
				const float v[3] = {r, g, b};
				for (int c = 0; c < 3; c++) {
					const float range = dmax[c] - dmin[c];
					data[count * 4 + c] = range != 0.0f ? (v[c] - dmin[c]) / range : v[c];
				}
				data[count * 4 + 3] = 1.0f;
				count++;
			}
		}
		// Other keywords (TITLE...) are ignored.
	}
	bfree(text);

	if (!data || count != expected) {
		if (data)
			blog(LOG_WARNING, "[Spectrum] Color Tools: LUT %s has %zu entries, expected %zu", path, count,
			     expected);
		bfree(data);
		return NULL;
	}
	*size_out = size;
	return data;
}

/* ------------------------------------------------------------------------
 * Hooks
 * --------------------------------------------------------------------- */

static void ct_create(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct color_tools *ct = bzalloc(sizeof(struct color_tools));
	pthread_mutex_init(&ct->mutex, NULL);
	ct->curve_pending = bzalloc(sizeof(float) * 4 * CURVE_SIZE);
	fx->user = ct;
}

static void ct_destroy(fx_filter_t *fx)
{
	struct color_tools *ct = fx->user;
	obs_enter_graphics();
	if (ct->curve_texture)
		gs_texture_destroy(ct->curve_texture);
	if (ct->lut_texture)
		gs_voltexture_destroy(ct->lut_texture);
	obs_leave_graphics();
	bfree(ct->curve_pending);
	bfree(ct->lut_pending);
	dstr_free(&ct->lut_path);
	pthread_mutex_destroy(&ct->mutex);
	bfree(ct);
}

static void ct_update(fx_filter_t *fx, obs_data_t *settings)
{
	struct color_tools *ct = fx->user;
	char key[64];

	// Curves.
	bool enabled = false;
	pthread_mutex_lock(&ct->mutex);
	for (int ch = 0; ch < 4; ch++) {
		float y[CURVE_POINTS];
		for (int i = 0; i < CURVE_POINTS; i++) {
			curve_key(key, sizeof(key), ch, i);
			y[i] = (float)obs_data_get_double(settings, key) / 100.0f;
			if (fabsf(y[i] - (float)i / (CURVE_POINTS - 1)) > 1e-4f)
				enabled = true;
		}
		bake_curve(y, ct->curve_pending + ch, 4);
	}
	ct->curves_enabled = enabled;
	ct->curve_dirty = true;
	pthread_mutex_unlock(&ct->mutex);

	// LUT file: (re)load when the path changes.
	const char *path = obs_data_get_string(settings, "ct_lut_file");
	if (!ct->lut_path.array || strcmp(ct->lut_path.array, path) != 0) {
		dstr_copy(&ct->lut_path, path);
		uint32_t size = 0;
		float *data = (path && *path) ? load_cube(path, &size) : NULL;
		pthread_mutex_lock(&ct->mutex);
		bfree(ct->lut_pending);
		ct->lut_pending = data;
		ct->lut_size = data ? size : 0;
		ct->lut_dirty = true;
		pthread_mutex_unlock(&ct->mutex);
	}
}

static void ct_set_params(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, obs_data_t *to, float t,
			  uint32_t width, uint32_t height)
{
	UNUSED_PARAMETER(to);
	UNUSED_PARAMETER(t);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	struct color_tools *ct = fx->user;

	pthread_mutex_lock(&ct->mutex);
	if (ct->curve_dirty || !ct->curve_texture) {
		const uint8_t *data = (const uint8_t *)ct->curve_pending;
		if (!ct->curve_texture)
			ct->curve_texture = gs_texture_create(CURVE_SIZE, 1, GS_RGBA32F, 1, &data, GS_DYNAMIC);
		else
			gs_texture_set_image(ct->curve_texture, data, CURVE_SIZE * 4 * sizeof(float), false);
		ct->curve_dirty = false;
	}
	if (ct->lut_dirty) {
		if (ct->lut_texture) {
			gs_voltexture_destroy(ct->lut_texture);
			ct->lut_texture = NULL;
		}
		if (ct->lut_pending) {
			const uint8_t *data = (const uint8_t *)ct->lut_pending;
			ct->lut_texture =
				gs_voltexture_create(ct->lut_size, ct->lut_size, ct->lut_size, GS_RGBA32F, 1, &data, 0);
			ct->lut_texture_size = ct->lut_size;
		}
		ct->lut_dirty = false;
	}
	const bool curves = ct->curves_enabled;
	pthread_mutex_unlock(&ct->mutex);

	fx_set_texture(effect, "curve_lut", ct->curve_texture);
	fx_set_float(effect, "curve_size", (float)CURVE_SIZE);
	fx_set_float(effect, "curves_enabled", curves ? 1.0f : 0.0f);

	gs_eparam_t *p = gs_effect_get_param_by_name(effect, "lut3d");
	if (p)
		gs_effect_set_texture(p, ct->lut_texture);
	fx_set_float(effect, "lut3d_size", (float)(ct->lut_texture ? ct->lut_texture_size : 2));
	fx_set_float(effect, "lut3d_enabled", ct->lut_texture ? 1.0f : 0.0f);

	bool hsl = false;
	char key[32];
	for (int r = 0; r < HSL_RANGES && !hsl; r++) {
		static const char *parts = "hsl";
		for (int k = 0; k < 3; k++) {
			snprintf(key, sizeof(key), "ct_hsl_%d_%c", r, parts[k]);
			if (obs_data_get_double(settings, key) != 0.0)
				hsl = true;
		}
	}
	fx_set_float(effect, "hsl_enabled", hsl ? 1.0f : 0.0f);
}

static const struct fx_filter_def color_tools_def = {
	.effect_file = "shaders/fx/color-tools.effect",
	.dither_key = "ct_dither",
	.set_params = ct_set_params,
	.create = ct_create,
	.update = ct_update,
	.destroy = ct_destroy,
};

/* ------------------------------------------------------------------------
 * Source info
 * --------------------------------------------------------------------- */

static const char *color_tools_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ColorToolsFilter");
}

static void *color_tools_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&color_tools_def, settings, source);
}

static void color_tools_defaults(obs_data_t *settings)
{
	char key[64];
	obs_data_set_default_double(settings, "ct_gamma", 1.0);
	obs_data_set_default_double(settings, "ct_in_white", 100.0);
	obs_data_set_default_double(settings, "ct_out_white", 100.0);
	for (int ch = 0; ch < 4; ch++)
		for (int i = 0; i < CURVE_POINTS; i++) {
			curve_key(key, sizeof(key), ch, i);
			obs_data_set_default_double(settings, key, 100.0 * i / (CURVE_POINTS - 1));
		}
	obs_data_set_default_int(settings, "ct_curve_channel", 3);
	obs_data_set_default_int(settings, "ct_hsl_range", 0);
	obs_data_set_default_int(settings, "ct_split_shadows", 0xFFB06020);
	obs_data_set_default_int(settings, "ct_split_highlights", 0xFF40A0FF);
	obs_data_set_default_double(settings, "ct_lut_amount", 100.0);
	obs_data_set_default_double(settings, "ct_amount", 100.0);
	obs_data_set_default_bool(settings, "ct_dither", true);
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static bool curve_channel_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	char key[64];
	const int shown = (int)obs_data_get_int(settings, "ct_curve_channel");
	for (int ch = 0; ch < 4; ch++)
		for (int i = 0; i < CURVE_POINTS; i++) {
			curve_key(key, sizeof(key), ch, i);
			obs_property_set_visible(obs_properties_get(props, key), ch == shown);
		}
	return true;
}

static bool hsl_range_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	char key[32];
	const int shown = (int)obs_data_get_int(settings, "ct_hsl_range");
	for (int r = 0; r < HSL_RANGES; r++)
		for (int k = 0; k < 3; k++) {
			snprintf(key, sizeof(key), "ct_hsl_%d_%c", r, "hsl"[k]);
			obs_property_set_visible(obs_properties_get(props, key), r == shown);
		}
	return true;
}

static bool reset_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	fx_filter_t *fx = data;
	obs_data_t *settings = obs_source_get_settings(fx->context);
	obs_data_clear(settings);
	obs_source_update(fx->context, settings);
	obs_data_release(settings);
	return true;
}

static obs_properties_t *color_tools_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;
	char key[64];

	obs_properties_t *basic = obs_properties_create();
	slider(basic, "ct_exposure", "ColorTools.Exposure", -5.0, 5.0, 0.01, " EV");
	slider(basic, "ct_temperature", "ColorTools.Temperature", -100.0, 100.0, 0.1, NULL);
	slider(basic, "ct_tint", "ColorTools.Tint", -100.0, 100.0, 0.1, NULL);
	slider(basic, "ct_contrast", "ColorTools.Contrast", -100.0, 100.0, 0.1, NULL);
	slider(basic, "ct_saturation", "ColorTools.Saturation", -100.0, 100.0, 0.1, NULL);
	slider(basic, "ct_vibrance", "ColorTools.Vibrance", -100.0, 100.0, 0.1, NULL);
	obs_properties_add_group(props, "ct_basic_group", obs_module_text("ColorTools.Basic"), OBS_GROUP_NORMAL, basic);

	obs_properties_t *levels = obs_properties_create();
	slider(levels, "ct_in_black", "ColorTools.InBlack", 0.0, 100.0, 0.1, "%");
	slider(levels, "ct_in_white", "ColorTools.InWhite", 0.0, 100.0, 0.1, "%");
	slider(levels, "ct_gamma", "GradientMap.Gamma", 0.1, 5.0, 0.01, NULL);
	slider(levels, "ct_out_black", "ColorTools.OutBlack", 0.0, 100.0, 0.1, "%");
	slider(levels, "ct_out_white", "ColorTools.OutWhite", 0.0, 100.0, 0.1, "%");
	obs_properties_add_group(props, "ct_levels_group", obs_module_text("ColorTools.Levels"), OBS_GROUP_NORMAL,
				 levels);

	obs_properties_t *curves = obs_properties_create();
	p = obs_properties_add_list(curves, "ct_curve_channel", obs_module_text("ColorTools.Channel"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("ColorTools.Channel.Master"), 3);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Red"), 0);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Green"), 1);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Blue"), 2);
	obs_property_set_modified_callback(p, curve_channel_modified);
	static const char *point_labels[CURVE_POINTS] = {"ColorTools.Curve.Black", "ColorTools.Curve.Shadows",
							 "ColorTools.Curve.Midtones", "ColorTools.Curve.Highlights",
							 "ColorTools.Curve.White"};
	for (int ch = 0; ch < 4; ch++)
		for (int i = 0; i < CURVE_POINTS; i++) {
			curve_key(key, sizeof(key), ch, i);
			slider(curves, key, point_labels[i], 0.0, 100.0, 0.1, "%");
		}
	obs_properties_add_group(props, "ct_curves_group", obs_module_text("ColorTools.Curves"), OBS_GROUP_NORMAL,
				 curves);

	obs_properties_t *hsl = obs_properties_create();
	p = obs_properties_add_list(hsl, "ct_hsl_range", obs_module_text("ColorTools.Range"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	static const char *ranges[HSL_RANGES] = {"ColorTools.Range.Reds",    "ColorTools.Range.Oranges",
						 "ColorTools.Range.Yellows", "ColorTools.Range.Greens",
						 "ColorTools.Range.Aquas",   "ColorTools.Range.Blues",
						 "ColorTools.Range.Purples", "ColorTools.Range.Magentas"};
	for (int r = 0; r < HSL_RANGES; r++)
		obs_property_list_add_int(p, obs_module_text(ranges[r]), r);
	obs_property_set_modified_callback(p, hsl_range_modified);
	for (int r = 0; r < HSL_RANGES; r++) {
		snprintf(key, sizeof(key), "ct_hsl_%d_h", r);
		slider(hsl, key, "ColorTools.Hue", -180.0, 180.0, 0.1, "°");
		snprintf(key, sizeof(key), "ct_hsl_%d_s", r);
		slider(hsl, key, "ColorTools.Saturation", -100.0, 100.0, 0.1, NULL);
		snprintf(key, sizeof(key), "ct_hsl_%d_l", r);
		slider(hsl, key, "ColorTools.Lightness", -100.0, 100.0, 0.1, NULL);
	}
	obs_properties_add_group(props, "ct_hsl_group", obs_module_text("ColorTools.Hsl"), OBS_GROUP_NORMAL, hsl);

	obs_properties_t *split = obs_properties_create();
	obs_properties_add_color(split, "ct_split_shadows", obs_module_text("ColorTools.SplitShadows"));
	obs_properties_add_color(split, "ct_split_highlights", obs_module_text("ColorTools.SplitHighlights"));
	slider(split, "ct_split_balance", "ColorTools.SplitBalance", -100.0, 100.0, 0.1, NULL);
	slider(split, "ct_split_amount", "Fx.Amount", 0.0, 100.0, 0.1, "%");
	obs_properties_add_group(props, "ct_split_group", obs_module_text("ColorTools.Split"), OBS_GROUP_NORMAL, split);

	obs_properties_t *lut = obs_properties_create();
	obs_properties_add_path(lut, "ct_lut_file", obs_module_text("ColorTools.LutFile"), OBS_PATH_FILE,
				"Cube LUT (*.cube);;", NULL);
	slider(lut, "ct_lut_amount", "Fx.Amount", 0.0, 100.0, 0.1, "%");
	obs_properties_add_group(props, "ct_lut_group", obs_module_text("ColorTools.Lut"), OBS_GROUP_NORMAL, lut);

	slider(props, "ct_amount", "ColorTools.Mix", 0.0, 100.0, 0.1, "%");
	obs_properties_add_bool(props, "ct_dither", obs_module_text("Gradient.Dither"));
	if (data)
		obs_properties_add_button2(props, "ct_reset", obs_module_text("ColorTools.Reset"), reset_clicked, data);
	spectrum_presets_properties(props, data ? ((fx_filter_t *)data)->context : NULL);
	return props;
}

struct obs_source_info spectrum_color_tools_filter = {
	.id = "spectrum_color_tools_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = color_tools_name,
	.create = color_tools_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = fx_cb_render,
	.get_properties = color_tools_properties,
	.get_defaults = color_tools_defaults,
};
