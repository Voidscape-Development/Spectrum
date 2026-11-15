#include "lens.h"

#include <string.h>

#define LENS_GOLDEN_ANGLE 2.39996322972865332f
#define LENS_TILE_SIZE 16u

static const int lens_quality_samples[] = {32, 80, 160, 320};

void set_lens_blur_types(obs_properties_t *props)
{
	obs_property_t *p = obs_properties_get(props, "blur_type");
	obs_property_list_clear(p);
	obs_property_list_add_int(p, obs_module_text(TYPE_AREA_LABEL), TYPE_AREA);
	obs_property_list_add_int(p, obs_module_text(TYPE_DEPTH_LABEL), TYPE_DEPTH);
}

void lens_setup_callbacks(blur_filter_data_t *data)
{
	data->video_render = render_video_lens;
	data->load_effect = load_effect_lens;
	data->update = NULL;
}

void load_effect_lens(blur_filter_data_t *filter)
{
	load_effect_with_params(&filter->effect, "/shaders/blur/lens_blur.effect", NULL, 0, NULL, NULL);
}

/* ------------------------------------------------------------------------
 * Shared helpers
 * --------------------------------------------------------------------- */

blur_filter_data_t *blur_move_peer(blur_filter_data_t *data, float *t, float *fade)
{
	*t = 0.0f;
	*fade = 1.0f;
	if (!move_get_transition_filter)
		return NULL;
	obs_source_t *filter_to = NULL;
	const float f = move_get_transition_filter(data->context, &filter_to);
	if (f <= 0.0f)
		return NULL;

	if (filter_to) {
		blur_filter_data_t *data_to = NULL;
		if (strcmp(obs_source_get_unversioned_id(filter_to), obs_source_get_unversioned_id(data->context)) == 0)
			data_to = obs_obj_get_data(filter_to);
		if (data_to && data_to->blur_algorithm == data->blur_algorithm &&
		    data_to->blur_type == data->blur_type) {
			*t = f;
			return data_to;
		}
		*fade = f > 0.5f ? 1.0f - (f - 0.5f) * 2.0f : 1.0f - f * 2.0f;
	} else {
		*fade = 1.0f - f;
	}
	return NULL;
}

void blur_fx_float(gs_effect_t *effect, const char *name, float value)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, name);
	if (p)
		gs_effect_set_float(p, value);
}

void blur_fx_vec2(gs_effect_t *effect, const char *name, float x, float y)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, name);
	if (p) {
		struct vec2 v;
		vec2_set(&v, x, y);
		gs_effect_set_vec2(p, &v);
	}
}

void blur_fx_vec4(gs_effect_t *effect, const char *name, const struct vec4 *value)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, name);
	if (p)
		gs_effect_set_vec4(p, value);
}

void blur_fx_texture(gs_effect_t *effect, const char *name, gs_texture_t *texture)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, name);
	if (p)
		gs_effect_set_texture(p, texture);
}

void blur_fx_set_linear_common(blur_filter_data_t *data, gs_effect_t *effect, uint32_t width, uint32_t height)
{
	const bool srgb = data->color_space == GS_CS_SRGB;
	blur_fx_vec2(effect, "uv_size", (float)width, (float)height);
	blur_fx_float(effect, "srgb_input", srgb ? 1.0f : 0.0f);
	// 8-bit targets get one step of triangular dither; float targets
	// don't band.
	blur_fx_float(effect, "dither_step", data->color_format == GS_RGBA ? 1.0f / 255.0f : 0.0f);
}

gs_texture_t *blur_fx_draw(gs_effect_t *effect, const char *technique, gs_texrender_t **texrender,
			   enum gs_color_format format, uint32_t width, uint32_t height)
{
	*texrender = blur_create_or_reset_texrender(*texrender, format);

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	blur_set_blending_parameters();

	bool rendered = false;
	if (gs_texrender_begin(*texrender, width, height)) {
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, technique))
			gs_draw_sprite(NULL, 0, width, height);
		gs_texrender_end(*texrender);
		rendered = true;
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
	return rendered ? gs_texrender_get_texture(*texrender) : NULL;
}

