#include "gradient.h"

#include <graphics/vec2.h>
#include <util/bmem.h>
#include <util/dstr.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
// MSVC only defines these with _USE_MATH_DEFINES.
#ifndef M_SQRT2
#define M_SQRT2 1.41421356237309504880
#endif

// Implemented in gradient-editor.cpp (Qt).
extern void gradient_editor_open(gradient_t *g);

/* ------------------------------------------------------------------------
 * Settings keys
 * --------------------------------------------------------------------- */

// Builds "<prefix><name>". OBS copies key and property names, so one buffer
// per call site is enough as long as a single key is used per expression.
static const char *gkey(char *buf, const char *prefix, const char *name)
{
	snprintf(buf, 128, "%s%s", prefix, name);
	return buf;
}

#define KEY(name) gkey(kbuf, prefix, name)

/* ------------------------------------------------------------------------
 * Color math
 * --------------------------------------------------------------------- */

float gradient_srgb_to_linear(float v)
{
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

float gradient_linear_to_srgb(float v)
{
	if (v <= 0.0f)
		return 0.0f;
	return v <= 0.0031308f ? v * 12.92f : 1.055f * powf(v, 1.0f / 2.4f) - 0.055f;
}

static void linear_to_oklab(const float rgb[3], float lab[3])
{
	const float l = 0.4122214708f * rgb[0] + 0.5363325363f * rgb[1] + 0.0514459929f * rgb[2];
	const float m = 0.2119034982f * rgb[0] + 0.6806995451f * rgb[1] + 0.1073969566f * rgb[2];
	const float s = 0.0883024619f * rgb[0] + 0.2817188376f * rgb[1] + 0.6299787005f * rgb[2];
	const float l_ = cbrtf(l), m_ = cbrtf(m), s_ = cbrtf(s);
	lab[0] = 0.2104542553f * l_ + 0.7936177850f * m_ - 0.0040720468f * s_;
	lab[1] = 1.9779984951f * l_ - 2.4285922050f * m_ + 0.4505937099f * s_;
	lab[2] = 0.0259040371f * l_ + 0.7827717662f * m_ - 0.8086757660f * s_;
}

static void oklab_to_linear(const float lab[3], float rgb[3])
{
	const float l_ = lab[0] + 0.3963377774f * lab[1] + 0.2158037573f * lab[2];
	const float m_ = lab[0] - 0.1055613458f * lab[1] - 0.0638541728f * lab[2];
	const float s_ = lab[0] - 0.0894841775f * lab[1] - 1.2914855480f * lab[2];
	const float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
	rgb[0] = 4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s;
	rgb[1] = -1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s;
	rgb[2] = -0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s;
}

static bool in_gamut(const float rgb[3])
{
	const float eps = 1e-4f;
	return rgb[0] >= -eps && rgb[0] <= 1.0f + eps && rgb[1] >= -eps && rgb[1] <= 1.0f + eps && rgb[2] >= -eps &&
	       rgb[2] <= 1.0f + eps;
}

static float clamp01(float v)
{
	return v < 0.0f ? 0.0f : (v > 1.0f ? 1.0f : v);
}

// Oklab to linear sRGB, bringing out of gamut colors (from interpolating in
// Oklab/OkLCh) back into gamut by reducing chroma at constant lightness and
// hue, instead of clipping channels (which shifts the hue).
static void oklab_to_linear_gamut_mapped(const float lab[3], float rgb[3])
{
	oklab_to_linear(lab, rgb);
	if (in_gamut(rgb)) {
		for (int i = 0; i < 3; i++)
			rgb[i] = clamp01(rgb[i]);
		return;
	}

	const float L = clamp01(lab[0]);
	float lo = 0.0f, hi = 1.0f;
	float test[3] = {L, 0.0f, 0.0f};
	for (int i = 0; i < 20; i++) {
		const float mid = (lo + hi) * 0.5f;
		test[1] = lab[1] * mid;
		test[2] = lab[2] * mid;
		oklab_to_linear(test, rgb);
		if (in_gamut(rgb))
			lo = mid;
		else
			hi = mid;
	}
	test[1] = lab[1] * lo;
	test[2] = lab[2] * lo;
	oklab_to_linear(test, rgb);
	for (int i = 0; i < 3; i++)
		rgb[i] = clamp01(rgb[i]);
}

static void color_to_vec(uint32_t color, float out[4])
{
	out[0] = (float)(color & 0xFF) / 255.0f;
	out[1] = (float)((color >> 8) & 0xFF) / 255.0f;
	out[2] = (float)((color >> 16) & 0xFF) / 255.0f;
	out[3] = (float)((color >> 24) & 0xFF) / 255.0f;
}

// Converts an sRGB color into the coordinates of an interpolation space.
// For the OkLCh modes this is (L, C, h in degrees).
static void to_space(uint32_t color, enum gradient_interp interp, float out[4])
{
	float srgb[4];
	color_to_vec(color, srgb);
	out[3] = srgb[3];

	if (interp == GRADIENT_INTERP_SRGB) {
		out[0] = srgb[0];
		out[1] = srgb[1];
		out[2] = srgb[2];
		return;
	}

	float lin[3] = {gradient_srgb_to_linear(srgb[0]), gradient_srgb_to_linear(srgb[1]),
			gradient_srgb_to_linear(srgb[2])};
	if (interp == GRADIENT_INTERP_LINEAR_RGB) {
		out[0] = lin[0];
		out[1] = lin[1];
		out[2] = lin[2];
		return;
	}

	float lab[3];
	linear_to_oklab(lin, lab);
	if (interp == GRADIENT_INTERP_OKLAB) {
		out[0] = lab[0];
		out[1] = lab[1];
		out[2] = lab[2];
		return;
	}

	out[0] = lab[0];
	out[1] = sqrtf(lab[1] * lab[1] + lab[2] * lab[2]);
	float h = atan2f(lab[2], lab[1]) * (float)(180.0 / M_PI);
	out[2] = h < 0.0f ? h + 360.0f : h;
}

static bool is_lch(enum gradient_interp interp)
{
	return interp == GRADIENT_INTERP_OKLCH_SHORTER || interp == GRADIENT_INTERP_OKLCH_LONGER ||
	       interp == GRADIENT_INTERP_OKLCH_INCREASING || interp == GRADIENT_INTERP_OKLCH_DECREASING;
}

// Converts interpolation space coordinates (straight alpha) back to linear
// sRGB.
static void from_space(const float c[3], enum gradient_interp interp, float rgb[3])
{
	switch (interp) {
	case GRADIENT_INTERP_SRGB:
		for (int i = 0; i < 3; i++)
			rgb[i] = gradient_srgb_to_linear(clamp01(c[i]));
		return;
	case GRADIENT_INTERP_LINEAR_RGB:
		for (int i = 0; i < 3; i++)
			rgb[i] = clamp01(c[i]);
		return;
	case GRADIENT_INTERP_OKLAB:
		oklab_to_linear_gamut_mapped(c, rgb);
		return;
	default: {
		const float h = c[2] * (float)(M_PI / 180.0);
		const float lab[3] = {c[0], c[1] * cosf(h), c[1] * sinf(h)};
		oklab_to_linear_gamut_mapped(lab, rgb);
		return;
	}
	}
}

/* ------------------------------------------------------------------------
 * Baking
 * --------------------------------------------------------------------- */

static float apply_ease(float u, enum gradient_ease ease)
{
	switch (ease) {
	case GRADIENT_EASE_SMOOTH:
		return u * u * (3.0f - 2.0f * u);
	case GRADIENT_EASE_IN:
		return u * u;
	case GRADIENT_EASE_OUT:
		return 1.0f - (1.0f - u) * (1.0f - u);
	case GRADIENT_EASE_IN_OUT:
		return u < 0.5f ? 4.0f * u * u * u : 1.0f - powf(-2.0f * u + 2.0f, 3.0f) * 0.5f;
	case GRADIENT_EASE_STEP:
		return u < 1.0f ? 0.0f : 1.0f;
	default:
		return u;
	}
}

static int compare_stops(const void *a, const void *b)
{
	const struct gradient_stop *sa = a, *sb = b;
	return (sa->position > sb->position) - (sa->position < sb->position);
}

// Interpolates between two colors in space coordinates, premultiplied by
// alpha (except hue), as CSS Color 4 does, so transparent stops don't pull
// in their (invisible) color.
static void mix_colors(const float a[4], const float b[4], float u, enum gradient_interp interp, float out[4])
{
	const float alpha = a[3] + (b[3] - a[3]) * u;
	out[3] = alpha;

	float ha = a[2], hb = b[2];
	const bool lch = is_lch(interp);
	if (lch) {
		// An achromatic stop has no hue: use the other one's.
		const float achromatic = 2e-4f;
		if (a[1] < achromatic && b[1] >= achromatic)
			ha = hb;
		else if (b[1] < achromatic && a[1] >= achromatic)
			hb = ha;

		float diff = hb - ha;
		switch (interp) {
		case GRADIENT_INTERP_OKLCH_SHORTER:
			if (diff > 180.0f)
				diff -= 360.0f;
			else if (diff < -180.0f)
				diff += 360.0f;
			break;
		case GRADIENT_INTERP_OKLCH_LONGER:
			if (diff > 0.0f && diff < 180.0f)
				diff -= 360.0f;
			else if (diff > -180.0f && diff <= 0.0f && a[1] >= achromatic && b[1] >= achromatic)
				diff += 360.0f;
			break;
		case GRADIENT_INTERP_OKLCH_INCREASING:
			if (diff < 0.0f)
				diff += 360.0f;
			break;
		case GRADIENT_INTERP_OKLCH_DECREASING:
			if (diff > 0.0f)
				diff -= 360.0f;
			break;
		default:
			break;
		}
		float h = ha + diff * u;
		h = fmodf(h, 360.0f);
		if (h < 0.0f)
			h += 360.0f;
		out[2] = h;
	}

	const int premul_count = lch ? 2 : 3;
	for (int i = 0; i < premul_count; i++) {
		if (alpha > 1e-6f) {
			const float pa = a[i] * a[3], pb = b[i] * b[3];
			out[i] = (pa + (pb - pa) * u) / alpha;
		} else {
			out[i] = a[i] + (b[i] - a[i]) * u;
		}
	}
}

void gradient_bake(const struct gradient_stop *stops_in, size_t stop_count, enum gradient_interp interp, float *out,
		   size_t count)
{
	if (count == 0)
		return;

	if (stop_count == 0) {
		memset(out, 0, sizeof(float) * 4 * count);
		return;
	}
	if (stop_count > GRADIENT_MAX_STOPS)
		stop_count = GRADIENT_MAX_STOPS;

	struct gradient_stop stops[GRADIENT_MAX_STOPS];
	memcpy(stops, stops_in, sizeof(struct gradient_stop) * stop_count);
	qsort(stops, stop_count, sizeof(struct gradient_stop), compare_stops);

	float coords[GRADIENT_MAX_STOPS][4];
	for (size_t i = 0; i < stop_count; i++)
		to_space(stops[i].color, interp, coords[i]);

	size_t seg = 0;
	for (size_t i = 0; i < count; i++) {
		const float t = count > 1 ? (float)i / (float)(count - 1) : 0.0f;
		float c[4];

		if (t <= stops[0].position || stop_count == 1) {
			memcpy(c, coords[0], sizeof(c));
		} else if (t >= stops[stop_count - 1].position) {
			memcpy(c, coords[stop_count - 1], sizeof(c));
		} else {
			while (seg + 1 < stop_count - 1 && t >= stops[seg + 1].position)
				seg++;
			const struct gradient_stop *s0 = &stops[seg];
			const struct gradient_stop *s1 = &stops[seg + 1];
			const float span = s1->position - s0->position;
			float u = span > 1e-7f ? (t - s0->position) / span : 1.0f;
			u = clamp01(u);

			// Midpoint: move the half way point of the segment.
			const float m = fminf(fmaxf(s0->midpoint, 0.01f), 0.99f);
			if (fabsf(m - 0.5f) > 1e-4f && u > 0.0f)
				u = powf(u, logf(0.5f) / logf(m));
			u = apply_ease(u, s0->ease);

			mix_colors(coords[seg], coords[seg + 1], u, interp, c);
		}

		float rgb[3];
		from_space(c, interp, rgb);
		out[i * 4 + 0] = rgb[0];
		out[i * 4 + 1] = rgb[1];
		out[i * 4 + 2] = rgb[2];
		out[i * 4 + 3] = clamp01(c[3]);
	}
}

/* ------------------------------------------------------------------------
 * Stops in settings
 * --------------------------------------------------------------------- */

void gradient_default_stops(struct gradient_stop *stops, size_t *count)
{
	// A deep violet to warm orange: a pair that bands badly in sRGB
	// interpolation, and shows off Oklab.
	stops[0] = (struct gradient_stop){0.0f, 0xFFB0264Bu, 0.5f, GRADIENT_EASE_LINEAR};
	stops[1] = (struct gradient_stop){1.0f, 0xFF309BFFu, 0.5f, GRADIENT_EASE_LINEAR};
	*count = 2;
}

size_t gradient_read_stops(obs_data_t *settings, const char *prefix, struct gradient_stop *stops, size_t max_stops)
{
	char kbuf[128];
	obs_data_array_t *array = obs_data_get_array(settings, KEY("stops"));
	size_t count = array ? obs_data_array_count(array) : 0;

	if (count == 0) {
		obs_data_array_release(array);
		size_t n = 0;
		struct gradient_stop defaults[2];
		gradient_default_stops(defaults, &n);
		if (n > max_stops)
			n = max_stops;
		memcpy(stops, defaults, sizeof(struct gradient_stop) * n);
		return n;
	}

	if (count > max_stops)
		count = max_stops;
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(array, i);
		obs_data_set_default_double(item, "midpoint", 0.5);
		obs_data_set_default_int(item, "ease", GRADIENT_EASE_LINEAR);
		stops[i].position = clamp01((float)obs_data_get_double(item, "position"));
		stops[i].color = (uint32_t)obs_data_get_int(item, "color");
		stops[i].midpoint = clamp01((float)obs_data_get_double(item, "midpoint"));
		int ease = (int)obs_data_get_int(item, "ease");
		if (ease < GRADIENT_EASE_LINEAR || ease > GRADIENT_EASE_STEP)
			ease = GRADIENT_EASE_LINEAR;
		stops[i].ease = (enum gradient_ease)ease;
		obs_data_release(item);
	}
	obs_data_array_release(array);
	return count;
}

