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
// Implemented in gradient-palette-dialog.cpp (Qt).
extern void gradient_palette_dialog_open(gradient_t *g);

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
 * Mesh and free points: settings
 * --------------------------------------------------------------------- */

#define RGB_COLOR(r, g, b) (0xFF000000u | ((uint32_t)(b) << 16) | ((uint32_t)(g) << 8) | (uint32_t)(r))

// Colors of the mesh points, row major. A smaller grid uses the top left
// part, so the default 3x3 mesh is the first three of the first three rows.
static const uint32_t mesh_default_colors[GRADIENT_MESH_POINTS] = {
	RGB_COLOR(75, 38, 176),   RGB_COLOR(230, 60, 140),  RGB_COLOR(255, 155, 48),  RGB_COLOR(255, 214, 102),
	RGB_COLOR(250, 246, 230), RGB_COLOR(40, 90, 220),   RGB_COLOR(180, 150, 240), RGB_COLOR(255, 210, 90),
	RGB_COLOR(255, 120, 90),  RGB_COLOR(200, 40, 90),   RGB_COLOR(25, 160, 170),  RGB_COLOR(90, 220, 155),
	RGB_COLOR(255, 170, 150), RGB_COLOR(120, 70, 200),  RGB_COLOR(30, 30, 90),    RGB_COLOR(20, 60, 110),
	RGB_COLOR(60, 180, 230),  RGB_COLOR(240, 240, 255), RGB_COLOR(250, 140, 200), RGB_COLOR(140, 40, 160),
	RGB_COLOR(10, 20, 40),    RGB_COLOR(30, 110, 140),  RGB_COLOR(150, 230, 200), RGB_COLOR(255, 200, 120),
	RGB_COLOR(255, 90, 60),
};

static const uint32_t point_default_colors[GRADIENT_POINTS_MAX] = {
	RGB_COLOR(255, 94, 98),   RGB_COLOR(255, 196, 112), RGB_COLOR(72, 92, 240),   RGB_COLOR(40, 200, 190),
	RGB_COLOR(250, 245, 235), RGB_COLOR(150, 60, 200),  RGB_COLOR(120, 230, 120), RGB_COLOR(20, 30, 70),
};

static const float point_default_pos[GRADIENT_POINTS_MAX][2] = {
	{20.0f, 25.0f}, {80.0f, 20.0f}, {25.0f, 80.0f}, {78.0f, 75.0f},
	{50.0f, 50.0f}, {8.0f, 55.0f},  {92.0f, 48.0f}, {50.0f, 92.0f},
};

#define MESH_DEFAULT_SIZE 3

static const char *mesh_key(char *buf, const char *prefix, int r, int c, const char *what)
{
	snprintf(buf, 128, "%smesh_%d_%d_%s", prefix, r, c, what);
	return buf;
}

static const char *point_key(char *buf, const char *prefix, int i, const char *what)
{
	snprintf(buf, 128, "%spoint_%d_%s", prefix, i, what);
	return buf;
}

static int clamp_int(int v, int lo, int hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static int mesh_cols_of(obs_data_t *settings, const char *prefix)
{
	char kbuf[128];
	return clamp_int((int)obs_data_get_int(settings, KEY("mesh_cols")), 2, GRADIENT_MESH_MAX);
}

static int mesh_rows_of(obs_data_t *settings, const char *prefix)
{
	char kbuf[128];
	return clamp_int((int)obs_data_get_int(settings, KEY("mesh_rows")), 2, GRADIENT_MESH_MAX);
}

// The stored mesh point positions belong to the grid size recorded in
// "mesh_layout" (cols * 10 + rows). After the grid size changes they are
// laid out again as a regular grid.
static bool mesh_layout_synced(obs_data_t *settings, const char *prefix)
{
	char kbuf[128];
	const int cols = mesh_cols_of(settings, prefix), rows = mesh_rows_of(settings, prefix);
	return obs_data_get_int(settings, KEY("mesh_layout")) == cols * 10 + rows;
}

static void mesh_write_regular(obs_data_t *settings, const char *prefix)
{
	char kbuf[128];
	const int cols = mesh_cols_of(settings, prefix), rows = mesh_rows_of(settings, prefix);
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			obs_data_set_double(settings, mesh_key(kbuf, prefix, r, c, "x"), 100.0 * c / (cols - 1));
			obs_data_set_double(settings, mesh_key(kbuf, prefix, r, c, "y"), 100.0 * r / (rows - 1));
		}
	}
	obs_data_set_int(settings, KEY("mesh_layout"), cols * 10 + rows);
}

static void mesh_points_defaults(obs_data_t *settings, const char *prefix)
{
	char kbuf[128];
	obs_data_set_default_int(settings, KEY("mesh_cols"), MESH_DEFAULT_SIZE);
	obs_data_set_default_int(settings, KEY("mesh_rows"), MESH_DEFAULT_SIZE);
	obs_data_set_default_int(settings, KEY("mesh_layout"), MESH_DEFAULT_SIZE * 10 + MESH_DEFAULT_SIZE);
	for (int r = 0; r < GRADIENT_MESH_MAX; r++) {
		for (int c = 0; c < GRADIENT_MESH_MAX; c++) {
			obs_data_set_default_int(settings, mesh_key(kbuf, prefix, r, c, "color"),
						 mesh_default_colors[r * GRADIENT_MESH_MAX + c]);
			obs_data_set_default_double(settings, mesh_key(kbuf, prefix, r, c, "x"),
						    100.0 * c / (MESH_DEFAULT_SIZE - 1));
			obs_data_set_default_double(settings, mesh_key(kbuf, prefix, r, c, "y"),
						    100.0 * r / (MESH_DEFAULT_SIZE - 1));
		}
	}
	obs_data_set_default_int(settings, KEY("point_count"), 4);
	obs_data_set_default_double(settings, KEY("point_softness"), 50.0);
	for (int i = 0; i < GRADIENT_POINTS_MAX; i++) {
		obs_data_set_default_int(settings, point_key(kbuf, prefix, i, "color"), point_default_colors[i]);
		obs_data_set_default_double(settings, point_key(kbuf, prefix, i, "x"), point_default_pos[i][0]);
		obs_data_set_default_double(settings, point_key(kbuf, prefix, i, "y"), point_default_pos[i][1]);
	}
	obs_data_set_default_double(settings, KEY("drift_amount"), 0.0);
	obs_data_set_default_double(settings, KEY("drift_speed"), 1.0);
}

