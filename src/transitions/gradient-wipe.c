// Spectrum Gradient Wipe: a soft linear, split or clock wipe with an edge
// glow filled with a color or the gradient engine.

#include "transition-common.h"

static const struct tr_def gradient_wipe_def = {
	.fx =
		{
			.effect_file = "shaders/transitions/gradient-wipe.effect",
			.gradient_prefix = TR_EDGE_PREFIX,
		},
};

static const char *gradient_wipe_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("GradientWipeTransition");
}

static void *gradient_wipe_create(obs_data_t *settings, obs_source_t *source)
{
	return tr_create(&gradient_wipe_def, settings, source);
}

static void gradient_wipe_defaults(obs_data_t *settings)
{
	tr_defaults_common(settings);
	obs_data_set_default_int(settings, "gw_type", 1);
	obs_data_set_default_double(settings, "gw_angle", 0.0);
	obs_data_set_default_double(settings, "gw_center_x", 50.0);
	obs_data_set_default_double(settings, "gw_center_y", 50.0);
	obs_data_set_default_double(settings, "gw_softness", 120.0);
	obs_data_set_default_bool(settings, "gw_reverse", false);
	tr_edge_defaults(settings, 2, 60.0, 100.0);
}

static bool type_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int type = (int)obs_data_get_int(settings, "gw_type");
	obs_property_set_visible(obs_properties_get(props, "gw_center_x"), type != 1);
	obs_property_set_visible(obs_properties_get(props, "gw_center_y"), type != 1);
	return true;
}

static obs_properties_t *gradient_wipe_properties(void *data)
{
	fx_filter_t *fx = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(props, "gw_type", obs_module_text("GradientWipe.Type"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("GradientWipe.Type.Linear"), 1);
	obs_property_list_add_int(p, obs_module_text("GradientWipe.Type.Split"), 2);
	obs_property_list_add_int(p, obs_module_text("GradientWipe.Type.Clock"), 3);
	obs_property_set_modified_callback(p, type_modified);

	tr_add_easing_property(props);
	tr_slider(props, "gw_angle", "Gradient.Angle", -360.0, 360.0, 0.1, "°");
	tr_slider(props, "gw_center_x", "Gradient.CenterX", 0.0, 100.0, 0.1, "%");
	tr_slider(props, "gw_center_y", "Gradient.CenterY", 0.0, 100.0, 0.1, "%");
	tr_slider(props, "gw_softness", "Transition.Softness", 0.0, 2000.0, 0.5, "px");
	obs_properties_add_bool(props, "gw_reverse", obs_module_text("Transition.Reverse"));

	tr_add_edge_properties(props, fx, "px", 1000.0);
	tr_finish_properties(props, data);
	return props;
}

struct obs_source_info spectrum_gradient_wipe_transition = {
	.id = "spectrum_gradient_wipe_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = gradient_wipe_name,
	.create = gradient_wipe_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = tr_video_render,
	.audio_render = tr_audio_render,
	.get_properties = gradient_wipe_properties,
	.get_defaults = gradient_wipe_defaults,
	.video_get_color_space = tr_video_get_color_space,
};