void gradient_write_stops(obs_data_t *settings, const char *prefix, const struct gradient_stop *stops, size_t count)
{
	char kbuf[128];
	obs_data_array_t *array = obs_data_array_create();
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_double(item, "position", stops[i].position);
		obs_data_set_int(item, "color", (long long)stops[i].color);
		obs_data_set_double(item, "midpoint", stops[i].midpoint);
		obs_data_set_int(item, "ease", stops[i].ease);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	obs_data_set_array(settings, KEY("stops"), array);
	obs_data_array_release(array);
}

/* ------------------------------------------------------------------------
 * Lifetime, defaults and update
 * --------------------------------------------------------------------- */

void gradient_init(gradient_t *g, obs_source_t *owner, const char *prefix)
{
	memset(g, 0, sizeof(*g));
	g->owner = owner;
	g->prefix = prefix;
	pthread_mutex_init(&g->mutex, NULL);
	g->lut_pending = bzalloc(sizeof(float) * 4 * GRADIENT_LUT_SIZE);
	g->shape = GRADIENT_SHAPE_LINEAR;
	g->interp = GRADIENT_INTERP_OKLAB;
	g->repeat = GRADIENT_REPEAT_PAD;
	g->scale = 1.0f;
	g->aspect = 1.0f;
	g->opacity = 1.0f;
	g->repetitions = 1;
	g->dither = true;
}