static void mesh_points_update(gradient_t *g, obs_data_t *settings)
{
	char kbuf[128];
	const char *prefix = g->prefix;
	const int cols = mesh_cols_of(settings, prefix), rows = mesh_rows_of(settings, prefix);
	const bool synced = mesh_layout_synced(settings, prefix);

	uint32_t colors[GRADIENT_MESH_POINTS];
	float pos[GRADIENT_MESH_POINTS][2];
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			const int i = r * GRADIENT_MESH_MAX + c;
			colors[i] = (uint32_t)obs_data_get_int(settings, mesh_key(kbuf, prefix, r, c, "color"));
			if (synced) {
				pos[i][0] = (float)obs_data_get_double(settings, mesh_key(kbuf, prefix, r, c, "x")) /
					    100.0f;
				pos[i][1] = (float)obs_data_get_double(settings, mesh_key(kbuf, prefix, r, c, "y")) /
					    100.0f;
			} else {
				pos[i][0] = (float)c / (float)(cols - 1);
				pos[i][1] = (float)r / (float)(rows - 1);
			}
		}
	}

	const int count = clamp_int((int)obs_data_get_int(settings, KEY("point_count")), 2, GRADIENT_POINTS_MAX);
	uint32_t pcolors[GRADIENT_POINTS_MAX];
	float ppos[GRADIENT_POINTS_MAX][2];
	for (int i = 0; i < count; i++) {
		pcolors[i] = (uint32_t)obs_data_get_int(settings, point_key(kbuf, prefix, i, "color"));
		ppos[i][0] = (float)obs_data_get_double(settings, point_key(kbuf, prefix, i, "x")) / 100.0f;
		ppos[i][1] = (float)obs_data_get_double(settings, point_key(kbuf, prefix, i, "y")) / 100.0f;
	}

	pthread_mutex_lock(&g->mutex);
	g->mesh_cols = cols;
	g->mesh_rows = rows;
	memcpy(g->mesh_colors, colors, sizeof(colors));
	memcpy(g->mesh_pos, pos, sizeof(pos));
	g->point_count = count;
	memcpy(g->point_colors, pcolors, sizeof(uint32_t) * count);
	memcpy(g->point_pos, ppos, sizeof(float) * 2 * count);
	pthread_mutex_unlock(&g->mutex);

	g->point_softness = clamp01((float)obs_data_get_double(settings, KEY("point_softness")) / 100.0f);
	g->drift_amount = clamp01((float)obs_data_get_double(settings, KEY("drift_amount")) / 100.0f);
	g->drift_speed = (float)obs_data_get_double(settings, KEY("drift_speed"));
}

void gradient_apply_palette(obs_data_t *settings, const char *prefix, const uint32_t *colors, size_t count)
{
	char kbuf[128];
	if (!count)
		return;
	struct gradient_stop stops[GRADIENT_MAX_STOPS];
	const size_t n = gradient_palette_to_stops(colors, count, stops, GRADIENT_MAX_STOPS);
	gradient_write_stops(settings, prefix, stops, n);

	const int shape = (int)obs_data_get_int(settings, KEY("shape"));
	if (shape == GRADIENT_SHAPE_MESH) {
		// Along the diagonal: the first color in the top left corner,
		// the last in the bottom right.
		const int cols = mesh_cols_of(settings, prefix), rows = mesh_rows_of(settings, prefix);
		for (int r = 0; r < rows; r++) {
			for (int c = 0; c < cols; c++) {
				const float t = (float)(r + c) / (float)(rows + cols - 2);
				const size_t i = (size_t)lroundf(t * (float)(count - 1));
				obs_data_set_int(settings, mesh_key(kbuf, prefix, r, c, "color"), colors[i]);
			}
		}
	} else if (shape == GRADIENT_SHAPE_POINTS) {
		for (int i = 0; i < GRADIENT_POINTS_MAX; i++)
			obs_data_set_int(settings, point_key(kbuf, prefix, i, "color"), colors[(size_t)i % count]);
	}
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
	g->mesh_cols = g->mesh_rows = MESH_DEFAULT_SIZE;
	g->point_count = 2;
	g->drift_speed = 1.0f;
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
	gs_vertexbuffer_destroy(g->mesh_vb);
	gs_indexbuffer_destroy(g->mesh_ib);
	gs_texture_destroy(g->mesh_tex);
	gs_texrender_destroy(g->mesh_texrender);
	obs_leave_graphics();
	g->lut_texture = NULL;
	g->effect = NULL;
	g->fill_texrender = NULL;
	g->mesh_vb = NULL;
	g->mesh_ib = NULL;
	g->mesh_tex = NULL;
	g->mesh_texrender = NULL;

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
	mesh_points_defaults(settings, prefix);
}