void blur_passthrough(blur_filter_data_t *data, gs_texture_t *texture)
{
	data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);
	blur_texrender_set_texture(texture, data->output_texrender);
}

/* ------------------------------------------------------------------------
 * Lens blur
 * --------------------------------------------------------------------- */

struct lens_params {
	float radius;
	float rotation;
	float roundness;
	float hl_threshold;
	float hl_gain;
	float focus_distance;
	float focus_range;
	float focus_falloff;
	float focus_x;
	float focus_y;
};

static void lens_params_from(const blur_filter_data_t *d, struct lens_params *p)
{
	p->radius = d->radius;
	p->rotation = d->lens_rotation;
	p->roundness = d->lens_roundness;
	p->hl_threshold = d->lens_highlight_threshold;
	p->hl_gain = d->lens_highlights ? d->lens_highlight_gain : 0.0f;
	p->focus_distance = d->depth_focus_distance;
	p->focus_range = d->depth_focus_range;
	p->focus_falloff = d->depth_focus_falloff;
	p->focus_x = d->depth_focus_x;
	p->focus_y = d->depth_focus_y;
}

// Current settings, interpolated during a Move Transition.
static void lens_resolve_params(blur_filter_data_t *data, struct lens_params *p)
{
	lens_params_from(data, p);
	float t, fade;
	blur_filter_data_t *to = blur_move_peer(data, &t, &fade);
	if (to) {
		struct lens_params q;
		lens_params_from(to, &q);
		float *a = (float *)p;
		const float *b = (const float *)&q;
		for (size_t i = 0; i < sizeof(*p) / sizeof(float); i++)
			a[i] = blur_lerpf(a[i], b[i], t);
	} else {
		p->radius *= fade;
	}
}

static int lens_aperture_blades(const blur_filter_data_t *data)
{
	switch (data->lens_shape) {
	case LENS_SHAPE_HEXAGON:
		return 6;
	case LENS_SHAPE_OCTAGON:
		return 8;
	case LENS_SHAPE_POLYGON:
		return data->lens_blades < 3 ? 3 : (data->lens_blades > 16 ? 16 : data->lens_blades);
	default:
		return 0;
	}
}

// (Re)builds the sample offset texture: n points of a golden-angle spiral
// spread evenly over the unit disc, then pushed out radially onto the
// aperture polygon. The area weight (b^2) keeps the sample density uniform
// over the polygon. Must be called with the graphics context entered.
static void lens_update_kernel(blur_filter_data_t *data, int n, int blades, float rotation_deg, float roundness)
{
	const float key[5] = {(float)n, (float)blades, rotation_deg, roundness, 1.0f};
	if (data->lens_kernel && memcmp(key, data->lens_kernel_key, sizeof(key)) == 0)
		return;
	memcpy(data->lens_kernel_key, key, sizeof(key));

	float *samples = bmalloc(sizeof(float) * 4 * (size_t)n);
	const float rot = rotation_deg * (float)M_PI / 180.0f;
	const float round = fminf(fmaxf(roundness, 0.0f), 1.0f);
	for (int i = 0; i < n; i++) {
		const float r = sqrtf(((float)i + 0.5f) / (float)n);
		const float th = (float)i * LENS_GOLDEN_ANGLE;
		float b = 1.0f;
		if (blades >= 3) {
			const float seg = 2.0f * (float)M_PI / (float)blades;
			float a = th - rot;
			a = a - seg * floorf(a / seg) - seg * 0.5f;
			const float poly = cosf((float)M_PI / (float)blades) / cosf(a);
			b = poly + (1.0f - poly) * round;
		}
		samples[i * 4 + 0] = r * b * cosf(th);
		samples[i * 4 + 1] = r * b * sinf(th);
		samples[i * 4 + 2] = r;
		samples[i * 4 + 3] = b * b;
	}

	if (data->lens_kernel)
		gs_texture_destroy(data->lens_kernel);
	const uint8_t *planes[] = {(const uint8_t *)samples};
	data->lens_kernel = gs_texture_create((uint32_t)n, 1, GS_RGBA32F, 1, planes, 0);
	bfree(samples);
}

