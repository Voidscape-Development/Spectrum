// Colors from Source: reads a downscaled frame of a source back from the
// GPU and finds its dominant colors (k-means in Oklab), to build gradient
// stops from.

#include "gradient.h"

#include <graphics/vec4.h>
#include <util/bmem.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ------------------------------------------------------------------------
 * Capture
 * --------------------------------------------------------------------- */

uint8_t *gradient_palette_capture(obs_source_t *source, uint32_t max_size, uint32_t *width, uint32_t *height)
{
	if (!source || !max_size)
		return NULL;
	const uint32_t w = obs_source_get_width(source), h = obs_source_get_height(source);
	if (!w || !h)
		return NULL;

	const float scale = fminf(1.0f, (float)max_size / (float)(w > h ? w : h));
	uint32_t sw = (uint32_t)((float)w * scale + 0.5f), sh = (uint32_t)((float)h * scale + 0.5f);
	if (!sw)
		sw = 1;
	if (!sh)
		sh = 1;

	uint8_t *pixels = NULL;
	obs_enter_graphics();
	gs_texrender_t *texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_stagesurf_t *stage = gs_stagesurface_create(sw, sh, GS_RGBA);
	if (texrender && stage && gs_texrender_begin_with_color_space(texrender, sw, sh, GS_CS_SRGB)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(source);
		gs_blend_state_pop();
		gs_texrender_end(texrender);

		gs_stage_texture(stage, gs_texrender_get_texture(texrender));
		uint8_t *data;
		uint32_t linesize;
		if (gs_stagesurface_map(stage, &data, &linesize)) {
			pixels = bmalloc((size_t)sw * sh * 4);
			for (uint32_t y = 0; y < sh; y++)
				memcpy(pixels + (size_t)y * sw * 4, data + (size_t)y * linesize, (size_t)sw * 4);
			gs_stagesurface_unmap(stage);
		}
	}
	gs_stagesurface_destroy(stage);
	gs_texrender_destroy(texrender);
	obs_leave_graphics();

	if (pixels) {
		// Blended over transparent black: undo the premultiplication.
		for (size_t i = 0; i < (size_t)sw * sh; i++) {
			uint8_t *p = pixels + i * 4;
			if (p[3] > 0 && p[3] < 255) {
				for (int k = 0; k < 3; k++) {
					const unsigned v = (unsigned)p[k] * 255u / p[3];
					p[k] = (uint8_t)(v > 255u ? 255u : v);
				}
			}
		}
		*width = sw;
		*height = sh;
	}
	return pixels;
}

/* ------------------------------------------------------------------------
 * Extraction
 * --------------------------------------------------------------------- */

#define PALETTE_MAX_COLORS 16
#define PALETTE_MAX_SAMPLES 24576
#define PALETTE_ITERATIONS 16

struct lab {
	float l, a, b;
};

static float srgb_lut[256];

static void init_srgb_lut(void)
{
	static bool done = false;
	if (done)
		return;
	for (int i = 0; i < 256; i++)
		srgb_lut[i] = gradient_srgb_to_linear((float)i / 255.0f);
	done = true;
}

static struct lab rgb8_to_lab(const uint8_t *p)
{
	const float r = srgb_lut[p[0]], g = srgb_lut[p[1]], b = srgb_lut[p[2]];
	const float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
	const float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
	const float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
	struct lab out = {
		0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
		1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
		0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s,
	};
	return out;
}

static uint32_t lab_to_color(struct lab c)
{
	const float l_ = c.l + 0.3963377774f * c.a + 0.2158037573f * c.b;
	const float m_ = c.l - 0.1055613458f * c.a - 0.0638541728f * c.b;
	const float s_ = c.l - 0.0894841775f * c.a - 1.2914855480f * c.b;
	const float l = l_ * l_ * l_, m = m_ * m_ * m_, s = s_ * s_ * s_;
	const float rgb[3] = {
		4.0767416621f * l - 3.3077115913f * m + 0.2309699292f * s,
		-1.2684380046f * l + 2.6097574011f * m - 0.3413193965f * s,
		-0.0041960863f * l - 0.7034186147f * m + 1.7076147010f * s,
	};
	uint32_t out = 0xFF000000u;
	for (int i = 0; i < 3; i++) {
		float v = gradient_linear_to_srgb(rgb[i] < 0.0f ? 0.0f : (rgb[i] > 1.0f ? 1.0f : rgb[i]));
		out |= (uint32_t)lroundf(v * 255.0f) << (8 * i);
	}
	return out;
}

