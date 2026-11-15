// Spectrum Blur Dissolve: blurs the outgoing scene out and the incoming one
// in, crossfading while the blur peaks.

#include "transition-common.h"

#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

struct blur_data {
	gs_texrender_t *texrender[2];
};

static void blur_create_hook(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	fx->user = bzalloc(sizeof(struct blur_data));
}

static void blur_destroy_hook(fx_filter_t *fx)
{
	struct blur_data *d = fx->user;
	obs_enter_graphics();
	for (int i = 0; i < 2; i++)
		if (d->texrender[i])
			gs_texrender_destroy(d->texrender[i]);
	obs_leave_graphics();
	bfree(d);
}

static float smoothstepf(float e0, float e1, float x)
{
	if (e1 <= e0)
		return x < e0 ? 0.0f : 1.0f;
	float t = (x - e0) / (e1 - e0);
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	return t * t * (3.0f - 2.0f * t);
}

static bool begin_pass(gs_texrender_t *tr, uint32_t w, uint32_t h)
{
	gs_texrender_reset(tr);
	if (!gs_texrender_begin_with_color_space(tr, w, h, GS_CS_SRGB_16F))
		return false;
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	return true;
}

static void draw(gs_effect_t *effect, const char *tech, uint32_t w, uint32_t h)
{
	while (gs_effect_loop(effect, tech))
		gs_draw_sprite(NULL, 0, w, h);
}

static void set_blur_params(gs_effect_t *e, gs_texture_t *input, float sigma, int taps, float step_x, float step_y)
{
	fx_set_texture(e, "image", input);
	fx_set_float(e, "sigma", sigma);
	fx_set_int(e, "taps", taps);
	fx_set_vec2(e, "blur_step", step_x, step_y);
}

static void blur_render(fx_filter_t *fx, gs_effect_t *e, obs_data_t *settings, gs_texture_t *a, gs_texture_t *b,
			float progress, uint32_t cx, uint32_t cy)
{
	struct blur_data *d = fx->user;
	const float fade = (float)obs_data_get_double(settings, "bd_crossfade") / 100.0f;
	const float mix_w = smoothstepf(0.5f - 0.5f * fade, 0.5f + 0.5f * fade, progress);
	// The radius is about two standard deviations.
	const float sigma = 0.5f * (float)obs_data_get_double(settings, "bd_radius") * sinf((float)M_PI * progress);

	if (sigma < 0.35f) {
		tr_set_common(fx, e, settings, a, b, progress, progress, cx, cy);
		fx_set_float(e, "mix_w", mix_w);
		draw(e, "Mix", cx, cy);
		return;
	}

	// Blur at a lower resolution when the blur is wide: a few texels of
	// blur hide the downsampling, and it keeps the kernel short.
	uint32_t scale = 1;
	while (scale < 8 && sigma / (float)scale > 6.0f)
		scale *= 2;
	const uint32_t w = (cx + scale - 1) / scale, h = (cy + scale - 1) / scale;
	const float s = sigma / (float)scale;
	int taps = (int)ceilf(s * 3.0f);
	if (taps > 63)
		taps = 63;

	for (int i = 0; i < 2; i++)
		if (!d->texrender[i])
			d->texrender[i] = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);

	gs_enable_framebuffer_srgb(false);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	bool ok = begin_pass(d->texrender[0], w, h);
	if (ok) {
		tr_set_common(fx, e, settings, a, b, progress, progress, w, h);
		fx_set_float(e, "mix_w", mix_w);
		fx_set_vec2(e, "src_texel", 1.0f / (float)cx, 1.0f / (float)cy);
		fx_set_float(e, "down_scale", (float)scale);
		draw(e, "Down", w, h);
		gs_texrender_end(d->texrender[0]);
	}
	// OBS resets every parameter after each draw: set them all per pass.
	if (ok && (ok = begin_pass(d->texrender[1], w, h))) {
		set_blur_params(e, gs_texrender_get_texture(d->texrender[0]), s, taps, 1.0f / (float)w, 0.0f);
		draw(e, "Blur", w, h);
		gs_texrender_end(d->texrender[1]);
	}
	if (ok && (ok = begin_pass(d->texrender[0], w, h))) {
		set_blur_params(e, gs_texrender_get_texture(d->texrender[1]), s, taps, 0.0f, 1.0f / (float)h);
		draw(e, "Blur", w, h);
		gs_texrender_end(d->texrender[0]);
	}

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(true);

	if (ok) {
		fx_filter_set_common(fx, e, settings, NULL, 0.0f, cx, cy, true, 0.0f);
		fx_set_texture(e, "image", gs_texrender_get_texture(d->texrender[0]));
		draw(e, "Out", cx, cy);
	}
}

static const struct tr_def blur_dissolve_def = {
	.fx =
		{
			.effect_file = "shaders/transitions/blur-dissolve.effect",
			.create = blur_create_hook,
			.destroy = blur_destroy_hook,
		},
	.render = blur_render,
};

static const char *blur_dissolve_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BlurDissolveTransition");
}

static void *blur_dissolve_create(obs_data_t *settings, obs_source_t *source)
{
	return tr_create(&blur_dissolve_def, settings, source);
}

static void blur_dissolve_defaults(obs_data_t *settings)
{
	tr_defaults_common(settings);
	obs_data_set_default_double(settings, "bd_radius", 40.0);
	obs_data_set_default_double(settings, "bd_crossfade", 50.0);
}

static obs_properties_t *blur_dissolve_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	tr_add_easing_property(props);
	tr_slider(props, "bd_radius", "BlurDissolve.Radius", 0.0, 200.0, 0.1, "px");
	obs_property_t *p = tr_slider(props, "bd_crossfade", "BlurDissolve.Crossfade", 0.0, 100.0, 0.1, "%");
	obs_property_set_long_description(p, obs_module_text("BlurDissolve.Crossfade.Description"));
	tr_finish_properties(props, data);
	return props;
}

struct obs_source_info spectrum_blur_dissolve_transition = {
	.id = "spectrum_blur_dissolve_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = blur_dissolve_name,
	.create = blur_dissolve_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = tr_video_render,
	.audio_render = tr_audio_render,
	.get_properties = blur_dissolve_properties,
	.get_defaults = blur_dissolve_defaults,
	.video_get_color_space = tr_video_get_color_space,
};
