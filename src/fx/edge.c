// Spectrum Edge: toon/ink outlines (Sobel or difference of Gaussians, with
// optional cel shading), neon edges with a glow, pencil sketch (XDoG plus
// hatching) and emboss, from the luma and alpha of the source.
//
// Like Spectrum Light, this renders the filter input into its own texture
// and runs a few float passes (see data/shaders/fx/edge.effect) before
// compositing onto the output.

#include "../common/fx-filter.h"
#include "../common/presets.h"

#include <graphics/vec4.h>
#include <math.h>

#define ED_GRADIENT "ed_gradient_"

#define MODE_TOON 1
#define MODE_NEON 2
#define MODE_PENCIL 3
#define MODE_EMBOSS 4

#define MAX_TAPS 48

struct edge_data {
	gs_texrender_t *input;
	gs_texrender_t *prep;
	gs_texrender_t *blur_h;
	gs_texrender_t *blur;
	gs_texrender_t *edge;
	gs_texrender_t *dilated;
	gs_texrender_t *core;
	gs_texrender_t *glow_h;
	gs_texrender_t *glow;
};

static void edge_create_hook(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	fx->user = bzalloc(sizeof(struct edge_data));
}

static void edge_destroy_hook(fx_filter_t *fx)
{
	struct edge_data *d = fx->user;
	obs_enter_graphics();
	gs_texrender_destroy(d->input);
	gs_texrender_destroy(d->prep);
	gs_texrender_destroy(d->blur_h);
	gs_texrender_destroy(d->blur);
	gs_texrender_destroy(d->edge);
	gs_texrender_destroy(d->dilated);
	gs_texrender_destroy(d->core);
	gs_texrender_destroy(d->glow_h);
	gs_texrender_destroy(d->glow);
	obs_leave_graphics();
	bfree(d);
}

static const struct fx_filter_def edge_def = {
	.effect_file = "shaders/fx/edge.effect",
	.gradient_prefix = ED_GRADIENT,
	.dither_key = "ed_dither",
	.create = edge_create_hook,
	.destroy = edge_destroy_hook,
};

static gs_texrender_t *ensure_tr(gs_texrender_t **tr, enum gs_color_format format)
{
	if (!*tr)
		*tr = gs_texrender_create(format, GS_ZS_NONE);
	gs_texrender_reset(*tr);
	return *tr;
}

// Everything a pass binds. OBS forgets an effect's parameter values at the
// end of each technique, so all of them are set again before every pass.
struct pass_state {
	fx_filter_t *fx;
	gs_effect_t *effect;
	obs_data_t *settings;
	gs_texture_t *input;
	uint32_t w, h;
	float sigma1, sigma2, taps, stride;
	float blur_x, blur_y;
	float dilate_radius;
	float sobel_layout;
	gs_texture_t *blur, *edges, *core, *glow;
};

static void bind_pass(struct pass_state *ps, gs_texture_t *src)
{
	gs_effect_t *e = ps->effect;
	fx_filter_set_common(ps->fx, e, ps->settings, NULL, 0.0f, ps->w, ps->h, true, 0.0f);
	gs_effect_set_texture_srgb(gs_effect_get_param_by_name(e, "image"), ps->input);
	fx_set_texture(e, "pass_tex", src);
	fx_set_vec2(e, "pass_texel", 1.0f / (float)ps->w, 1.0f / (float)ps->h);
	fx_set_vec2(e, "blur_dir", ps->blur_x, ps->blur_y);
	fx_set_float(e, "sigma1", ps->sigma1);
	fx_set_float(e, "sigma2", ps->sigma2);
	fx_set_float(e, "taps", ps->taps);
	fx_set_float(e, "stride", ps->stride);
	fx_set_float(e, "dilate_radius", ps->dilate_radius);
	fx_set_float(e, "sobel_layout", ps->sobel_layout);
	gs_texture_t *fallback = ps->blur;
	fx_set_texture(e, "blur_tex", ps->blur);
	fx_set_texture(e, "edge_tex", ps->edges ? ps->edges : fallback);
	fx_set_texture(e, "core_tex", ps->core ? ps->core : fallback);
	fx_set_texture(e, "glow_tex", ps->glow ? ps->glow : fallback);
	fx_set_float(e, "use_glow", ps->glow ? 1.0f : 0.0f);
}