static float lab_dist2(struct lab x, struct lab y)
{
	const float dl = x.l - y.l, da = x.a - y.a, db = x.b - y.b;
	return dl * dl + da * da + db * db;
}

// Deterministic xorshift, so the same frame always gives the same palette.
static uint32_t next_random(uint32_t *state)
{
	uint32_t x = *state;
	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	return *state = x;
}

struct cluster {
	struct lab center;
	float weight;
};

static int compare_lightness(const void *a, const void *b)
{
	const struct cluster *x = a, *y = b;
	return (x->center.l > y->center.l) - (x->center.l < y->center.l);
}

static int compare_weight(const void *a, const void *b)
{
	const struct cluster *x = a, *y = b;
	return (x->weight < y->weight) - (x->weight > y->weight);
}

static float hue_of(struct lab c)
{
	float h = atan2f(c.b, c.a) * (float)(180.0 / M_PI);
	return h < 0.0f ? h + 360.0f : h;
}

static int compare_hue_key(const void *a, const void *b)
{
	const float x = *(const float *)a, y = *(const float *)b;
	return (x > y) - (x < y);
}

// Around the hue wheel, starting after its biggest empty gap so the colors
// run in one sweep. Near-greys (no meaningful hue) go first, dark to light.
static void sort_by_hue(struct cluster *c, size_t n)
{
	const float grey = 0.035f;
	struct cluster greys[PALETTE_MAX_COLORS], hued[PALETTE_MAX_COLORS];
	size_t ng = 0, nh = 0;
	for (size_t i = 0; i < n; i++) {
		if (hypotf(c[i].center.a, c[i].center.b) < grey)
			greys[ng++] = c[i];
		else
			hued[nh++] = c[i];
	}
	qsort(greys, ng, sizeof(struct cluster), compare_lightness);

	// Sort the hued colors by hue (pairs of hue and index).
	float keys[PALETTE_MAX_COLORS][2];
	for (size_t i = 0; i < nh; i++) {
		keys[i][0] = hue_of(hued[i].center);
		keys[i][1] = (float)i;
	}
	qsort(keys, nh, sizeof(keys[0]), compare_hue_key);
	size_t start = 0;
	float gap = -1.0f;
	for (size_t i = 0; i < nh; i++) {
		const float next = i + 1 < nh ? keys[i + 1][0] : keys[0][0] + 360.0f;
		if (next - keys[i][0] > gap) {
			gap = next - keys[i][0];
			start = (i + 1) % nh;
		}
	}

	size_t k = 0;
	for (size_t i = 0; i < ng; i++)
		c[k++] = greys[i];
	for (size_t i = 0; i < nh; i++)
		c[k++] = hued[(size_t)keys[(start + i) % nh][1]];
}