// The depth map for this frame, or NULL if none is available.
static gs_texture_t *lens_depth_map(blur_filter_data_t *data)
{
	if (data->depth_map == DEPTH_MAP_EFFECT_MASK) {
		gs_texture_t *map = blur_render_effect_mask_map(data);
		// The mask now drives the blur amount; don't also mix with it.
		if (map)
			data->skip_effect_mask = true;
		return map;
	}
	obs_source_t *source = external_source_get(data, &data->depth_source);
	if (!source)
		return NULL;
	// Depth maps are data, not color: always render them as SDR.
	gs_texture_t *map = render_source_to_texrender(source, &data->depth_source_texrender, GS_CS_SRGB);
	obs_source_release(source);
	return map;
}

// Binds every uniform the depth passes and the depth gather read. OBS
// clears effect parameters after each draw, so this runs before each one.
static void lens_set_depth_uniforms(blur_filter_data_t *data, gs_effect_t *effect, const struct lens_params *p)
{
	const uint32_t w = data->width, h = data->height;
	const uint32_t tw = (w + LENS_TILE_SIZE - 1) / LENS_TILE_SIZE;
	const uint32_t th = (h + LENS_TILE_SIZE - 1) / LENS_TILE_SIZE;

	blur_fx_set_linear_common(data, effect, w, h);
	float channel = 0.0f;
	if (data->depth_map == DEPTH_MAP_EFFECT_MASK)
		channel = 2.0f;
	else if (data->depth_map == DEPTH_MAP_ALPHA)
		channel = 1.0f;
	blur_fx_float(effect, "depth_channel", channel);
	blur_fx_float(effect, "depth_invert", data->depth_invert ? 1.0f : 0.0f);
	blur_fx_float(effect, "focus_distance", p->focus_distance / 100.0f);
	blur_fx_float(effect, "focus_range", p->focus_range / 100.0f);
	blur_fx_float(effect, "focus_falloff", p->focus_falloff / 100.0f);
	blur_fx_float(effect, "focus_use_point", data->depth_focus_mode == DEPTH_FOCUS_POINT ? 1.0f : 0.0f);
	blur_fx_vec2(effect, "focus_point", p->focus_x / 100.0f, p->focus_y / 100.0f);
	blur_fx_vec2(effect, "tile_count", (float)tw, (float)th);
	blur_fx_vec2(effect, "tile_uv_scale", (float)w / (float)(tw * LENS_TILE_SIZE),
		     (float)h / (float)(th * LENS_TILE_SIZE));
}

// Computes per-pixel CoC (data->lens_coc) and the dilated per-tile max CoC
// (data->lens_tile_max). Returns false if any pass failed.
static bool lens_depth_passes(blur_filter_data_t *data, gs_effect_t *effect, gs_texture_t *depth_map,
			      const struct lens_params *p)
{
	const uint32_t w = data->width, h = data->height;
	const uint32_t tw = (w + LENS_TILE_SIZE - 1) / LENS_TILE_SIZE;
	const uint32_t th = (h + LENS_TILE_SIZE - 1) / LENS_TILE_SIZE;

	lens_set_depth_uniforms(data, effect, p);
	blur_fx_texture(effect, "depth_tex", depth_map);
	gs_texture_t *coc = blur_fx_draw(effect, "Coc", &data->lens_coc, GS_RGBA16F, w, h);
	if (!coc)
		return false;

	lens_set_depth_uniforms(data, effect, p);
	blur_fx_texture(effect, "image", coc);
	gs_texture_t *tile = blur_fx_draw(effect, "TileMax", &data->lens_tile, GS_RGBA16F, tw, th);
	if (!tile)
		return false;

	lens_set_depth_uniforms(data, effect, p);
	blur_fx_texture(effect, "image", tile);
	return blur_fx_draw(effect, "TileDilate", &data->lens_tile_max, GS_RGBA16F, tw, th) != NULL;
}

