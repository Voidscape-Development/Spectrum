// Spectrum Glitch, Pixelate and Noise Dissolve transitions: single pass
// effects that peak or burn through at the middle of the transition.

#include "transition-common.h"

/* ------------------------------------------------------------------------
 * Glitch
 * --------------------------------------------------------------------- */

static const struct tr_def glitch_def = {
	.fx = {.effect_file = "shaders/transitions/glitch.effect"},
};

static const char *glitch_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("GlitchTransition");
}

static void *glitch_create(obs_data_t *settings, obs_source_t *source)
{
	return tr_create(&glitch_def, settings, source);
}

static void glitch_defaults(obs_data_t *settings)
{
	tr_defaults_common(settings);
	obs_data_set_default_int(settings, "tr_easing", TR_EASE_LINEAR);
	obs_data_set_default_double(settings, "gt_amount", 60.0);
	obs_data_set_default_double(settings, "gt_block", 40.0);
	obs_data_set_default_double(settings, "gt_split", 16.0);
	obs_data_set_default_double(settings, "gt_rate", 12.0);
	obs_data_set_default_double(settings, "gt_switch", 40.0);
	obs_data_set_default_double(settings, "gt_noise", 30.0);
	obs_data_set_default_double(settings, "gt_seed", 0.0);
}

static obs_properties_t *glitch_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	tr_add_easing_property(props);
	tr_slider(props, "gt_amount", "Glitch.Amount", 0.0, 500.0, 0.1, "px");
	tr_slider(props, "gt_block", "Glitch.BlockSize", 2.0, 400.0, 0.1, "px");
	tr_slider(props, "gt_split", "Glitch.Split", 0.0, 200.0, 0.1, "px");
	tr_slider(props, "gt_rate", "Glitch.Rate", 0.5, 60.0, 0.1, "/s");
	obs_property_t *p = tr_slider(props, "gt_switch", "Glitch.Switch", 0.0, 100.0, 0.1, "%");
	obs_property_set_long_description(p, obs_module_text("Glitch.Switch.Description"));
	tr_slider(props, "gt_noise", "Glitch.Noise", 0.0, 100.0, 0.1, "%");
	tr_slider(props, "gt_seed", "Noise.Seed", 0.0, 1000.0, 1.0, NULL);
	return props;
}

struct obs_source_info spectrum_glitch_transition = {
	.id = "spectrum_glitch_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = glitch_name,
	.create = glitch_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = tr_video_render,
	.audio_render = tr_audio_render,
	.get_properties = glitch_properties,
	.get_defaults = glitch_defaults,
	.video_get_color_space = tr_video_get_color_space,
};

/* ------------------------------------------------------------------------
 * Pixelate
 * --------------------------------------------------------------------- */

static const struct tr_def pixelate_def = {
	.fx = {.effect_file = "shaders/transitions/pixelate.effect"},
};

static const char *pixelate_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("PixelateTransition");
}

static void *pixelate_create(obs_data_t *settings, obs_source_t *source)
{
	return tr_create(&pixelate_def, settings, source);
}

static void pixelate_defaults(obs_data_t *settings)
{
	tr_defaults_common(settings);
	obs_data_set_default_double(settings, "px_size", 48.0);
	obs_data_set_default_double(settings, "px_crossfade", 30.0);
	obs_data_set_default_double(settings, "px_center_x", 50.0);
	obs_data_set_default_double(settings, "px_center_y", 50.0);
}

static obs_properties_t *pixelate_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	tr_add_easing_property(props);
	tr_slider(props, "px_size", "Pixelate.Size", 2.0, 512.0, 0.1, "px");
	obs_property_t *p = tr_slider(props, "px_crossfade", "BlurDissolve.Crossfade", 0.0, 100.0, 0.1, "%");
	obs_property_set_long_description(p, obs_module_text("BlurDissolve.Crossfade.Description"));
	tr_slider(props, "px_center_x", "Gradient.CenterX", 0.0, 100.0, 0.1, "%");
	tr_slider(props, "px_center_y", "Gradient.CenterY", 0.0, 100.0, 0.1, "%");
	return props;
}