// Runs one full-size pass of `tech` reading `src` into `dst`.
static gs_texture_t *run_pass(struct pass_state *ps, const char *tech, gs_texture_t *src, gs_texrender_t *dst)
{
	bind_pass(ps, src);
	if (gs_texrender_begin(dst, ps->w, ps->h)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)ps->w, 0.0f, (float)ps->h, -100.0f, 100.0f);
		while (gs_effect_loop(ps->effect, tech))
			gs_draw_sprite(NULL, 0, ps->w, ps->h);
		gs_texrender_end(dst);
	}
	return gs_texrender_get_texture(dst);
}

// Gaussian taps for a blur reaching about 3 sigma (of the larger sigma),
// with at most MAX_TAPS samples per side.
static void set_taps(struct pass_state *ps, float s1, float s2)
{
	const float reach = 3.0f * fmaxf(s1, s2);
	float taps = ceilf(reach);
	float stride = 1.0f;
	if (taps > MAX_TAPS) {
		stride = reach / MAX_TAPS;
		taps = MAX_TAPS;
	}
	if (fmaxf(s1, s2) < 0.3f)
		taps = 0.0f;
	ps->sigma1 = fmaxf(s1, 0.01f);
	ps->sigma2 = fmaxf(s2, 0.01f);
	ps->taps = taps;
	ps->stride = stride;
}

static gs_texture_t *separable(struct pass_state *ps, const char *tech, gs_texture_t *src, gs_texrender_t **mid,
			       gs_texrender_t **out)
{
	ps->blur_x = 1.0f / (float)ps->w;
	ps->blur_y = 0.0f;
	gs_texture_t *t = run_pass(ps, tech, src, ensure_tr(mid, GS_RGBA16F));
	ps->blur_x = 0.0f;
	ps->blur_y = 1.0f / (float)ps->h;
	return run_pass(ps, tech, t, ensure_tr(out, GS_RGBA16F));
}

static void edge_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	fx_filter_t *fx = data;
	struct edge_data *d = fx->user;

	obs_source_t *target = obs_filter_get_target(fx->context);
	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	if (!w || !h || !fx_filter_ensure_effect(fx)) {
		obs_source_skip_video_filter(fx->context);
		return;
	}

	// Move Transition: blend towards the counterpart filter's settings.
	obs_data_t *settings = fx_filter_settings(fx);
	obs_source_t *move_to = NULL;
	const float move_t = fx_move_target(fx->context, &move_to);
	if (move_to) {
		fx_filter_t *other = obs_obj_get_data(move_to);
		if (other && other->def == fx->def) {
			obs_data_t *to = fx_filter_settings(other);
			obs_data_t *mixed = fx_lerp_settings(settings, to, move_t);
			obs_data_release(to);
			obs_data_release(settings);
			settings = mixed;
		}
	}

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

	const int mode = (int)obs_data_get_int(settings, "ed_mode");
	const int method = (int)obs_data_get_int(settings, "ed_method");
	const bool dog = mode == MODE_PENCIL || (mode == MODE_TOON && method == 2);

	struct pass_state ps = {
		.fx = fx,
		.effect = fx->effect,
		.settings = settings,
		.input = input,
		.w = w,
		.h = h,
	};
	set_taps(&ps, 0.0f, 0.0f);
	ps.sobel_layout = (mode == MODE_NEON || (mode == MODE_TOON && method != 2)) ? 1.0f : 0.0f;

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	// 2. Luma and alpha, smoothed (two sigmas for the difference of
	// Gaussians).
	gs_texture_t *prep = run_pass(&ps, "Prep", NULL, ensure_tr(&d->prep, GS_RGBA16F));
	const float s1 = (float)obs_data_get_double(settings, dog ? "ed_detail" : "ed_smooth");
	set_taps(&ps, s1, dog ? s1 * 1.6f : s1);
	ps.blur = separable(&ps, "Blur", prep, &d->blur_h, &d->blur);

	// 3. Edges, then thicker lines.
	if (mode != MODE_EMBOSS) {
		ps.edges = run_pass(&ps, "Edge", NULL, ensure_tr(&d->edge, GS_RGBA16F));
		const float thickness = (float)obs_data_get_double(settings, "ed_thickness");
		const float radius = fminf((thickness - 1.0f) * 0.5f, 8.0f);
		if ((mode == MODE_TOON || mode == MODE_NEON) && radius > 0.01f) {
			ps.dilate_radius = radius;
			ps.edges = run_pass(&ps, "Dilate", ps.edges, ensure_tr(&d->dilated, GS_RGBA16F));
		}
	}

	// 4. Neon: colored edges and their glow, linear light.
	if (mode == MODE_NEON && ps.edges) {
		ps.core = run_pass(&ps, "Colorize", ps.edges, ensure_tr(&d->core, GS_RGBA16F));
		const float radius = (float)obs_data_get_double(settings, "ed_glow_radius");
		if (radius >= 1.0f && obs_data_get_double(settings, "ed_glow") > 0.0) {
			set_taps(&ps, radius / 3.0f, radius / 3.0f);
			ps.glow = separable(&ps, "Glow", ps.core, &d->glow_h, &d->glow);
		}
	}
	gs_enable_framebuffer_srgb(previous);
	gs_blend_state_pop();

	// 5. Composite onto the output, linear light through the sRGB
	// framebuffer, straight alpha.
	bind_pass(&ps, NULL);
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	while (gs_effect_loop(ps.effect, "Draw"))
		gs_draw_sprite(NULL, 0, w, h);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);

	obs_data_release(settings);
}