void gradient_free(gradient_t *g)
{
	obs_enter_graphics();
	if (g->lut_texture)
		gs_texture_destroy(g->lut_texture);
	if (g->effect)
		gs_effect_destroy(g->effect);
	if (g->fill_texrender)
		gs_texrender_destroy(g->fill_texrender);
	obs_leave_graphics();
	g->lut_texture = NULL;
	g->effect = NULL;
	g->fill_texrender = NULL;

	bfree(g->lut_pending);
	g->lut_pending = NULL;
	pthread_mutex_destroy(&g->mutex);
}

void gradient_defaults(obs_data_t *settings, const char *prefix)
{
	char kbuf[128];
	obs_data_set_default_int(settings, KEY("shape"), GRADIENT_SHAPE_LINEAR);
	obs_data_set_default_int(settings, KEY("interp"), GRADIENT_INTERP_OKLAB);
	obs_data_set_default_int(settings, KEY("repeat"), GRADIENT_REPEAT_PAD);
	obs_data_set_default_double(settings, KEY("center_x"), 50.0);
	obs_data_set_default_double(settings, KEY("center_y"), 50.0);
	obs_data_set_default_double(settings, KEY("angle"), 0.0);
	obs_data_set_default_double(settings, KEY("scale"), 100.0);
	obs_data_set_default_double(settings, KEY("aspect"), 100.0);
	obs_data_set_default_double(settings, KEY("focal_x"), 0.0);
	obs_data_set_default_double(settings, KEY("focal_y"), 0.0);
	obs_data_set_default_double(settings, KEY("twist"), 1.0);
	obs_data_set_default_int(settings, KEY("repetitions"), 1);
	obs_data_set_default_double(settings, KEY("offset"), 0.0);
	obs_data_set_default_double(settings, KEY("hue_shift"), 0.0);
	obs_data_set_default_bool(settings, KEY("corner_smooth"), true);
	obs_data_set_default_int(settings, KEY("corner_tl"), 0xFFB0264B);
	obs_data_set_default_int(settings, KEY("corner_tr"), 0xFF309BFF);
	obs_data_set_default_int(settings, KEY("corner_bl"), 0xFFE0A019);
	obs_data_set_default_int(settings, KEY("corner_br"), 0xFF5ADC9B);
	obs_data_set_default_double(settings, KEY("anim_rotate"), 0.0);
	obs_data_set_default_double(settings, KEY("anim_scroll"), 0.0);
	obs_data_set_default_double(settings, KEY("anim_hue"), 0.0);
	obs_data_set_default_double(settings, KEY("opacity"), 100.0);
	obs_data_set_default_double(settings, KEY("grain_amount"), 0.0);
	obs_data_set_default_double(settings, KEY("grain_size"), 1.0);
	obs_data_set_default_bool(settings, KEY("grain_animate"), true);
	obs_data_set_default_bool(settings, KEY("dither"), true);
}