void render_video_lens(blur_filter_data_t *data)
{
	gs_effect_t *effect = data->effect;
	gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);
	if (!effect || !texture)
		return;

	struct lens_params p;
	lens_resolve_params(data, &p);
	if (p.radius < MIN_LENS_BLUR_RADIUS) {
		blur_passthrough(data, texture);
		return;
	}

	texture = blend_composite(texture, data);
	const uint32_t w = data->width, h = data->height;

	// 1. Linear premultiplied float copy of the input, with highlights
	//    boosted, plus 2x and 4x downsampled levels for sparse taps.
	const float thr = fminf(fmaxf(p.hl_threshold / 100.0f, 0.0f), 1.0f);
	const float thr_lin = thr <= 0.04045f ? thr / 12.92f : powf((thr + 0.055f) / 1.055f, 2.4f);
	blur_fx_set_linear_common(data, effect, w, h);
	blur_fx_float(effect, "hl_threshold", thr_lin);
	blur_fx_float(effect, "hl_gain", fmaxf(p.hl_gain, 0.0f));
	blur_fx_texture(effect, "image", texture);
	gs_texture_t *linear = blur_fx_draw(effect, "PrefilterBoost", &data->lens_linear, GS_RGBA16F, w, h);
	if (!linear)
		return;
	const uint32_t hw = (w + 1) / 2, hh = (h + 1) / 2;
	blur_fx_texture(effect, "image", linear);
	blur_fx_vec2(effect, "src_texel", 1.0f / (float)w, 1.0f / (float)h);
	gs_texture_t *half = blur_fx_draw(effect, "Down", &data->lens_half, GS_RGBA16F, hw, hh);
	blur_fx_texture(effect, "image", half);
	blur_fx_vec2(effect, "src_texel", 1.0f / (float)hw, 1.0f / (float)hh);
	gs_texture_t *quarter =
		blur_fx_draw(effect, "Down", &data->lens_quarter, GS_RGBA16F, (hw + 1) / 2, (hh + 1) / 2);
	if (!half || !quarter)
		return;

	// 2. Depth: per-pixel circle of confusion. Without a usable depth map
	//    everything is out of focus (a plain lens blur).
	bool depth = false;
	if (data->blur_type == TYPE_DEPTH) {
		gs_texture_t *map = lens_depth_map(data);
		depth = map && lens_depth_passes(data, effect, map, &p);
	}

	// 3. Aperture kernel. A uniform blur needs no more samples than the
	//    disc has pixels; a depth gather spans the max radius, so it always
	//    uses the full count.
	const int quality = data->lens_quality < LENS_QUALITY_DRAFT || data->lens_quality > LENS_QUALITY_ULTRA
				    ? LENS_QUALITY_NORMAL
				    : data->lens_quality;
	int n = lens_quality_samples[quality];
	if (!depth) {
		const int disc = (int)ceilf((float)M_PI * p.radius * p.radius);
		n = disc < n ? (disc < 12 ? 12 : disc) : n;
	}
	lens_update_kernel(data, n, lens_aperture_blades(data), p.rotation, p.roundness / 100.0f);
	if (!data->lens_kernel)
		return;

	// 4. Gather.
	if (depth) {
		lens_set_depth_uniforms(data, effect, &p);
		blur_fx_texture(effect, "coc_tex", gs_texrender_get_texture(data->lens_coc));
		blur_fx_texture(effect, "tile_tex", gs_texrender_get_texture(data->lens_tile_max));
	} else {
		blur_fx_set_linear_common(data, effect, w, h);
	}
	blur_fx_texture(effect, "image", linear);
	blur_fx_texture(effect, "image_half", half);
	blur_fx_texture(effect, "image_quarter", quarter);
	blur_fx_texture(effect, "kernel_tex", data->lens_kernel);
	blur_fx_float(effect, "radius", p.radius);
	blur_fx_float(effect, "sample_count", (float)n);

	blur_fx_draw(effect, depth ? "GatherDepth" : "Gather", &data->output_texrender, data->color_format, w, h);
}