void gradient_update(gradient_t *g, obs_data_t *settings)
{
	char kbuf[128];
	const char *prefix = g->prefix;

	g->shape = (enum gradient_shape)obs_data_get_int(settings, KEY("shape"));
	if (g->shape < GRADIENT_SHAPE_LINEAR || g->shape > GRADIENT_SHAPE_POINTS)
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
	mesh_points_update(g, settings);

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

	const bool mesh = shape == GRADIENT_SHAPE_MESH;
	const bool points = shape == GRADIENT_SHAPE_POINTS;
	// Shapes that place their colors themselves rather than through the
	// stops.
	const bool corners = shape == GRADIENT_SHAPE_CORNERS || mesh || points;
	const bool angular = shape == GRADIENT_SHAPE_CONIC || shape == GRADIENT_SHAPE_SPIRAL;
	const bool radial = shape == GRADIENT_SHAPE_RADIAL || shape == GRADIENT_SHAPE_ELLIPTICAL;
	const bool stretchable = radial || shape == GRADIENT_SHAPE_DIAMOND;

	set_visible(props, KEY("edit_stops"), !corners);
	set_visible(props, KEY("colors_from_source"), true);
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
	const bool four = shape == GRADIENT_SHAPE_CORNERS;
	set_visible(props, KEY("corner_tl"), four);
	set_visible(props, KEY("corner_tr"), four);
	set_visible(props, KEY("corner_bl"), four);
	set_visible(props, KEY("corner_br"), four);
	set_visible(props, KEY("corner_smooth"), four || mesh);

	set_visible(props, KEY("mesh_group"), mesh || points);
	set_visible(props, KEY("drift_amount"), mesh || points);
	set_visible(props, KEY("drift_speed"), mesh || points);
	set_visible(props, KEY("mesh_cols"), mesh);
	set_visible(props, KEY("mesh_rows"), mesh);
	set_visible(props, KEY("mesh_reset"), mesh);
	const int cols = mesh_cols_of(settings, prefix), rows = mesh_rows_of(settings, prefix);
	for (int r = 0; r < GRADIENT_MESH_MAX; r++) {
		for (int c = 0; c < GRADIENT_MESH_MAX; c++) {
			const bool on = mesh && r < rows && c < cols;
			set_visible(props, mesh_key(kbuf, prefix, r, c, "color"), on);
			set_visible(props, mesh_key(kbuf, prefix, r, c, "x"), on);
			set_visible(props, mesh_key(kbuf, prefix, r, c, "y"), on);
		}
	}
	set_visible(props, KEY("point_count"), points);
	set_visible(props, KEY("point_softness"), points);
	const int count = clamp_int((int)obs_data_get_int(settings, KEY("point_count")), 2, GRADIENT_POINTS_MAX);
	for (int i = 0; i < GRADIENT_POINTS_MAX; i++) {
		const bool on = points && i < count;
		set_visible(props, point_key(kbuf, prefix, i, "color"), on);
		set_visible(props, point_key(kbuf, prefix, i, "x"), on);
		set_visible(props, point_key(kbuf, prefix, i, "y"), on);
	}
}

void gradient_hide_geometry(obs_properties_t *props, const char *prefix)
{
	char kbuf[128];
	static const char *keys[] = {"shape",       "center_x",     "center_y",   "angle",        "scale",
				     "aspect",      "focal_x",      "focal_y",    "twist",        "repetitions",
				     "corner_tl",   "corner_tr",    "corner_bl",  "corner_br",    "corner_smooth",
				     "anim_rotate", "finish_group", "mesh_group", "drift_amount", "drift_speed",
				     NULL};
	for (size_t i = 0; keys[i]; i++)
		set_visible(props, KEY(keys[i]), false);
	set_visible(props, KEY("edit_stops"), true);
	set_visible(props, KEY("colors_from_source"), true);
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
	// Shown again by gradient_refresh_visibility for the mesh shapes.
	if (!visible)
		set_visible(props, KEY("mesh_group"), false);
}

static bool shape_modified(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	gradient_refresh_visibility(props, (const char *)priv, settings);
	return true;
}

// The grid size changed: lay the points out again as a regular grid.
static bool mesh_size_modified(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const char *prefix = (const char *)priv;
	if (!mesh_layout_synced(settings, prefix))
		mesh_write_regular(settings, prefix);
	gradient_refresh_visibility(props, prefix, settings);
	return true;
}

static bool mesh_reset_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	gradient_t *g = data;
	if (!g || !g->owner)
		return false;
	obs_data_t *settings = obs_source_get_settings(g->owner);
	mesh_write_regular(settings, g->prefix);
	obs_source_update(g->owner, NULL);
	obs_data_release(settings);
	return true;
}

static bool colors_from_source_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	if (!data)
		return false;
	gradient_palette_dialog_open((gradient_t *)data);
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