void gradient_update(gradient_t *g, obs_data_t *settings)
{
	char kbuf[128];
	const char *prefix = g->prefix;

	g->shape = (enum gradient_shape)obs_data_get_int(settings, KEY("shape"));
	if (g->shape < GRADIENT_SHAPE_LINEAR || g->shape > GRADIENT_SHAPE_CORNERS)
		g->shape = GRADIENT_SHAPE_LINEAR;
	enum gradient_interp interp = (enum gradient_interp)obs_data_get_int(settings, KEY("interp"));
	if (interp < GRADIENT_INTERP_OKLAB || interp > GRADIENT_INTERP_SRGB)
		interp = GRADIENT_INTERP_OKLAB;
	g->interp = interp;
	g->repeat = (enum gradient_repeat)obs_data_get_int(settings, KEY("repeat"));
	if (g->repeat < GRADIENT_REPEAT_PAD || g->repeat > GRADIENT_REPEAT_MIRROR)
		g->repeat = GRADIENT_REPEAT_PAD;

	g->center_x = (float)obs_data_get_double(settings, KEY("center_x")) / 100.0f;
	g->center_y = (float)obs_data_get_double(settings, KEY("center_y")) / 100.0f;
	g->angle = (float)(obs_data_get_double(settings, KEY("angle")) * M_PI / 180.0);
	g->scale = fmaxf((float)obs_data_get_double(settings, KEY("scale")) / 100.0f, 0.001f);
	g->aspect = fmaxf((float)obs_data_get_double(settings, KEY("aspect")) / 100.0f, 0.01f);
	g->focal_x = (float)obs_data_get_double(settings, KEY("focal_x")) / 100.0f;
	g->focal_y = (float)obs_data_get_double(settings, KEY("focal_y")) / 100.0f;
	g->twist = (float)obs_data_get_double(settings, KEY("twist"));
	g->repetitions = (int)obs_data_get_int(settings, KEY("repetitions"));
	if (g->repetitions < 1)
		g->repetitions = 1;
	g->offset = (float)obs_data_get_double(settings, KEY("offset")) / 100.0f;
	g->hue_shift = (float)obs_data_get_double(settings, KEY("hue_shift"));
	g->corner_smooth = obs_data_get_bool(settings, KEY("corner_smooth"));
	g->corners[0] = (uint32_t)obs_data_get_int(settings, KEY("corner_tl"));
	g->corners[1] = (uint32_t)obs_data_get_int(settings, KEY("corner_tr"));
	g->corners[2] = (uint32_t)obs_data_get_int(settings, KEY("corner_bl"));
	g->corners[3] = (uint32_t)obs_data_get_int(settings, KEY("corner_br"));
	g->anim_rotate = (float)obs_data_get_double(settings, KEY("anim_rotate"));
	g->anim_scroll = (float)obs_data_get_double(settings, KEY("anim_scroll"));
	g->anim_hue = (float)obs_data_get_double(settings, KEY("anim_hue"));
	g->opacity = clamp01((float)obs_data_get_double(settings, KEY("opacity")) / 100.0f);
	g->grain_amount = clamp01((float)obs_data_get_double(settings, KEY("grain_amount")) / 100.0f);
	g->grain_size = fmaxf((float)obs_data_get_double(settings, KEY("grain_size")), 1.0f);
	g->grain_animate = obs_data_get_bool(settings, KEY("grain_animate"));
	g->dither = obs_data_get_bool(settings, KEY("dither"));

	struct gradient_stop stops[GRADIENT_MAX_STOPS];
	const size_t count = gradient_read_stops(settings, prefix, stops, GRADIENT_MAX_STOPS);

	// Only re-bake when the stops or blending changed: animated settings
	// can update a source every frame.
	uint64_t hash = 1469598103934665603ull;
	const uint8_t *bytes = (const uint8_t *)stops;
	for (size_t i = 0; i < sizeof(struct gradient_stop) * count; i++)
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	hash = (hash ^ (uint64_t)interp) * 1099511628211ull;
	hash = (hash ^ (uint64_t)count) * 1099511628211ull;

	pthread_mutex_lock(&g->mutex);
	if (!g->lut_baked || hash != g->lut_hash) {
		gradient_bake(stops, count, interp, g->lut_pending, GRADIENT_LUT_SIZE);
		g->lut_dirty = true;
		g->lut_baked = true;
		g->lut_hash = hash;
	}
	pthread_mutex_unlock(&g->mutex);
}