size_t gradient_palette_extract(const uint8_t *rgba, uint32_t width, uint32_t height, uint32_t linesize,
				size_t max_colors, enum gradient_palette_order order, uint32_t *colors, float *weights)
{
	if (!rgba || !width || !height || !max_colors)
		return 0;
	if (max_colors > PALETTE_MAX_COLORS)
		max_colors = PALETTE_MAX_COLORS;
	init_srgb_lut();

	// Opaque enough pixels, subsampled evenly to a bounded count.
	const size_t total = (size_t)width * height;
	const size_t step = total > PALETTE_MAX_SAMPLES ? (total + PALETTE_MAX_SAMPLES - 1) / PALETTE_MAX_SAMPLES : 1;
	struct lab *samples = bmalloc(sizeof(struct lab) * (total / step + 1));
	size_t n = 0;
	for (size_t i = 0; i < total; i += step) {
		const uint8_t *p = rgba + (i / width) * linesize + (i % width) * 4;
		if (p[3] < 128)
			continue;
		samples[n++] = rgb8_to_lab(p);
	}
	if (n == 0) {
		bfree(samples);
		return 0;
	}

	// k-means++ seeding.
	size_t k = max_colors < n ? max_colors : n;
	struct cluster clusters[PALETTE_MAX_COLORS];
	float *dist = bmalloc(sizeof(float) * n);
	uint32_t seed = 0x2545F491u;
	clusters[0].center = samples[next_random(&seed) % n];
	for (size_t i = 0; i < n; i++)
		dist[i] = lab_dist2(samples[i], clusters[0].center);
	size_t seeded = 1;
	while (seeded < k) {
		double sum = 0.0;
		for (size_t i = 0; i < n; i++)
			sum += dist[i];
		if (sum <= 1e-12)
			break; // fewer distinct colors than asked for
		double target = (double)(next_random(&seed) & 0xFFFFFF) / (double)0x1000000 * sum;
		size_t pick = n - 1;
		for (size_t i = 0; i < n; i++) {
			target -= dist[i];
			if (target <= 0.0) {
				pick = i;
				break;
			}
		}
		clusters[seeded++].center = samples[pick];
		for (size_t i = 0; i < n; i++)
			dist[i] = fminf(dist[i], lab_dist2(samples[i], samples[pick]));
	}
	k = seeded;

	// Lloyd iterations.
	uint8_t *assign = bmalloc(n);
	memset(assign, 0xFF, n);
	for (int it = 0; it < PALETTE_ITERATIONS; it++) {
		double acc[PALETTE_MAX_COLORS][4];
		memset(acc, 0, sizeof(acc));
		bool changed = false;
		for (size_t i = 0; i < n; i++) {
			size_t best = 0;
			float best_d = lab_dist2(samples[i], clusters[0].center);
			for (size_t j = 1; j < k; j++) {
				const float d = lab_dist2(samples[i], clusters[j].center);
				if (d < best_d) {
					best_d = d;
					best = j;
				}
			}
			if (assign[i] != best) {
				changed = true;
				assign[i] = (uint8_t)best;
			}
			acc[best][0] += samples[i].l;
			acc[best][1] += samples[i].a;
			acc[best][2] += samples[i].b;
			acc[best][3] += 1.0;
		}
		for (size_t j = 0; j < k; j++) {
			clusters[j].weight = (float)(acc[j][3] / (double)n);
			if (acc[j][3] > 0.0) {
				clusters[j].center.l = (float)(acc[j][0] / acc[j][3]);
				clusters[j].center.a = (float)(acc[j][1] / acc[j][3]);
				clusters[j].center.b = (float)(acc[j][2] / acc[j][3]);
			}
		}
		if (!changed)
			break;
	}
	bfree(assign);
	bfree(dist);
	bfree(samples);

	// Drop empty clusters and merge near duplicates.
	const float merge = 0.025f * 0.025f;
	size_t m = 0;
	for (size_t j = 0; j < k; j++) {
		if (clusters[j].weight <= 0.0f)
			continue;
		bool merged = false;
		for (size_t q = 0; q < m && !merged; q++) {
			if (lab_dist2(clusters[q].center, clusters[j].center) < merge) {
				const float wsum = clusters[q].weight + clusters[j].weight;
				const float t = clusters[j].weight / wsum;
				clusters[q].center.l += (clusters[j].center.l - clusters[q].center.l) * t;
				clusters[q].center.a += (clusters[j].center.a - clusters[q].center.a) * t;
				clusters[q].center.b += (clusters[j].center.b - clusters[q].center.b) * t;
				clusters[q].weight = wsum;
				merged = true;
			}
		}
		if (!merged)
			clusters[m++] = clusters[j];
	}

	switch (order) {
	case GRADIENT_PALETTE_HUE:
		sort_by_hue(clusters, m);
		break;
	case GRADIENT_PALETTE_PROMINENCE:
		qsort(clusters, m, sizeof(struct cluster), compare_weight);
		break;
	default:
		qsort(clusters, m, sizeof(struct cluster), compare_lightness);
		break;
	}

	for (size_t j = 0; j < m; j++) {
		colors[j] = lab_to_color(clusters[j].center);
		if (weights)
			weights[j] = clusters[j].weight;
	}
	return m;
}

size_t gradient_palette_to_stops(const uint32_t *colors, size_t count, struct gradient_stop *stops, size_t max_stops)
{
	if (!count || max_stops < 2)
		return 0;
	if (count > max_stops)
		count = max_stops;
	if (count == 1) {
		stops[0] = (struct gradient_stop){0.0f, colors[0], 0.5f, GRADIENT_EASE_LINEAR};
		stops[1] = (struct gradient_stop){1.0f, colors[0], 0.5f, GRADIENT_EASE_LINEAR};
		return 2;
	}
	for (size_t i = 0; i < count; i++)
		stops[i] = (struct gradient_stop){(float)i / (float)(count - 1), colors[i], 0.5f, GRADIENT_EASE_LINEAR};
	return count;
}