static void add_mesh_properties(obs_properties_t *props, const char *prefix, gradient_t *g)
{
	char kbuf[128];
	char text[128];
	obs_property_t *p;
	obs_properties_t *mesh = obs_properties_create();

	const char *size_keys[2] = {"mesh_cols", "mesh_rows"};
	const char *size_texts[2] = {"Gradient.Mesh.Columns", "Gradient.Mesh.Rows"};
	for (int k = 0; k < 2; k++) {
		p = obs_properties_add_list(mesh, KEY(size_keys[k]), obs_module_text(size_texts[k]),
					    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
		for (int n = 2; n <= GRADIENT_MESH_MAX; n++) {
			snprintf(text, sizeof(text), "%d", n);
			obs_property_list_add_int(p, text, n);
		}
		obs_property_set_modified_callback2(p, mesh_size_modified, (void *)prefix);
	}
	p = obs_properties_add_button2(mesh, KEY("mesh_reset"), obs_module_text("Gradient.Mesh.Reset"),
				       mesh_reset_clicked, g);
	obs_property_set_long_description(p, obs_module_text("Gradient.Mesh.Reset.Description"));

	const char *point_text = obs_module_text("Gradient.Mesh.Point");
	for (int r = 0; r < GRADIENT_MESH_MAX; r++) {
		for (int c = 0; c < GRADIENT_MESH_MAX; c++) {
			snprintf(text, sizeof(text), "%s %d,%d", point_text, r + 1, c + 1);
			obs_properties_add_color_alpha(mesh, mesh_key(kbuf, prefix, r, c, "color"), text);
			snprintf(text, sizeof(text), "%s %d,%d X", point_text, r + 1, c + 1);
			p = obs_properties_add_float_slider(mesh, mesh_key(kbuf, prefix, r, c, "x"), text, -50.0, 150.0,
							    0.1);
			obs_property_float_set_suffix(p, "%");
			snprintf(text, sizeof(text), "%s %d,%d Y", point_text, r + 1, c + 1);
			p = obs_properties_add_float_slider(mesh, mesh_key(kbuf, prefix, r, c, "y"), text, -50.0, 150.0,
							    0.1);
			obs_property_float_set_suffix(p, "%");
		}
	}

	p = obs_properties_add_int_slider(mesh, KEY("point_count"), obs_module_text("Gradient.Points.Count"), 2,
					  GRADIENT_POINTS_MAX, 1);
	obs_property_set_modified_callback2(p, shape_modified, (void *)prefix);
	p = add_float(mesh, KEY("point_softness"), "Gradient.Points.Softness", 0.0, 100.0, 0.1, "%");
	obs_property_set_long_description(p, obs_module_text("Gradient.Points.Softness.Description"));
	for (int i = 0; i < GRADIENT_POINTS_MAX; i++) {
		snprintf(text, sizeof(text), "%s %d", point_text, i + 1);
		obs_properties_add_color_alpha(mesh, point_key(kbuf, prefix, i, "color"), text);
		snprintf(text, sizeof(text), "%s %d X", point_text, i + 1);
		p = obs_properties_add_float_slider(mesh, point_key(kbuf, prefix, i, "x"), text, -50.0, 150.0, 0.1);
		obs_property_float_set_suffix(p, "%");
		snprintf(text, sizeof(text), "%s %d Y", point_text, i + 1);
		p = obs_properties_add_float_slider(mesh, point_key(kbuf, prefix, i, "y"), text, -50.0, 150.0, 0.1);
		obs_property_float_set_suffix(p, "%");
	}

	obs_properties_add_group(props, KEY("mesh_group"), obs_module_text("Gradient.MeshGroup"), OBS_GROUP_NORMAL,
				 mesh);
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
		obs_property_list_add_int(p, obs_module_text("Gradient.Shape.Mesh"), GRADIENT_SHAPE_MESH);
		obs_property_list_add_int(p, obs_module_text("Gradient.Shape.Points"), GRADIENT_SHAPE_POINTS);
		obs_property_set_modified_callback2(p, shape_modified, (void *)prefix);
	}

	obs_properties_add_button2(group, KEY("edit_stops"), obs_module_text("Gradient.EditStops"), edit_stops_clicked,
				   g);
	p = obs_properties_add_button2(group, KEY("colors_from_source"), obs_module_text("Gradient.ColorsFromSource"),
				       colors_from_source_clicked, g);
	obs_property_set_long_description(p, obs_module_text("Gradient.ColorsFromSource.Description"));

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

	/* Mesh and free points */
	add_mesh_properties(props, prefix, g);

	/* Animation */
	obs_properties_t *anim = obs_properties_create();
	add_float(anim, KEY("anim_rotate"), "Gradient.Anim.Rotate", -360.0, 360.0, 0.1, "°/s");
	add_float(anim, KEY("anim_scroll"), "Gradient.Anim.Scroll", -5.0, 5.0, 0.01, "/s");
	add_float(anim, KEY("anim_hue"), "Gradient.Anim.Hue", -360.0, 360.0, 0.1, "°/s");
	add_float(anim, KEY("hue_shift"), "Gradient.HueShift", -180.0, 180.0, 0.1, "°");
	add_float(anim, KEY("drift_amount"), "Gradient.Drift", 0.0, 100.0, 0.1, "%");
	add_float(anim, KEY("drift_speed"), "Gradient.DriftSpeed", 0.0, 10.0, 0.01, "x");
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

#define DRIFT_PERIOD 64.0f

bool gradient_is_animated(const gradient_t *g)
{
	const bool drifting = (g->shape == GRADIENT_SHAPE_MESH || g->shape == GRADIENT_SHAPE_POINTS) &&
			      g->drift_amount > 0.0f && g->drift_speed != 0.0f;
	return g->anim_rotate != 0.0f || g->anim_scroll != 0.0f || g->anim_hue != 0.0f ||
	       (g->grain_amount > 0.0f && g->grain_animate) || drifting;
}

void gradient_tick(gradient_t *g, float seconds)
{
	g->time_rotate = fmodf(g->time_rotate + g->anim_rotate * seconds * (float)(M_PI / 180.0), (float)(2.0 * M_PI));
	// Wrapped at 2 so both repeat and mirror stay continuous.
	g->time_scroll = fmodf(g->time_scroll + g->anim_scroll * seconds, 2.0f);
	g->time_hue = fmodf(g->time_hue + g->anim_hue * seconds, 360.0f);
	if (g->grain_animate && g->grain_amount > 0.0f)
		g->grain_seed = fmodf(g->grain_seed + 1.0f, 251.0f);
	// Every drift wave has a whole number of periods in DRIFT_PERIOD
	// seconds, so wrapping the time there is seamless.
	if (g->drift_amount > 0.0f) {
		g->time_drift = fmodf(g->time_drift + g->drift_speed * seconds, DRIFT_PERIOD);
		if (g->time_drift < 0.0f)
			g->time_drift += DRIFT_PERIOD;
	}
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

/* ------------------------------------------------------------------------
 * Mesh and free points: rendering
 *
 * The mesh is tessellated on the CPU into a fine grid of triangles whose
 * vertices carry the mesh parameter (u, v in grid units). The point
 * positions are interpolated with a Catmull-Rom spline (so the surface
 * passes through every point and a regular grid maps linearly), and the
 * outer ring is extended far outwards so the mesh always covers the whole
 * frame. The colors are evaluated per pixel from the parameter as a bicubic
 * Hermite patch (Catmull-Rom tangents inside, flat at the border), in float,
 * from a small table of colors and tangents.
 * --------------------------------------------------------------------- */

#define MESH_SEGMENTS 32 // tessellation steps per patch and axis
#define MESH_MAX_STEPS ((GRADIENT_MESH_MAX - 1) * MESH_SEGMENTS + 3)

static uint32_t hash_u32(uint32_t x)
{
	x ^= x >> 16;
	x *= 0x7feb352du;
	x ^= x >> 15;
	x *= 0x846ca68bu;
	x ^= x >> 16;
	return x;
}

// A slow smooth wave in -1..1 for the drift of point `index` along `axis`.
// It completes a whole number of cycles every DRIFT_PERIOD seconds.
static float drift_wave(int index, int axis, float time)
{
	const uint32_t h = hash_u32((uint32_t)(index * 2 + axis) * 2654435761u + 0x9e3779b9u);
	const float cycles = (float)(3 + h % 7);
	const float phase = (float)((h >> 8) & 0xFFFF) / 65536.0f * (float)(2.0 * M_PI);
	return sinf(time * (float)(2.0 * M_PI) * cycles / DRIFT_PERIOD + phase);
}

struct mesh_snapshot {
	int cols, rows;
	uint32_t colors[GRADIENT_MESH_POINTS];
	float pos[GRADIENT_MESH_POINTS][2]; // px, drift applied
};

static void mesh_snapshot(gradient_t *g, float w, float h, struct mesh_snapshot *m)
{
	memset(m, 0, sizeof(*m));
	pthread_mutex_lock(&g->mutex);
	m->cols = g->mesh_cols;
	m->rows = g->mesh_rows;
	memcpy(m->colors, g->mesh_colors, sizeof(m->colors));
	memcpy(m->pos, g->mesh_pos, sizeof(m->pos));
	pthread_mutex_unlock(&g->mutex);

	// Inner points drift freely, edge points slide along their edge and
	// the corners stay put, so the mesh keeps covering the frame.
	const float amount = g->drift_amount * 0.3f;
	for (int r = 0; r < m->rows; r++) {
		for (int c = 0; c < m->cols; c++) {
			const int i = r * GRADIENT_MESH_MAX + c;
			float x = m->pos[i][0], y = m->pos[i][1];
			if (amount > 0.0f) {
				if (c > 0 && c < m->cols - 1)
					x += amount / (float)(m->cols - 1) * drift_wave(i, 0, g->time_drift);
				if (r > 0 && r < m->rows - 1)
					y += amount / (float)(m->rows - 1) * drift_wave(i, 1, g->time_drift);
			}
			m->pos[i][0] = x * w;
			m->pos[i][1] = y * h;
		}
	}
}

static void add_tangent(float *w, int k, int n, float s)
{
	if (k == 0) {
		w[0] -= s;
		w[1] += s;
	} else if (k == n - 1) {
		w[n - 2] -= s;
		w[n - 1] += s;
	} else {
		w[k - 1] -= 0.5f * s;
		w[k + 1] += 0.5f * s;
	}
}

// Weights of n control values for the parameter t (in grid units) on a
// Catmull-Rom spline through them, whose end tangents point at the
// neighbour, extended linearly outside 0..n-1.
static void spline_weights(float t, int n, float *w)
{
	for (int i = 0; i < n; i++)
		w[i] = 0.0f;
	if (t <= 0.0f) {
		w[0] = 1.0f - t;
		w[1] = t;
		return;
	}
	if (t >= (float)(n - 1)) {
		const float s = t - (float)(n - 1);
		w[n - 1] = 1.0f + s;
		w[n - 2] = -s;
		return;
	}
	int i = (int)floorf(t);
	if (i > n - 2)
		i = n - 2;
	const float f = t - (float)i, f2 = f * f, f3 = f2 * f;
	w[i] += 2.0f * f3 - 3.0f * f2 + 1.0f;
	w[i + 1] += -2.0f * f3 + 3.0f * f2;
	add_tangent(w, i, n, f3 - 2.0f * f2 + f);
	add_tangent(w, i + 1, n, f3 - f2);
}

// The tessellation is an nu x nv grid of vertices: the mesh itself in
// 1..nu-2 / 1..nv-2, and a skirt ring around it (index 0 and nu-1 / nv-1)
// that reaches out along the border's outward normals, so the mesh covers
// the whole output whatever the edge points do.
static void mesh_fill_vertices(const struct mesh_snapshot *m, float w, float h, struct gs_vb_data *d)
{
	const int cols = m->cols, rows = m->rows;
	const int nu = (cols - 1) * MESH_SEGMENTS + 3, nv = (rows - 1) * MESH_SEGMENTS + 3;
#define MP(r, c) m->pos[(r) * GRADIENT_MESH_MAX + (c)]

	// Graphics thread only, so static scratch space is fine.
	static float wu[MESH_MAX_STEPS][GRADIENT_MESH_MAX], wv[MESH_MAX_STEPS][GRADIENT_MESH_MAX];
	static float rp[GRADIENT_MESH_MAX][MESH_MAX_STEPS][2];
	float us[MESH_MAX_STEPS], vs[MESH_MAX_STEPS];
	for (int a = 1; a < nu - 1; a++) {
		us[a] = (float)(a - 1) / (float)MESH_SEGMENTS;
		spline_weights(us[a], cols, wu[a]);
	}
	for (int b = 1; b < nv - 1; b++) {
		vs[b] = (float)(b - 1) / (float)MESH_SEGMENTS;
		spline_weights(vs[b], rows, wv[b]);
	}
	us[0] = us[1];
	us[nu - 1] = us[nu - 2];
	vs[0] = vs[1];
	vs[nv - 1] = vs[nv - 2];

	// Row splines first: rp[r][a] is row r evaluated at us[a].
	for (int r = 0; r < rows; r++) {
		for (int a = 1; a < nu - 1; a++) {
			float x = 0.0f, y = 0.0f;
			for (int c = 0; c < cols; c++) {
				x += wu[a][c] * MP(r, c)[0];
				y += wu[a][c] * MP(r, c)[1];
			}
			rp[r][a][0] = x;
			rp[r][a][1] = y;
		}
	}
#undef MP

	struct vec3 *pts = d->points;
	struct vec2 *param = d->tvarray[0].array;
#define V(a, b) pts[(size_t)(b) * nu + (a)]
	for (int b = 1; b < nv - 1; b++) {
		for (int a = 1; a < nu - 1; a++) {
			float x = 0.0f, y = 0.0f;
			for (int r = 0; r < rows; r++) {
				x += wv[b][r] * rp[r][a][0];
				y += wv[b][r] * rp[r][a][1];
			}
			vec3_set(&V(a, b), x, y, 0.0f);
		}
	}

	// Skirt: each border vertex pushed out along the normal of the border
	// (pointing away from the next vertex inwards).
	const float reach = 2.0f * (w + h);
	struct vec2 offs_top[MESH_MAX_STEPS], offs_bottom[MESH_MAX_STEPS];
	struct vec2 offs_left[MESH_MAX_STEPS], offs_right[MESH_MAX_STEPS];
	for (int pass = 0; pass < 4; pass++) {
		const bool along_u = pass < 2;
		const int count = along_u ? nu : nv;
		for (int k = 1; k < count - 1; k++) {
			const int k0 = k > 1 ? k - 1 : k, k1 = k < count - 2 ? k + 1 : k;
			struct vec3 base, prev, next, inner;
			if (pass == 0) { // top: b = 1
				base = V(k, 1), prev = V(k0, 1), next = V(k1, 1), inner = V(k, 2);
			} else if (pass == 1) { // bottom
				base = V(k, nv - 2), prev = V(k0, nv - 2), next = V(k1, nv - 2), inner = V(k, nv - 3);
			} else if (pass == 2) { // left: a = 1
				base = V(1, k), prev = V(1, k0), next = V(1, k1), inner = V(2, k);
			} else { // right
				base = V(nu - 2, k), prev = V(nu - 2, k0), next = V(nu - 2, k1), inner = V(nu - 3, k);
			}
			float nx = -(next.y - prev.y), ny = next.x - prev.x;
			const float len = hypotf(nx, ny);
			if (len > 1e-6f) {
				nx /= len;
				ny /= len;
			} else {
				nx = base.x - inner.x;
				ny = base.y - inner.y;
				const float l2 = fmaxf(hypotf(nx, ny), 1e-6f);
				nx /= l2;
				ny /= l2;
			}
			if (nx * (base.x - inner.x) + ny * (base.y - inner.y) < 0.0f) {
				nx = -nx;
				ny = -ny;
			}
			struct vec2 *o = pass == 0 ? offs_top
						   : (pass == 1 ? offs_bottom : (pass == 2 ? offs_left : offs_right));
			vec2_set(&o[k], nx * reach, ny * reach);
		}
	}
	for (int a = 1; a < nu - 1; a++) {
		vec3_set(&V(a, 0), V(a, 1).x + offs_top[a].x, V(a, 1).y + offs_top[a].y, 0.0f);
		vec3_set(&V(a, nv - 1), V(a, nv - 2).x + offs_bottom[a].x, V(a, nv - 2).y + offs_bottom[a].y, 0.0f);
	}
	for (int b = 1; b < nv - 1; b++) {
		vec3_set(&V(0, b), V(1, b).x + offs_left[b].x, V(1, b).y + offs_left[b].y, 0.0f);
		vec3_set(&V(nu - 1, b), V(nu - 2, b).x + offs_right[b].x, V(nu - 2, b).y + offs_right[b].y, 0.0f);
	}
	vec3_set(&V(0, 0), V(1, 1).x + offs_top[1].x + offs_left[1].x, V(1, 1).y + offs_top[1].y + offs_left[1].y,
		 0.0f);
	vec3_set(&V(nu - 1, 0), V(nu - 2, 1).x + offs_top[nu - 2].x + offs_right[1].x,
		 V(nu - 2, 1).y + offs_top[nu - 2].y + offs_right[1].y, 0.0f);
	vec3_set(&V(0, nv - 1), V(1, nv - 2).x + offs_bottom[1].x + offs_left[nv - 2].x,
		 V(1, nv - 2).y + offs_bottom[1].y + offs_left[nv - 2].y, 0.0f);
	vec3_set(&V(nu - 1, nv - 1), V(nu - 2, nv - 2).x + offs_bottom[nu - 2].x + offs_right[nv - 2].x,
		 V(nu - 2, nv - 2).y + offs_bottom[nu - 2].y + offs_right[nv - 2].y, 0.0f);
#undef V

	// The skirt carries the parameter of its border vertex: the border
	// colors extend outwards.
	for (int b = 0; b < nv; b++)
		for (int a = 0; a < nu; a++)
			vec2_set(&param[(size_t)b * nu + a], us[a], vs[b]);
}

static void mesh_destroy_buffers(gradient_t *g)
{
	gs_vertexbuffer_destroy(g->mesh_vb);
	gs_indexbuffer_destroy(g->mesh_ib);
	gs_texture_destroy(g->mesh_tex);
	gs_texrender_destroy(g->mesh_texrender);
	g->mesh_vb = NULL;
	g->mesh_ib = NULL;
	g->mesh_tex = NULL;
	g->mesh_texrender = NULL;
}

static void push_quad(uint32_t *indices, size_t *n, int nu, int a, int b)
{
	const uint32_t i0 = (uint32_t)(b * nu + a), i1 = i0 + 1, i2 = i0 + (uint32_t)nu, i3 = i2 + 1;
	indices[(*n)++] = i0;
	indices[(*n)++] = i1;
	indices[(*n)++] = i2;
	indices[(*n)++] = i1;
	indices[(*n)++] = i3;
	indices[(*n)++] = i2;
}

static bool mesh_build_geometry(gradient_t *g, const struct mesh_snapshot *m, float w, float h)
{
	const int nu = (m->cols - 1) * MESH_SEGMENTS + 3, nv = (m->rows - 1) * MESH_SEGMENTS + 3;
	const size_t num = (size_t)nu * nv;

	if (g->mesh_vb) {
		struct gs_vb_data *d = gs_vertexbuffer_get_data(g->mesh_vb);
		mesh_fill_vertices(m, w, h, d);
		gs_vertexbuffer_flush(g->mesh_vb);
		return g->mesh_ib != NULL;
	}

	struct gs_vb_data *d = gs_vbdata_create();
	d->num = num;
	d->points = bmalloc(sizeof(struct vec3) * num);
	d->num_tex = 1;
	d->tvarray = bzalloc(sizeof(struct gs_tvertarray));
	d->tvarray[0].width = 2;
	d->tvarray[0].array = bmalloc(sizeof(struct vec2) * num);
	mesh_fill_vertices(m, w, h, d);
	g->mesh_vb = gs_vertexbuffer_create(d, GS_DYNAMIC);
	if (!g->mesh_vb)
		return false;

	// The skirt first, so the mesh is drawn over it where they overlap.
	const size_t index_count = (size_t)(nu - 1) * (nv - 1) * 6;
	uint32_t *indices = bmalloc(sizeof(uint32_t) * index_count);
	size_t n = 0;
	for (int pass = 0; pass < 2; pass++) {
		for (int b = 0; b < nv - 1; b++) {
			for (int a = 0; a < nu - 1; a++) {
				const bool skirt = a == 0 || b == 0 || a == nu - 2 || b == nv - 2;
				if (skirt == (pass == 0))
					push_quad(indices, &n, nu, a, b);
			}
		}
	}
	g->mesh_ib = gs_indexbuffer_create(GS_UNSIGNED_LONG, indices, index_count, 0);
	return g->mesh_ib != NULL;
}

// The interpolation space of the corner, mesh and point shapes: the OkLCh
// modes blend in Oklab (a hue path between many colors is not well
// defined).
static enum gradient_interp blend_space(const gradient_t *g)
{
	return is_lch(g->interp) ? GRADIENT_INTERP_OKLAB : g->interp;
}

static void to_space_premul(uint32_t color, enum gradient_interp space, float out[4])
{
	to_space(color, space, out);
	out[0] *= out[3];
	out[1] *= out[3];
	out[2] *= out[3];
}

// Uploads the colors and their tangents: a cols x (rows * 4) float texture,
// with blocks of rows for the value, d/du, d/dv and d2/dudv.
static bool mesh_upload_colors(gradient_t *g, const struct mesh_snapshot *m)
{
	const int cols = m->cols, rows = m->rows;
	const enum gradient_interp space = blend_space(g);
	float cv[GRADIENT_MESH_MAX][GRADIENT_MESH_MAX][4];
	float du[GRADIENT_MESH_MAX][GRADIENT_MESH_MAX][4];
	float dv[GRADIENT_MESH_MAX][GRADIENT_MESH_MAX][4];
	float duv[GRADIENT_MESH_MAX][GRADIENT_MESH_MAX][4];

	for (int r = 0; r < rows; r++)
		for (int c = 0; c < cols; c++)
			to_space_premul(m->colors[r * GRADIENT_MESH_MAX + c], space, cv[r][c]);
	// Catmull-Rom tangents inside, flat at the border (so a 2x2 mesh is
	// the smooth four corner blend).
	for (int r = 0; r < rows; r++) {
		for (int c = 0; c < cols; c++) {
			for (int k = 0; k < 4; k++) {
				du[r][c][k] = (c > 0 && c < cols - 1) ? 0.5f * (cv[r][c + 1][k] - cv[r][c - 1][k])
								      : 0.0f;
				dv[r][c][k] = (r > 0 && r < rows - 1) ? 0.5f * (cv[r + 1][c][k] - cv[r - 1][c][k])
								      : 0.0f;
			}
		}
	}
	for (int r = 0; r < rows; r++)
		for (int c = 0; c < cols; c++)
			for (int k = 0; k < 4; k++)
				duv[r][c][k] = (r > 0 && r < rows - 1) ? 0.5f * (du[r + 1][c][k] - du[r - 1][c][k])
								       : 0.0f;

	float data[GRADIENT_MESH_POINTS * 4 * 4];
	float(*blocks[4])[GRADIENT_MESH_MAX][4] = {cv, du, dv, duv};
	for (int b = 0; b < 4; b++)
		for (int r = 0; r < rows; r++)
			for (int c = 0; c < cols; c++)
				memcpy(&data[(((size_t)b * rows + r) * cols + c) * 4], blocks[b][r][c],
				       sizeof(float) * 4);

	const uint8_t *ptr = (const uint8_t *)data;
	if (!g->mesh_tex)
		g->mesh_tex = gs_texture_create((uint32_t)cols, (uint32_t)rows * 4, GS_RGBA32F, 1, &ptr, GS_DYNAMIC);
	else
		gs_texture_set_image(g->mesh_tex, ptr, (uint32_t)cols * 4 * sizeof(float), false);
	return g->mesh_tex != NULL;
}

static uint64_t fnv(uint64_t hash, const void *data, size_t size)
{
	const uint8_t *bytes = data;
	for (size_t i = 0; i < size; i++)
		hash = (hash ^ bytes[i]) * 1099511628211ull;
	return hash;
}

// Renders the mesh colors (linear, straight alpha, float) into an image of
// the output's size, when anything changed, and binds it. Graphics thread.
static bool mesh_prepare(gradient_t *g, uint32_t width, uint32_t height)
{
	const float w = (float)width, h = (float)height;
	struct mesh_snapshot m;
	mesh_snapshot(g, w, h, &m);

	if (g->mesh_vb && (g->mesh_vb_cols != m.cols || g->mesh_vb_rows != m.rows))
		mesh_destroy_buffers(g);

	uint64_t hash = 1469598103934665603ull;
	hash = fnv(hash, &m, sizeof(m));
	hash = fnv(hash, &width, sizeof(width));
	hash = fnv(hash, &height, sizeof(height));
	const int space = (int)blend_space(g);
	hash = fnv(hash, &space, sizeof(space));
	const int smooth = g->corner_smooth ? 1 : 0;
	hash = fnv(hash, &smooth, sizeof(smooth));

	if (!g->mesh_vb || !g->mesh_ib || !g->mesh_tex || !g->mesh_texrender || hash != g->mesh_hash) {
		if (!mesh_build_geometry(g, &m, w, h) || !mesh_upload_colors(g, &m)) {
			mesh_destroy_buffers(g);
			return false;
		}
		g->mesh_vb_cols = m.cols;
		g->mesh_vb_rows = m.rows;

		if (!g->mesh_texrender)
			g->mesh_texrender = gs_texrender_create(GS_RGBA32F, GS_ZS_NONE);
		gs_texrender_reset(g->mesh_texrender);

		gs_eparam_t *p = gs_effect_get_param_by_name(g->effect, "mesh_data");
		if (p)
			gs_effect_set_texture(p, g->mesh_tex);
		set_vec2(g->effect, "mesh_dims", (float)m.cols, (float)m.rows);
		set_float(g->effect, "mesh_smooth", (float)smooth);
		set_int(g->effect, "corner_space",
			space == GRADIENT_INTERP_SRGB ? 0 : (space == GRADIENT_INTERP_LINEAR_RGB ? 1 : 2));

		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
		const enum gs_cull_mode cull = gs_get_cull_mode();
		gs_set_cull_mode(GS_NEITHER);
		bool ok = false;
		if (gs_texrender_begin(g->mesh_texrender, width, height)) {
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
			gs_ortho(0.0f, w, 0.0f, h, -100.0f, 100.0f);
			gs_load_vertexbuffer(g->mesh_vb);
			gs_load_indexbuffer(g->mesh_ib);
			while (gs_effect_loop(g->effect, "MeshColor"))
				gs_draw(GS_TRIS, 0, 0);
			gs_load_indexbuffer(NULL);
			gs_load_vertexbuffer(NULL);
			gs_texrender_end(g->mesh_texrender);
			ok = true;
		}
		gs_set_cull_mode(cull);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous);
		if (!ok)
			return false;
		g->mesh_hash = hash;
	}

	gs_eparam_t *p = gs_effect_get_param_by_name(g->effect, "mesh_image");
	if (p)
		gs_effect_set_texture(p, gs_texrender_get_texture(g->mesh_texrender));
	return true;
}

static void points_set_params(gradient_t *g, float w, float h)
{
	gs_effect_t *e = g->effect;
	int count;
	uint32_t colors[GRADIENT_POINTS_MAX];
	float pos[GRADIENT_POINTS_MAX][2];
	pthread_mutex_lock(&g->mutex);
	count = g->point_count;
	memcpy(colors, g->point_colors, sizeof(colors));
	memcpy(pos, g->point_pos, sizeof(pos));
	pthread_mutex_unlock(&g->mutex);

	const enum gradient_interp space = blend_space(g);
	const float amount = g->drift_amount * 0.12f;
	char name[32];
	for (int i = 0; i < GRADIENT_POINTS_MAX; i++) {
		float c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
		float x = 0.0f, y = 0.0f;
		if (i < count) {
			to_space_premul(colors[i], space, c);
			x = pos[i][0];
			y = pos[i][1];
			if (amount > 0.0f) {
				x += amount * drift_wave(100 + i, 0, g->time_drift);
				y += amount * drift_wave(100 + i, 1, g->time_drift);
			}
		}
		snprintf(name, sizeof(name), "point_color%d", i);
		set_vec4(e, name, c);
		snprintf(name, sizeof(name), "point_pos%d", i);
		set_vec2(e, name, x * w, y * h);
	}
	set_int(e, "point_count", count);
	// Softness sets the falloff of the inverse distance weights: high
	// powers give nearly flat cells, low ones wide, soft blends.
	const float hard = 1.0f - g->point_softness;
	set_float(e, "point_power", 2.0f + 6.0f * hard * hard);
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
	enum gradient_interp corner_interp = blend_space(g);
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

	// Every uniform must be set, whichever shape is drawn. The mesh ones
	// are set again by mesh_prepare.
	points_set_params(g, w, h);
	set_vec2(e, "mesh_dims", 2.0f, 2.0f);
	set_float(e, "mesh_smooth", 0.0f);
	gs_eparam_t *image = gs_effect_get_param_by_name(e, "mesh_image");
	if (image && g->mesh_texrender)
		gs_effect_set_texture(image, gs_texrender_get_texture(g->mesh_texrender));
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

	// The mesh image is rendered before the parameters of this draw are
	// set (it uses some of the same ones).
	const bool mesh = g->shape == GRADIENT_SHAPE_MESH;
	if (mesh && !mesh_prepare(g, width, height))
		return;
	set_params(g, width, height, spectrum_target_dither_step());

	// Output linear values and let the sRGB framebuffer encode them, so
	// alpha blends in linear light (as OBS' color source does).
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	while (gs_effect_loop(g->effect, mesh ? "DrawMeshImage" : "Draw"))
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

	const bool mesh = g->shape == GRADIENT_SHAPE_MESH;
	if (mesh && !mesh_prepare(g, width, height))
		return NULL;

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
		while (gs_effect_loop(g->effect, mesh ? "DrawMeshImageFill" : "DrawFill"))
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
