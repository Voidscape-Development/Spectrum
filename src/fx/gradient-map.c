// Spectrum Gradient Map: maps the brightness (or a channel) of a source
// through a gradient, for duotones, thermal looks and stylized grades.

#include "../common/fx-filter.h"
#include "../common/presets.h"

#define GM_PREFIX "gm_gradient_"

static const struct fx_filter_def gradient_map_def = {
	.effect_file = "shaders/fx/gradient-map.effect",
	.gradient_prefix = GM_PREFIX,
	.dither_key = "gm_dither",
};

static const char *gradient_map_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("GradientMapFilter");
}

static void *gradient_map_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&gradient_map_def, settings, source);
}

static void gradient_map_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "gm_source", 1);
	obs_data_set_default_double(settings, "gm_black", 0.0);
	obs_data_set_default_double(settings, "gm_white", 100.0);
	obs_data_set_default_double(settings, "gm_gamma", 1.0);
	obs_data_set_default_bool(settings, "gm_invert", false);
	obs_data_set_default_int(settings, "gm_blend", 1);
	obs_data_set_default_double(settings, "gm_amount", 100.0);
	obs_data_set_default_bool(settings, "gm_use_alpha", false);
	obs_data_set_default_bool(settings, "gm_dither", true);
	gradient_defaults(settings, GM_PREFIX);
}

static obs_properties_t *gradient_map_properties(void *data)
{
	fx_filter_t *fx = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "gm_source", obs_module_text("GradientMap.Source"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Luma"), 1);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Lightness"), 2);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Average"), 3);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Max"), 4);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Red"), 5);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Green"), 6);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Blue"), 7);
	obs_property_list_add_int(p, obs_module_text("GradientMap.Source.Alpha"), 8);

	p = obs_properties_add_float_slider(props, "gm_black", obs_module_text("GradientMap.Black"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(props, "gm_white", obs_module_text("GradientMap.White"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");
	obs_properties_add_float_slider(props, "gm_gamma", obs_module_text("GradientMap.Gamma"), 0.1, 5.0, 0.01);
	obs_properties_add_bool(props, "gm_invert", obs_module_text("GradientMap.Invert"));

	gradient_properties_ex(props, GM_PREFIX, fx ? &fx->gradient : NULL, GRADIENT_PROPS_MAP);

	fx_add_blend_list(props, "gm_blend");
	p = obs_properties_add_float_slider(props, "gm_amount", obs_module_text("Fx.Amount"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");
	obs_properties_add_bool(props, "gm_use_alpha", obs_module_text("GradientMap.UseAlpha"));
	obs_properties_add_bool(props, "gm_dither", obs_module_text("Gradient.Dither"));
	spectrum_presets_properties(props, data ? ((fx_filter_t *)data)->context : NULL);
	return props;
}

struct obs_source_info spectrum_gradient_map_filter = {
	.id = "spectrum_gradient_map_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = gradient_map_name,
	.create = gradient_map_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = fx_cb_render,
	.get_properties = gradient_map_properties,
	.get_defaults = gradient_map_defaults,
};