/* ------------------------------------------------------------------------
 * Properties
 * --------------------------------------------------------------------- */

static void set_visible(obs_properties_t *props, const char *name, bool visible)
{
	obs_property_t *p = obs_properties_get(props, name);
	if (p)
		obs_property_set_visible(p, visible);
}

void gradient_refresh_visibility(obs_properties_t *props, const char *prefix, obs_data_t *settings)
{
	char kbuf[128];
	const enum gradient_shape shape = (enum gradient_shape)obs_data_get_int(settings, KEY("shape"));

	const bool corners = shape == GRADIENT_SHAPE_CORNERS;
	const bool angular = shape == GRADIENT_SHAPE_CONIC || shape == GRADIENT_SHAPE_SPIRAL;
	const bool radial = shape == GRADIENT_SHAPE_RADIAL || shape == GRADIENT_SHAPE_ELLIPTICAL;
	const bool stretchable = radial || shape == GRADIENT_SHAPE_DIAMOND;

	set_visible(props, KEY("edit_stops"), !corners);
	set_visible(props, KEY("repeat"), !corners && !angular);
	set_visible(props, KEY("center_x"), !corners);
	set_visible(props, KEY("center_y"), !corners);
	set_visible(props, KEY("angle"), !corners);
	set_visible(props, KEY("scale"), !corners && !angular);
	set_visible(props, KEY("aspect"), stretchable);
	set_visible(props, KEY("focal_x"), radial);
	set_visible(props, KEY("focal_y"), radial);
	set_visible(props, KEY("twist"), shape == GRADIENT_SHAPE_SPIRAL);
	set_visible(props, KEY("repetitions"), angular);
	set_visible(props, KEY("offset"), !corners);
	set_visible(props, KEY("anim_scroll"), !corners);
	set_visible(props, KEY("anim_rotate"), !corners);
	set_visible(props, KEY("corner_tl"), corners);
	set_visible(props, KEY("corner_tr"), corners);
	set_visible(props, KEY("corner_bl"), corners);
	set_visible(props, KEY("corner_br"), corners);
	set_visible(props, KEY("corner_smooth"), corners);
}

void gradient_hide_geometry(obs_properties_t *props, const char *prefix)
{
	char kbuf[128];
	static const char *keys[] = {"shape",       "center_x",     "center_y",  "angle",     "scale",
				     "aspect",      "focal_x",      "focal_y",   "twist",     "repetitions",
				     "corner_tl",   "corner_tr",    "corner_bl", "corner_br", "corner_smooth",
				     "anim_rotate", "finish_group", NULL};
	for (size_t i = 0; keys[i]; i++)
		set_visible(props, KEY(keys[i]), false);
	set_visible(props, KEY("edit_stops"), true);
	set_visible(props, KEY("repeat"), true);
	set_visible(props, KEY("offset"), true);
	set_visible(props, KEY("anim_scroll"), true);
}

void gradient_set_visible(obs_properties_t *props, const char *prefix, bool visible)
{
	char kbuf[128];
	set_visible(props, KEY("group"), visible);
	set_visible(props, KEY("anim_group"), visible);
	set_visible(props, KEY("finish_group"), visible);
}

static bool shape_modified(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	gradient_refresh_visibility(props, (const char *)priv, settings);
	return true;
}

static bool edit_stops_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	if (!data)
		return false;
	gradient_editor_open((gradient_t *)data);
	// Refresh, as the editor may have changed the interpolation.
	return true;
}

