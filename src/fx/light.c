// Spectrum Light: bloom, light rays, lens flare and a point light with bump
// from the source's alpha.
//
// Unlike the single-pass effects, this renders the filter input into its own
// texture, builds a bright pass and a dual filter (Kawase) blur pyramid in
// float, then composites everything in linear light.

#include "../common/fx-filter.h"

#include <graphics/vec4.h>
#include <math.h>
#include "../common/presets.h"

#define MAX_LEVELS 7

struct light_data {
	gs_texrender_t *input;
	gs_texrender_t *bright;
	gs_texrender_t *down[MAX_LEVELS];
	gs_texrender_t *up[MAX_LEVELS];
};

static void light_create_hook(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	fx->user = bzalloc(sizeof(struct light_data));
}

static void light_destroy_hook(fx_filter_t *fx)
{
	struct light_data *d = fx->user;
	obs_enter_graphics();
	gs_texrender_destroy(d->input);
	gs_texrender_destroy(d->bright);
	for (int i = 0; i < MAX_LEVELS; i++) {
		gs_texrender_destroy(d->down[i]);
		gs_texrender_destroy(d->up[i]);
	}
	obs_leave_graphics();
	bfree(d);
}

static const struct fx_filter_def light_def = {
	.effect_file = "shaders/fx/light.effect",
	.dither_key = "li_dither",
	.create = light_create_hook,
	.destroy = light_destroy_hook,
};

static gs_texrender_t *ensure_tr(gs_texrender_t **tr, enum gs_color_format format)
{
	if (!*tr)
		*tr = gs_texrender_create(format, GS_ZS_NONE);
	gs_texrender_reset(*tr);
	return *tr;
}

// Runs one pass of `tech` reading `src` (w x h source) into `dst` (dw x dh).
static gs_texture_t *run_pass(gs_effect_t *effect, const char *tech, gs_texture_t *src, uint32_t sw, uint32_t sh,
			      gs_texrender_t *dst, uint32_t dw, uint32_t dh)
{
	fx_set_texture(effect, "pass_tex", src);
	fx_set_vec2(effect, "pass_texel", 1.0f / (float)sw, 1.0f / (float)sh);
	if (gs_texrender_begin(dst, dw, dh)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)dw, 0.0f, (float)dh, -100.0f, 100.0f);
		while (gs_effect_loop(effect, tech))
			gs_draw_sprite(NULL, 0, dw, dh);
		gs_texrender_end(dst);
	}
	return gs_texrender_get_texture(dst);
}