static const char *edge_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("EdgeFilter");
}

static void *edge_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&edge_def, settings, source);
}

static void edge_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "ed_mode", MODE_TOON);
	obs_data_set_default_double(settings, "ed_amount", 100.0);
	obs_data_set_default_int(settings, "ed_method", 1);
	obs_data_set_default_double(settings, "ed_smooth", 1.0);
	obs_data_set_default_double(settings, "ed_detail", 1.5);
	obs_data_set_default_double(settings, "ed_threshold", 20.0);
	obs_data_set_default_double(settings, "ed_softness", 20.0);
	obs_data_set_default_double(settings, "ed_thickness", 2.0);
	obs_data_set_default_double(settings, "ed_color_edges", 100.0);
	obs_data_set_default_int(settings, "ed_ink_color", 0xFF000000);
	obs_data_set_default_double(settings, "ed_cel_levels", 0.0);
	obs_data_set_default_double(settings, "ed_cel_softness", 20.0);
	obs_data_set_default_int(settings, "ed_background", 1);
	obs_data_set_default_int(settings, "ed_bg_color", 0xFFFFFFFF);
	obs_data_set_default_double(settings, "ed_bg_dim", 0.0);
	obs_data_set_default_int(settings, "ed_neon_fill", 1);
	obs_data_set_default_int(settings, "ed_neon_color", 0xFFFFE020);
	obs_data_set_default_double(settings, "ed_neon_intensity", 150.0);
	obs_data_set_default_double(settings, "ed_glow", 150.0);
	obs_data_set_default_double(settings, "ed_glow_radius", 16.0);
	obs_data_set_default_int(settings, "ed_pencil_color", 0xFF383838);
	obs_data_set_default_double(settings, "ed_sketch", 20.0);
	obs_data_set_default_double(settings, "ed_hatch", 60.0);
	obs_data_set_default_double(settings, "ed_hatch_size", 6.0);
	obs_data_set_default_double(settings, "ed_hatch_angle", 45.0);
	obs_data_set_default_double(settings, "ed_angle", 135.0);
	obs_data_set_default_double(settings, "ed_emboss", 100.0);
	obs_data_set_default_double(settings, "ed_emboss_size", 2.0);
	obs_data_set_default_bool(settings, "ed_dither", true);
	gradient_defaults(settings, ED_GRADIENT);
}