static obs_property_t *add_float(obs_properties_t *group, const char *name, const char *text_id, double min, double max,
				 double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(group, name, obs_module_text(text_id), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

void gradient_properties(obs_properties_t *props, const char *prefix, gradient_t *g)
{
	gradient_properties_ex(props, prefix, g, 0);
}

void gradient_properties_ex(obs_properties_t *props, const char *prefix, gradient_t *g, uint32_t flags)
{
	char kbuf[128];
	obs_property_t *p;
	const bool map = (flags & GRADIENT_PROPS_MAP) != 0;

	/* Gradient */
	obs_properties_t *group = obs_properties_create();

	if (!map) {
		p = obs_properties_add_list(group, KEY("shape"), obs_module_text("Gradient.Shape"), OBS_COMBO_TYPE_LIST,
					    OBS_COMBO_FORMAT_INT);
		obs_property_list_add_int(p, obs_module_text("Gradient.Shape.Linear"), GRADIENT_SHAPE_LINEAR);
		obs_property_list_add_int(p, obs_module_text("Gradient.Shape.Reflected"), GRADIENT_SHAPE_REFLECTED);
		obs_property_list_add_int(p, obs_module_text("Gradient.Shape.Radial"), GRADIENT_SHAPE_RADIAL);
		obs_property_list_add_int(p, obs_module_text("Gradient.Shape.Elliptical"), GRADIENT_SHAPE_ELLIPTICAL);
		obs_property_list_add_int(p, obs_module_text("Gradient.Shape.Conic"), GRADIENT_SHAPE_CONIC);
		obs_property_list_add_int(p, obs_module_text("Gradient.Shape.Spiral"), GRADIENT_SHAPE_SPIRAL);
		obs_property_list_add_int(p, obs_module_text("Gradient.Shape.Diamond"), GRADIENT_SHAPE_DIAMOND);
		obs_property_list_add_int(p, obs_module_text("Gradient.Shape.Corners"), GRADIENT_SHAPE_CORNERS);
		obs_property_set_modified_callback2(p, shape_modified, (void *)prefix);
	}

	obs_properties_add_button2(group, KEY("edit_stops"), obs_module_text("Gradient.EditStops"), edit_stops_clicked,
				   g);

	p = obs_properties_add_list(group, KEY("interp"), obs_module_text("Gradient.Interpolation"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Gradient.Interpolation.Oklab"), GRADIENT_INTERP_OKLAB);
	obs_property_list_add_int(p, obs_module_text("Gradient.Interpolation.OklchShorter"),
				  GRADIENT_INTERP_OKLCH_SHORTER);
	obs_property_list_add_int(p, obs_module_text("Gradient.Interpolation.OklchLonger"),
				  GRADIENT_INTERP_OKLCH_LONGER);
	obs_property_list_add_int(p, obs_module_text("Gradient.Interpolation.OklchIncreasing"),
				  GRADIENT_INTERP_OKLCH_INCREASING);
	obs_property_list_add_int(p, obs_module_text("Gradient.Interpolation.OklchDecreasing"),
				  GRADIENT_INTERP_OKLCH_DECREASING);
	obs_property_list_add_int(p, obs_module_text("Gradient.Interpolation.LinearRgb"), GRADIENT_INTERP_LINEAR_RGB);
	obs_property_list_add_int(p, obs_module_text("Gradient.Interpolation.Srgb"), GRADIENT_INTERP_SRGB);
	obs_property_set_long_description(p, obs_module_text("Gradient.Interpolation.Description"));

	p = obs_properties_add_list(group, KEY("repeat"), obs_module_text("Gradient.Repeat"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Gradient.Repeat.Pad"), GRADIENT_REPEAT_PAD);
	obs_property_list_add_int(p, obs_module_text("Gradient.Repeat.Repeat"), GRADIENT_REPEAT_REPEAT);
	obs_property_list_add_int(p, obs_module_text("Gradient.Repeat.Mirror"), GRADIENT_REPEAT_MIRROR);

	if (map) {
		add_float(group, KEY("offset"), "Gradient.Offset", -100.0, 100.0, 0.1, "%");
		obs_properties_add_group(props, KEY("group"), obs_module_text("Gradient.Group"), OBS_GROUP_NORMAL,
					 group);
		obs_properties_t *anim = obs_properties_create();
		add_float(anim, KEY("anim_scroll"), "Gradient.Anim.Scroll", -5.0, 5.0, 0.01, "/s");
		add_float(anim, KEY("anim_hue"), "Gradient.Anim.Hue", -360.0, 360.0, 0.1, "°/s");
		add_float(anim, KEY("hue_shift"), "Gradient.HueShift", -180.0, 180.0, 0.1, "°");
		obs_properties_add_group(props, KEY("anim_group"), obs_module_text("Gradient.AnimGroup"),
					 OBS_GROUP_NORMAL, anim);
		return;
	}

	add_float(group, KEY("center_x"), "Gradient.CenterX", -100.0, 200.0, 0.1, "%");
	add_float(group, KEY("center_y"), "Gradient.CenterY", -100.0, 200.0, 0.1, "%");
	add_float(group, KEY("angle"), "Gradient.Angle", -360.0, 360.0, 0.1, "°");
	add_float(group, KEY("scale"), "Gradient.Scale", 1.0, 1000.0, 0.1, "%");
	add_float(group, KEY("aspect"), "Gradient.Aspect", 10.0, 1000.0, 0.1, "%");
	add_float(group, KEY("focal_x"), "Gradient.FocalX", -95.0, 95.0, 0.1, "%");
	add_float(group, KEY("focal_y"), "Gradient.FocalY", -95.0, 95.0, 0.1, "%");
	add_float(group, KEY("twist"), "Gradient.Twist", -10.0, 10.0, 0.01, NULL);
	obs_properties_add_int_slider(group, KEY("repetitions"), obs_module_text("Gradient.Repetitions"), 1, 32, 1);
	add_float(group, KEY("offset"), "Gradient.Offset", -100.0, 100.0, 0.1, "%");

	obs_properties_add_color_alpha(group, KEY("corner_tl"), obs_module_text("Gradient.Corner.TopLeft"));
	obs_properties_add_color_alpha(group, KEY("corner_tr"), obs_module_text("Gradient.Corner.TopRight"));
	obs_properties_add_color_alpha(group, KEY("corner_bl"), obs_module_text("Gradient.Corner.BottomLeft"));
	obs_properties_add_color_alpha(group, KEY("corner_br"), obs_module_text("Gradient.Corner.BottomRight"));
	obs_properties_add_bool(group, KEY("corner_smooth"), obs_module_text("Gradient.Corner.Smooth"));

	obs_properties_add_group(props, KEY("group"), obs_module_text("Gradient.Group"), OBS_GROUP_NORMAL, group);

	/* Animation */
	obs_properties_t *anim = obs_properties_create();
	add_float(anim, KEY("anim_rotate"), "Gradient.Anim.Rotate", -360.0, 360.0, 0.1, "°/s");
	add_float(anim, KEY("anim_scroll"), "Gradient.Anim.Scroll", -5.0, 5.0, 0.01, "/s");
	add_float(anim, KEY("anim_hue"), "Gradient.Anim.Hue", -360.0, 360.0, 0.1, "°/s");
	add_float(anim, KEY("hue_shift"), "Gradient.HueShift", -180.0, 180.0, 0.1, "°");
	obs_properties_add_group(props, KEY("anim_group"), obs_module_text("Gradient.AnimGroup"), OBS_GROUP_NORMAL,
				 anim);

	/* Finish */
	obs_properties_t *finish = obs_properties_create();
	add_float(finish, KEY("opacity"), "Gradient.Opacity", 0.0, 100.0, 0.1, "%");
	add_float(finish, KEY("grain_amount"), "Gradient.Grain", 0.0, 100.0, 0.1, "%");
	add_float(finish, KEY("grain_size"), "Gradient.GrainSize", 1.0, 16.0, 0.1, "px");
	obs_properties_add_bool(finish, KEY("grain_animate"), obs_module_text("Gradient.GrainAnimate"));
	p = obs_properties_add_bool(finish, KEY("dither"), obs_module_text("Gradient.Dither"));
	obs_property_set_long_description(p, obs_module_text("Gradient.Dither.Description"));
	obs_properties_add_group(props, KEY("finish_group"), obs_module_text("Gradient.FinishGroup"), OBS_GROUP_NORMAL,
				 finish);
}

/* ------------------------------------------------------------------------
 * Animation
 * --------------------------------------------------------------------- */

bool gradient_is_animated(const gradient_t *g)
{
	return g->anim_rotate != 0.0f || g->anim_scroll != 0.0f || g->anim_hue != 0.0f ||
	       (g->grain_amount > 0.0f && g->grain_animate);
}

void gradient_tick(gradient_t *g, float seconds)
{
	g->time_rotate = fmodf(g->time_rotate + g->anim_rotate * seconds * (float)(M_PI / 180.0), (float)(2.0 * M_PI));
	// Wrapped at 2 so both repeat and mirror stay continuous.
	g->time_scroll = fmodf(g->time_scroll + g->anim_scroll * seconds, 2.0f);
	g->time_hue = fmodf(g->time_hue + g->anim_hue * seconds, 360.0f);
	if (g->grain_animate && g->grain_amount > 0.0f)
		g->grain_seed = fmodf(g->grain_seed + 1.0f, 251.0f);
}

/* ------------------------------------------------------------------------
 * Rendering
 * --------------------------------------------------------------------- */

static bool ensure_lut(gradient_t *g)
{
	pthread_mutex_lock(&g->mutex);
	if (g->lut_dirty || !g->lut_texture) {
		if (!g->lut_texture) {
			const uint8_t *data = (const uint8_t *)g->lut_pending;
			g->lut_texture = gs_texture_create(GRADIENT_LUT_SIZE, 1, GS_RGBA32F, 1, &data, GS_DYNAMIC);
		} else {
			gs_texture_set_image(g->lut_texture, (const uint8_t *)g->lut_pending,
					     GRADIENT_LUT_SIZE * 4 * sizeof(float), false);
		}
		g->lut_dirty = false;
	}
	pthread_mutex_unlock(&g->mutex);
	return g->lut_texture != NULL;
}

static bool ensure_resources(gradient_t *g)
{
	if (!g->effect) {
		char *path = obs_module_file("shaders/gradient/gradient.effect");
		char *errors = NULL;
		g->effect = gs_effect_create_from_file(path, &errors);
		if (!g->effect)
			blog(LOG_WARNING, "[Spectrum] Unable to load gradient shader %s: %s", path ? path : "(null)",
			     errors ? errors : "(no errors)");
		bfree(errors);
		bfree(path);
		if (!g->effect)
			return false;
	}
	return ensure_lut(g);
}

static void set_float(gs_effect_t *e, const char *name, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p)
		gs_effect_set_float(p, v);
}

static void set_int(gs_effect_t *e, const char *name, int v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p)
		gs_effect_set_int(p, v);
}

static void set_vec2(gs_effect_t *e, const char *name, float x, float y)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p) {
		struct vec2 v;
		vec2_set(&v, x, y);
		gs_effect_set_vec2(p, &v);
	}
}

static void set_vec4(gs_effect_t *e, const char *name, const float c[4])
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p) {
		struct vec4 v;
		vec4_set(&v, c[0], c[1], c[2], c[3]);
		gs_effect_set_vec4(p, &v);
	}
}