static void light_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	fx_filter_t *fx = data;
	struct light_data *d = fx->user;

	obs_source_t *target = obs_filter_get_target(fx->context);
	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	if (!w || !h || !fx_filter_ensure_effect(fx)) {
		obs_source_skip_video_filter(fx->context);
		return;
	}
	gs_effect_t *effect = fx->effect;
	obs_data_t *settings = fx_filter_settings(fx);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	// 1. The filter input, 8-bit sRGB (sampled as linear light below).
	ensure_tr(&d->input, GS_RGBA);
	bool have_input = false;
	if (gs_texrender_begin_with_color_space(d->input, w, h, GS_CS_SRGB)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		if (obs_source_process_filter_begin(fx->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
			obs_source_process_filter_end(fx->context, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
			have_input = true;
		}
		gs_texrender_end(d->input);
	}
	gs_texture_t *input = have_input ? gs_texrender_get_texture(d->input) : NULL;
	if (!input) {
		gs_blend_state_pop();
		obs_data_release(settings);
		obs_source_skip_video_filter(fx->context);
		return;
	}

	fx_filter_set_common(fx, effect, settings, NULL, 0.0f, w, h, true, 0.0f);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");

	const bool bloom = obs_data_get_double(settings, "li_bloom") > 0.0;
	const bool rays = obs_data_get_double(settings, "li_rays") > 0.0;
	gs_texture_t *bright = NULL, *blurred = NULL;
	int bloom_levels = 1;
	if (bloom || rays) {
		// 2. Bright pass at half resolution.
		const uint32_t bw = (w + 1) / 2, bh = (h + 1) / 2;
		gs_effect_set_texture_srgb(image, input);
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		bright = run_pass(effect, "Bright", input, w, h, ensure_tr(&d->bright, GS_RGBA16F), bw, bh);

		// 3. Blur pyramid: halve down to the radius, then back up.
		if (bloom && bright) {
			const double radius = obs_data_get_double(settings, "li_radius");
			int levels = (int)ceil(log2(fmax(radius, 2.0) / 2.0));
			levels = levels < 1 ? 1 : (levels > MAX_LEVELS ? MAX_LEVELS : levels);
			uint32_t sizes_w[MAX_LEVELS + 1], sizes_h[MAX_LEVELS + 1];
			sizes_w[0] = bw;
			sizes_h[0] = bh;
			gs_texture_t *tex = bright;
			gs_texture_t *level_tex[MAX_LEVELS + 1];
			level_tex[0] = bright;
			int done = 0;
			for (int i = 0; i < levels; i++) {
				const uint32_t nw = sizes_w[i] / 2, nh = sizes_h[i] / 2;
				if (nw < 2 || nh < 2)
					break;
				sizes_w[i + 1] = nw;
				sizes_h[i + 1] = nh;
				tex = run_pass(effect, "Down", tex, sizes_w[i], sizes_h[i],
					       ensure_tr(&d->down[i], GS_RGBA16F), nw, nh);
				level_tex[i + 1] = tex;
				done++;
			}
			// Upsample, adding each level back (progressive bloom).
			for (int i = done - 1; i >= 0; i--) {
				fx_set_texture(effect, "add_tex", level_tex[i]);
				tex = run_pass(effect, "Up", tex, sizes_w[i + 1], sizes_h[i + 1],
					       ensure_tr(&d->up[i], GS_RGBA16F), sizes_w[i], sizes_h[i]);
			}
			blurred = tex;
			bloom_levels = done + 1;
		}
		gs_enable_framebuffer_srgb(previous);
	}
	gs_blend_state_pop();

	// 4. Composite onto the output, linear light through the sRGB
	// framebuffer, straight alpha.
	fx_filter_set_common(fx, effect, settings, NULL, 0.0f, w, h, true, 0.0f);
	gs_effect_set_texture_srgb(image, input);
	fx_set_texture(effect, "bloom_tex", blurred ? blurred : input);
	fx_set_texture(effect, "bright_tex", bright ? bright : input);
	fx_set_float(effect, "bloom_scale", 1.0f / (float)bloom_levels);
	if (!blurred)
		fx_set_float(effect, "li_bloom", 0.0f);
	if (!bright)
		fx_set_float(effect, "li_rays", 0.0f);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(effect, "Draw"))
		gs_draw_sprite(NULL, 0, w, h);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);

	obs_data_release(settings);
}

static const char *light_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LightFilter");
}

static void *light_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&light_def, settings, source);
}