struct obs_source_info spectrum_pixelate_transition = {
	.id = "spectrum_pixelate_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = pixelate_name,
	.create = pixelate_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = tr_video_render,
	.audio_render = tr_audio_render,
	.get_properties = pixelate_properties,
	.get_defaults = pixelate_defaults,
	.video_get_color_space = tr_video_get_color_space,
};

/* ------------------------------------------------------------------------
 * Noise Dissolve
 * --------------------------------------------------------------------- */

static const struct tr_def noise_dissolve_def = {
	.fx =
		{
			.effect_file = "shaders/transitions/noise-dissolve.effect",
			.gradient_prefix = TR_EDGE_PREFIX,
		},
};

static const char *noise_dissolve_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("NoiseDissolveTransition");
}

static void *noise_dissolve_create(obs_data_t *settings, obs_source_t *source)
{
	return tr_create(&noise_dissolve_def, settings, source);
}

// A fire gradient for the burn edge: embers on the incoming side, white-hot
// at the front, orange on the outgoing side.
static void set_fire_stops(obs_data_t *settings)
{
	static const struct {
		double position;
		uint32_t color; // 0xAABBGGRR
	} stops[] = {
		{0.0, 0xFF0818B0}, {0.35, 0xFF1070FF}, {0.5, 0xFFC0F8FF}, {0.65, 0xFF20B0FF}, {1.0, 0xFF0830D0},
	};
	obs_data_array_t *array = obs_data_array_create();
	for (size_t i = 0; i < OBS_COUNTOF(stops); i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_double(item, "position", stops[i].position);
		obs_data_set_int(item, "color", stops[i].color);
		obs_data_set_double(item, "midpoint", 0.5);
		obs_data_set_int(item, "ease", 1);
		obs_data_array_push_back(array, item);
		obs_data_release(item);
	}
	obs_data_set_default_array(settings, TR_EDGE_PREFIX "stops", array);
	obs_data_array_release(array);
}

static void noise_dissolve_defaults(obs_data_t *settings)
{
	tr_defaults_common(settings);
	obs_data_set_default_int(settings, "tr_easing", TR_EASE_LINEAR);
	obs_data_set_default_double(settings, "nd_scale", 120.0);
	obs_data_set_default_int(settings, "nd_detail", 4);
	obs_data_set_default_double(settings, "nd_softness", 1.0);
	obs_data_set_default_double(settings, "nd_evolve", 0.5);
	obs_data_set_default_double(settings, "nd_seed", 0.0);
	obs_data_set_default_double(settings, "nd_bias", 0.0);
	obs_data_set_default_double(settings, "nd_angle", 0.0);
	tr_edge_defaults(settings, 2, 5.0, 200.0);
	set_fire_stops(settings);
}

static obs_properties_t *noise_dissolve_properties(void *data)
{
	fx_filter_t *fx = data;
	obs_properties_t *props = obs_properties_create();
	tr_add_easing_property(props);
	tr_slider(props, "nd_scale", "Noise.Scale", 4.0, 4000.0, 1.0, "px");
	obs_properties_add_int_slider(props, "nd_detail", obs_module_text("Noise.Octaves"), 1, 8, 1);
	tr_slider(props, "nd_softness", "Transition.Softness", 0.0, 100.0, 0.1, "%");
	tr_slider(props, "nd_evolve", "Noise.Evolve", 0.0, 5.0, 0.01, NULL);
	tr_slider(props, "nd_seed", "Noise.Seed", 0.0, 1000.0, 1.0, NULL);
	obs_property_t *p = tr_slider(props, "nd_bias", "NoiseDissolve.Bias", 0.0, 100.0, 0.1, "%");
	obs_property_set_long_description(p, obs_module_text("NoiseDissolve.Bias.Description"));
	tr_slider(props, "nd_angle", "Gradient.Angle", -360.0, 360.0, 0.1, "°");
	tr_add_edge_properties(props, fx, "%", 50.0);
	return props;
}

struct obs_source_info spectrum_noise_dissolve_transition = {
	.id = "spectrum_noise_dissolve_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = noise_dissolve_name,
	.create = noise_dissolve_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = tr_video_render,
	.audio_render = tr_audio_render,
	.get_properties = noise_dissolve_properties,
	.get_defaults = noise_dissolve_defaults,
	.video_get_color_space = tr_video_get_color_space,
};