static float farthest_corner(float cx, float cy, float w, float h)
{
	const float dx = fmaxf(cx, w - cx), dy = fmaxf(cy, h - cy);
	return sqrtf(dx * dx + dy * dy);
}

static void set_params(gradient_t *g, uint32_t width, uint32_t height, float dither_step)
{
	gs_effect_t *e = g->effect;
	const float w = (float)width, h = (float)height;
	const float cx = g->center_x * w, cy = g->center_y * h;
	const float angle = g->angle + g->time_rotate;
	const float ca = cosf(angle), sa = sinf(angle);
	const float sqrt_aspect = sqrtf(g->aspect);

	float inv_rx = 1.0f, inv_ry = 1.0f;
	switch (g->shape) {
	case GRADIENT_SHAPE_LINEAR: {
		const float len = fmaxf((fabsf(w * ca) + fabsf(h * sa)) * g->scale, 1e-3f);
		inv_rx = inv_ry = 1.0f / len;
		break;
	}
	case GRADIENT_SHAPE_REFLECTED: {
		const float len = fmaxf((fabsf(w * ca) + fabsf(h * sa)) * g->scale, 1e-3f);
		inv_rx = inv_ry = 2.0f / len;
		break;
	}
	case GRADIENT_SHAPE_RADIAL: {
		const float r = fmaxf(farthest_corner(cx, cy, w, h) * g->scale, 1e-3f);
		inv_rx = 1.0f / (r * sqrt_aspect);
		inv_ry = sqrt_aspect / r;
		break;
	}
	case GRADIENT_SHAPE_ELLIPTICAL: {
		const float rx = fmaxf(fmaxf(cx, w - cx) * (float)M_SQRT2 * g->scale, 1e-3f);
		const float ry = fmaxf(fmaxf(cy, h - cy) * (float)M_SQRT2 * g->scale, 1e-3f);
		inv_rx = 1.0f / (rx * sqrt_aspect);
		inv_ry = sqrt_aspect / ry;
		break;
	}
	case GRADIENT_SHAPE_DIAMOND: {
		const float rx = fmaxf(2.0f * fmaxf(cx, w - cx) * g->scale, 1e-3f);
		const float ry = fmaxf(2.0f * fmaxf(cy, h - cy) * g->scale, 1e-3f);
		inv_rx = 1.0f / (rx * sqrt_aspect);
		inv_ry = sqrt_aspect / ry;
		break;
	}
	default: {
		const float r = fmaxf(farthest_corner(cx, cy, w, h), 1e-3f);
		inv_rx = inv_ry = 1.0f / r;
		break;
	}
	}

	// Keep the focal point strictly inside the circle.
	float fx = g->focal_x, fy = g->focal_y;
	const float fl = sqrtf(fx * fx + fy * fy);
	if (fl > 0.95f) {
		fx *= 0.95f / fl;
		fy *= 0.95f / fl;
	}

	set_vec2(e, "size", w, h);
	set_vec2(e, "center", cx, cy);
	set_vec2(e, "axis", ca, sa);
	set_vec2(e, "inv_radius", inv_rx, inv_ry);
	set_vec2(e, "focal", fx, fy);
	set_int(e, "shape", (int)g->shape);
	// Conic and spiral wrap around by nature: never pad them.
	int repeat = (int)g->repeat;
	if ((g->shape == GRADIENT_SHAPE_CONIC || g->shape == GRADIENT_SHAPE_SPIRAL) && g->repeat == GRADIENT_REPEAT_PAD)
		repeat = GRADIENT_REPEAT_REPEAT;
	set_int(e, "repeat_mode", repeat);
	set_float(e, "offset", g->offset + g->time_scroll);
	set_float(e, "repetitions", (float)g->repetitions);
	set_float(e, "twist", g->twist);
	set_float(e, "hue_shift", (float)((g->hue_shift + g->time_hue) * M_PI / 180.0));
	set_float(e, "lut_size", (float)GRADIENT_LUT_SIZE);
	set_float(e, "opacity", g->opacity);
	set_float(e, "grain_amount", g->grain_amount * 0.2f);
	set_float(e, "grain_size", g->grain_size);
	set_float(e, "grain_seed", g->grain_seed);
	set_float(e, "dither_step", g->dither ? dither_step : 0.0f);

	// Corner colors, premultiplied in the interpolation space. The OkLCh
	// modes blend corners in Oklab (a hue path between four colors is
	// not well defined).
	enum gradient_interp corner_interp = is_lch(g->interp) ? GRADIENT_INTERP_OKLAB : g->interp;
	int corner_space =
		corner_interp == GRADIENT_INTERP_SRGB ? 0 : (corner_interp == GRADIENT_INTERP_LINEAR_RGB ? 1 : 2);
	set_int(e, "corner_space", corner_space);
	set_float(e, "corner_smooth", g->corner_smooth ? 1.0f : 0.0f);
	static const char *corner_names[4] = {"corner_tl", "corner_tr", "corner_bl", "corner_br"};
	for (int i = 0; i < 4; i++) {
		float c[4];
		to_space(g->corners[i], corner_interp, c);
		c[0] *= c[3];
		c[1] *= c[3];
		c[2] *= c[3];
		set_vec4(e, corner_names[i], c);
	}

	gs_eparam_t *p = gs_effect_get_param_by_name(e, "lut");
	if (p)
		gs_effect_set_texture(p, g->lut_texture);
}