static void light_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "li_bloom", 60.0);
	obs_data_set_default_double(settings, "li_threshold", 70.0);
	obs_data_set_default_double(settings, "li_knee", 30.0);
	obs_data_set_default_double(settings, "li_radius", 48.0);
	obs_data_set_default_int(settings, "li_bloom_tint", 0xFFFFFFFF);
	obs_data_set_default_double(settings, "li_rays_x", 50.0);
	obs_data_set_default_double(settings, "li_rays_y", 0.0);
	obs_data_set_default_double(settings, "li_rays_length", 50.0);
	obs_data_set_default_double(settings, "li_rays_decay", 95.0);
	obs_data_set_default_double(settings, "li_flare_x", 25.0);
	obs_data_set_default_double(settings, "li_flare_y", 25.0);
	obs_data_set_default_double(settings, "li_flare_size", 100.0);
	obs_data_set_default_double(settings, "li_flare_streaks", 6.0);
	obs_data_set_default_int(settings, "li_flare_color", 0xFFA0D8FF);
	obs_data_set_default_double(settings, "li_light_x", 30.0);
	obs_data_set_default_double(settings, "li_light_y", 30.0);
	obs_data_set_default_double(settings, "li_light_height", 150.0);
	obs_data_set_default_double(settings, "li_light_radius", 60.0);
	obs_data_set_default_double(settings, "li_ambient", 40.0);
	obs_data_set_default_double(settings, "li_bump", 6.0);
	obs_data_set_default_double(settings, "li_bump_depth", 100.0);
	obs_data_set_default_int(settings, "li_light_color", 0xFFFFFFFF);
	obs_data_set_default_bool(settings, "li_dither", true);
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static obs_properties_t *light_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *g;

	g = obs_properties_create();
	slider(g, "li_bloom", "Light.Intensity", 0.0, 400.0, 0.1, "%");
	slider(g, "li_threshold", "Light.Threshold", 0.0, 100.0, 0.1, "%");
	slider(g, "li_knee", "Light.Knee", 0.0, 100.0, 0.1, "%");
	slider(g, "li_radius", "Light.Radius", 2.0, 256.0, 1.0, "px");
	obs_properties_add_color(g, "li_bloom_tint", obs_module_text("Light.Tint"));
	obs_properties_add_group(props, "li_bloom_group", obs_module_text("Light.Bloom"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	slider(g, "li_rays", "Light.Intensity", 0.0, 400.0, 0.1, "%");
	slider(g, "li_rays_x", "Light.SourceX", -50.0, 150.0, 0.1, "%");
	slider(g, "li_rays_y", "Light.SourceY", -50.0, 150.0, 0.1, "%");
	slider(g, "li_rays_length", "LongShadow.Length", 0.0, 100.0, 0.1, "%");
	slider(g, "li_rays_decay", "Distort.Decay", 50.0, 100.0, 0.1, "%");
	obs_properties_add_group(props, "li_rays_group", obs_module_text("Light.Rays"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	slider(g, "li_flare", "Light.Intensity", 0.0, 400.0, 0.1, "%");
	slider(g, "li_flare_x", "Light.SourceX", -50.0, 150.0, 0.1, "%");
	slider(g, "li_flare_y", "Light.SourceY", -50.0, 150.0, 0.1, "%");
	slider(g, "li_flare_size", "Light.Size", 10.0, 400.0, 0.1, "%");
	slider(g, "li_flare_streaks", "Light.Streaks", 0.0, 16.0, 1.0, NULL);
	obs_properties_add_color(g, "li_flare_color", obs_module_text("Light.Tint"));
	obs_properties_add_group(props, "li_flare_group", obs_module_text("Light.Flare"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	slider(g, "li_light", "Light.Intensity", 0.0, 400.0, 0.1, "%");
	slider(g, "li_light_x", "Light.SourceX", -50.0, 150.0, 0.1, "%");
	slider(g, "li_light_y", "Light.SourceY", -50.0, 150.0, 0.1, "%");
	slider(g, "li_light_height", "Light.Height", 1.0, 2000.0, 1.0, "px");
	slider(g, "li_light_radius", "Light.Falloff", 1.0, 400.0, 0.1, "%");
	slider(g, "li_ambient", "Light.Ambient", 0.0, 200.0, 0.1, "%");
	slider(g, "li_bump", "Light.Bump", 0.5, 64.0, 0.1, "px");
	slider(g, "li_bump_depth", "Bevel.Depth", 0.0, 1000.0, 1.0, "%");
	obs_properties_add_color(g, "li_light_color", obs_module_text("Light.Color"));
	obs_properties_add_group(props, "li_point_group", obs_module_text("Light.Point"), OBS_GROUP_NORMAL, g);

	obs_properties_add_bool(props, "li_dither", obs_module_text("Gradient.Dither"));
	spectrum_presets_properties(props, data ? ((fx_filter_t *)data)->context : NULL);
	return props;
}

struct obs_source_info spectrum_light_filter = {
	.id = "spectrum_light_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = light_name,
	.create = light_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = light_render,
	.get_properties = light_properties,
	.get_defaults = light_defaults,
};
