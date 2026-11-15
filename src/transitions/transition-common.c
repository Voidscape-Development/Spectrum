#include "transition-common.h"
#include "../common/presets.h"

#include <math.h>

float tr_ease(int easing, float t)
{
	t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
	switch (easing) {
	case TR_EASE_SMOOTH:
		return t * t * (3.0f - 2.0f * t);
	case TR_EASE_IN_OUT:
		return t < 0.5f ? 4.0f * t * t * t : 1.0f - powf(-2.0f * t + 2.0f, 3.0f) * 0.5f;
	case TR_EASE_IN:
		return t * t * t;
	case TR_EASE_OUT:
		return 1.0f - powf(1.0f - t, 3.0f);
	default:
		return t;
	}
}

void tr_set_common(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, gs_texture_t *a, gs_texture_t *b,
		   float progress, float raw, uint32_t cx, uint32_t cy)
{
	// The scenes are sampled through their sRGB views (linear light) and
	// the output is linear, encoded by the sRGB framebuffer. Float
	// (HDR / 16-bit) textures are linear already and are sampled as is.
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, "tex_a");
	if (p)
		gs_effect_set_texture_srgb(p, a);
	p = gs_effect_get_param_by_name(effect, "tex_b");
	if (p)
		gs_effect_set_texture_srgb(p, b);
	fx_set_float(effect, "progress", progress);
	fx_set_float(effect, "raw_progress", raw);
	fx_filter_set_common(fx, effect, settings, NULL, 0.0f, cx, cy, true, 0.0f);
}

static void tr_callback(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	fx_filter_t *fx = data;
	const struct tr_def *def = (const struct tr_def *)fx->def;
	if (!fx_filter_ensure_effect(fx))
		return;

	obs_data_t *settings = fx_filter_settings(fx);
	const float progress = tr_ease((int)obs_data_get_int(settings, "tr_easing"), t);

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	if (def->render) {
		def->render(fx, fx->effect, settings, a, b, progress, cx, cy);
	} else {
		tr_set_common(fx, fx->effect, settings, a, b, progress, t, cx, cy);
		while (gs_effect_loop(fx->effect, "Draw"))
			gs_draw_sprite(NULL, 0, cx, cy);
	}
	gs_enable_framebuffer_srgb(previous);
	obs_data_release(settings);
}

void *tr_create(const struct tr_def *def, obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&def->fx, settings, source);
}

void tr_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	fx_filter_t *fx = data;
	obs_transition_video_render(fx->context, tr_callback);
}

static float mix_a(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return 1.0f - t;
}

static float mix_b(void *data, float t)
{
	UNUSED_PARAMETER(data);
	return t;
}

bool tr_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers, size_t channels,
		     size_t sample_rate)
{
	fx_filter_t *fx = data;
	return obs_transition_audio_render(fx->context, ts_out, audio, mixers, channels, sample_rate, mix_a, mix_b);
}

enum gs_color_space tr_video_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);
	fx_filter_t *fx = data;
	return obs_transition_video_get_color_space(fx->context);
}

/* ------------------------------------------------------------------------
 * Properties
 * --------------------------------------------------------------------- */

obs_property_t *tr_slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			  double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

void tr_defaults_common(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "tr_easing", TR_EASE_SMOOTH);
}

void tr_add_easing_property(obs_properties_t *props)
{
	obs_property_t *p = obs_properties_add_list(props, "tr_easing", obs_module_text("Transition.Easing"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Gradient.Ease.Linear"), TR_EASE_LINEAR);
	obs_property_list_add_int(p, obs_module_text("Gradient.Ease.Smooth"), TR_EASE_SMOOTH);
	obs_property_list_add_int(p, obs_module_text("Gradient.Ease.InOut"), TR_EASE_IN_OUT);
	obs_property_list_add_int(p, obs_module_text("Gradient.Ease.In"), TR_EASE_IN);
	obs_property_list_add_int(p, obs_module_text("Gradient.Ease.Out"), TR_EASE_OUT);
}

void tr_edge_defaults(obs_data_t *settings, int mode, double width, double intensity)
{
	obs_data_set_default_int(settings, "tr_edge_mode", mode);
	obs_data_set_default_int(settings, "tr_edge_style", 1);
	obs_data_set_default_int(settings, "tr_edge_color", 0xFF40C0FF);
	obs_data_set_default_double(settings, "tr_edge_width", width);
	obs_data_set_default_double(settings, "tr_edge_intensity", intensity);
	gradient_defaults(settings, TR_EDGE_PREFIX);
}

static bool edge_mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int mode = (int)obs_data_get_int(settings, "tr_edge_mode");
	obs_property_set_visible(obs_properties_get(props, "tr_edge_style"), mode != 0);
	obs_property_set_visible(obs_properties_get(props, "tr_edge_color"), mode == 1);
	obs_property_set_visible(obs_properties_get(props, "tr_edge_width"), mode != 0);
	obs_property_set_visible(obs_properties_get(props, "tr_edge_intensity"), mode != 0);
	gradient_set_visible(props, TR_EDGE_PREFIX, mode == 2);
	return true;
}

void tr_add_edge_properties(obs_properties_t *props, fx_filter_t *fx, const char *width_suffix, double max_width)
{
	obs_properties_t *group = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(group, "tr_edge_mode", obs_module_text("Transition.Edge"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Transition.Edge.None"), 0);
	obs_property_list_add_int(p, obs_module_text("Transition.Edge.Color"), 1);
	obs_property_list_add_int(p, obs_module_text("Transition.Edge.Gradient"), 2);
	obs_property_set_modified_callback(p, edge_mode_modified);

	p = obs_properties_add_list(group, "tr_edge_style", obs_module_text("Transition.EdgeStyle"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Transition.EdgeStyle.Glow"), 1);
	obs_property_list_add_int(p, obs_module_text("Transition.EdgeStyle.Solid"), 2);
	obs_property_set_long_description(p, obs_module_text("Transition.EdgeStyle.Description"));

	obs_properties_add_color_alpha(group, "tr_edge_color", obs_module_text("Transition.EdgeColor"));
	tr_slider(group, "tr_edge_width", "Transition.EdgeWidth", 0.0, max_width, 0.1, width_suffix);
	tr_slider(group, "tr_edge_intensity", "Transition.EdgeIntensity", 0.0, 400.0, 0.1, "%");
	obs_properties_add_group(props, "tr_edge_group", obs_module_text("Transition.EdgeGroup"), OBS_GROUP_NORMAL,
				 group);

	gradient_properties_ex(props, TR_EDGE_PREFIX, fx ? &fx->gradient : NULL, GRADIENT_PROPS_MAP);
}

void tr_finish_properties(obs_properties_t *props, void *data)
{
	spectrum_presets_properties(props, data ? ((fx_filter_t *)data)->context : NULL);
}