// The size of one quantisation step of the current render target, in
// (sRGB encoded) output units, or 0 for float targets that don't band.
float spectrum_target_dither_step(void)
{
	gs_texture_t *target = gs_get_render_target();
	// No texture: a swap chain (preview, projector), which is 8-bit.
	enum gs_color_format format = target ? gs_texture_get_color_format(target) : GS_BGRA;
	switch (format) {
	case GS_R10G10B10A2:
		return 1.0f / 1023.0f;
	case GS_RGBA16:
		return 1.0f / 65535.0f;
	case GS_RGBA16F:
	case GS_RGBA32F:
	case GS_RG16F:
	case GS_RG32F:
	case GS_R16F:
	case GS_R32F:
		return 0.0f;
	default:
		return 1.0f / 255.0f;
	}
}

void gradient_draw(gradient_t *g, uint32_t width, uint32_t height)
{
	if (!width || !height || !ensure_resources(g))
		return;

	set_params(g, width, height, spectrum_target_dither_step());

	// Output linear values and let the sRGB framebuffer encode them, so
	// alpha blends in linear light (as OBS' color source does).
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	while (gs_effect_loop(g->effect, "Draw"))
		gs_draw_sprite(NULL, 0, width, height);
	gs_enable_framebuffer_srgb(previous);
}

gs_texture_t *gradient_render_fill(gradient_t *g, uint32_t width, uint32_t height)
{
	if (!width || !height || !ensure_resources(g))
		return NULL;

	if (!g->fill_texrender)
		g->fill_texrender = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	gs_texrender_reset(g->fill_texrender);

	// The fill is consumed by another shader in float, which dithers
	// its own output: no dithering here.
	set_params(g, width, height, 0.0f);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin(g->fill_texrender, width, height)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		while (gs_effect_loop(g->effect, "DrawFill"))
			gs_draw_sprite(NULL, 0, width, height);
		gs_texrender_end(g->fill_texrender);
	}
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);

	return gs_texrender_get_texture(g->fill_texrender);
}

bool gradient_bind_lut(gradient_t *g, gs_effect_t *effect)
{
	if (!ensure_lut(g))
		return false;
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, "gradient_lut");
	if (p)
		gs_effect_set_texture(p, g->lut_texture);
	set_float(effect, "gradient_lut_size", (float)GRADIENT_LUT_SIZE);
	set_float(effect, "gradient_offset", g->offset + g->time_scroll);
	set_float(effect, "gradient_repeat", (float)g->repeat);
	set_float(effect, "gradient_hue", (float)((g->hue_shift + g->time_hue) * M_PI / 180.0));
	return true;
}