static void vis(obs_properties_t *props, const char *key, bool visible)
{
	obs_property_set_visible(obs_properties_get(props, key), visible);
}

static bool edge_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int m = (int)obs_data_get_int(settings, "ed_mode");
	const int method = (int)obs_data_get_int(settings, "ed_method");
	const bool dog = m == MODE_PENCIL || (m == MODE_TOON && method == 2);
	const int fill = (int)obs_data_get_int(settings, "ed_neon_fill");
	const int bg = (int)obs_data_get_int(settings, "ed_background");

	vis(props, "ed_method", m == MODE_TOON);
	vis(props, "ed_smooth", !dog);
	vis(props, "ed_color_edges", m == MODE_NEON || (m == MODE_TOON && method != 2));
	vis(props, "ed_detail", dog);
	vis(props, "ed_threshold", m != MODE_EMBOSS);
	vis(props, "ed_softness", m != MODE_EMBOSS);
	vis(props, "ed_thickness", m == MODE_TOON || m == MODE_NEON);
	vis(props, "ed_toon_group", m == MODE_TOON);
	vis(props, "ed_neon_group", m == MODE_NEON);
	vis(props, "ed_pencil_group", m == MODE_PENCIL);
	vis(props, "ed_emboss_group", m == MODE_EMBOSS);
	vis(props, "ed_neon_color", fill == 1);
	vis(props, "ed_bg_color", bg == 2);
	vis(props, "ed_bg_dim", bg == 1);
	vis(props, "ed_cel_levels", bg == 1);
	vis(props, "ed_cel_softness", bg == 1);
	gradient_set_visible(props, ED_GRADIENT, m == MODE_NEON && (fill == 2 || fill == 3));
	return true;
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static obs_properties_t *edge_properties(void *data)
{
	fx_filter_t *fx = data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *g;
	obs_property_t *p;

	p = obs_properties_add_list(props, "ed_mode", obs_module_text("Edge.Mode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Edge.Mode.Toon"), MODE_TOON);
	obs_property_list_add_int(p, obs_module_text("Edge.Mode.Neon"), MODE_NEON);
	obs_property_list_add_int(p, obs_module_text("Edge.Mode.Pencil"), MODE_PENCIL);
	obs_property_list_add_int(p, obs_module_text("Edge.Mode.Emboss"), MODE_EMBOSS);
	obs_property_set_modified_callback(p, edge_modified);
	slider(props, "ed_amount", "Fx.Amount", 0.0, 100.0, 0.1, "%");

	// Edge detection
	g = obs_properties_create();
	p = obs_properties_add_list(g, "ed_method", obs_module_text("Edge.Method"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Edge.Method.Sobel"), 1);
	obs_property_list_add_int(p, obs_module_text("Edge.Method.DoG"), 2);
	obs_property_set_modified_callback(p, edge_modified);
	slider(g, "ed_smooth", "Edge.Smooth", 0.0, 10.0, 0.05, "px");
	slider(g, "ed_detail", "Edge.Detail", 0.3, 10.0, 0.05, "px");
	slider(g, "ed_threshold", "Edge.Threshold", 0.0, 100.0, 0.1, "%");
	slider(g, "ed_softness", "Edge.Softness", 0.0, 100.0, 0.1, "%");
	slider(g, "ed_color_edges", "Edge.ColorEdges", 0.0, 200.0, 0.1, "%");
	slider(g, "ed_thickness", "Edge.Thickness", 1.0, 17.0, 0.1, "px");
	obs_properties_add_group(props, "ed_detect_group", obs_module_text("Edge.Detection"), OBS_GROUP_NORMAL, g);

	// Background
	g = obs_properties_create();
	p = obs_properties_add_list(g, "ed_background", obs_module_text("Edge.Background"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Edge.Background.Image"), 1);
	obs_property_list_add_int(p, obs_module_text("Edge.Background.Color"), 2);
	obs_property_list_add_int(p, obs_module_text("Edge.Background.Transparent"), 3);
	obs_property_set_modified_callback(p, edge_modified);
	obs_properties_add_color_alpha(g, "ed_bg_color", obs_module_text("Edge.BackgroundColor"));
	slider(g, "ed_bg_dim", "Edge.Dim", 0.0, 100.0, 0.1, "%");
	slider(g, "ed_cel_levels", "Edge.CelLevels", 0.0, 16.0, 1.0, NULL);
	slider(g, "ed_cel_softness", "Edge.CelSoftness", 0.0, 100.0, 0.1, "%");
	obs_properties_add_group(props, "ed_bg_group", obs_module_text("Edge.BackgroundGroup"), OBS_GROUP_NORMAL, g);

	// Toon
	g = obs_properties_create();
	obs_properties_add_color_alpha(g, "ed_ink_color", obs_module_text("Edge.InkColor"));
	obs_properties_add_group(props, "ed_toon_group", obs_module_text("Edge.Mode.Toon"), OBS_GROUP_NORMAL, g);

	// Neon
	g = obs_properties_create();
	p = obs_properties_add_list(g, "ed_neon_fill", obs_module_text("Edge.NeonFill"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Edge.NeonFill.Color"), 1);
	obs_property_list_add_int(p, obs_module_text("Edge.NeonFill.Direction"), 2);
	obs_property_list_add_int(p, obs_module_text("Edge.NeonFill.Across"), 3);
	obs_property_list_add_int(p, obs_module_text("Edge.NeonFill.Source"), 4);
	obs_property_set_modified_callback(p, edge_modified);
	obs_properties_add_color(g, "ed_neon_color", obs_module_text("Edge.NeonColor"));
	slider(g, "ed_neon_intensity", "Edge.NeonIntensity", 0.0, 400.0, 0.1, "%");
	slider(g, "ed_glow", "Edge.Glow", 0.0, 400.0, 0.1, "%");
	slider(g, "ed_glow_radius", "Edge.GlowRadius", 1.0, 128.0, 0.1, "px");
	obs_properties_add_group(props, "ed_neon_group", obs_module_text("Edge.Mode.Neon"), OBS_GROUP_NORMAL, g);
	gradient_properties_ex(props, ED_GRADIENT, fx ? &fx->gradient : NULL, GRADIENT_PROPS_MAP);

	// Pencil
	g = obs_properties_create();
	obs_properties_add_color_alpha(g, "ed_pencil_color", obs_module_text("Edge.PencilColor"));
	slider(g, "ed_sketch", "Edge.Sketch", 0.0, 100.0, 0.1, NULL);
	slider(g, "ed_hatch", "Edge.Hatch", 0.0, 100.0, 0.1, "%");
	slider(g, "ed_hatch_size", "Edge.HatchSize", 2.0, 32.0, 0.1, "px");
	slider(g, "ed_hatch_angle", "Edge.HatchAngle", -180.0, 180.0, 0.1, "°");
	obs_properties_add_group(props, "ed_pencil_group", obs_module_text("Edge.Mode.Pencil"), OBS_GROUP_NORMAL, g);

	// Emboss
	g = obs_properties_create();
	slider(g, "ed_angle", "Bevel.Angle", -180.0, 180.0, 0.1, "°");
	slider(g, "ed_emboss", "Edge.EmbossStrength", 0.0, 1000.0, 0.1, "%");
	slider(g, "ed_emboss_size", "Edge.EmbossSize", 0.25, 16.0, 0.05, "px");
	obs_properties_add_group(props, "ed_emboss_group", obs_module_text("Edge.Mode.Emboss"), OBS_GROUP_NORMAL, g);

	obs_properties_add_bool(props, "ed_dither", obs_module_text("Gradient.Dither"));
	spectrum_presets_properties(props, fx ? fx->context : NULL);
	return props;
}

struct obs_source_info spectrum_edge_filter = {
	.id = "spectrum_edge_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = edge_name,
	.create = edge_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = edge_render,
	.get_properties = edge_properties,
	.get_defaults = edge_defaults,
};
