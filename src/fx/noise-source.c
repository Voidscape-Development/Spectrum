// Spectrum Noise: an animated procedural noise source, mapped through a
// gradient.

#include "../common/fx-filter.h"
#include "../common/presets.h"

#define NZ_PREFIX "nz_gradient_"

static const struct fx_filter_def noise_def = {
	.effect_file = "shaders/fx/noise.effect",
	.gradient_prefix = NZ_PREFIX,
};

static const char *noise_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("NoiseSource");
}

static void *noise_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&noise_def, settings, source);
}

static void noise_defaults(obs_data_t *settings)
{
	fx_size_defaults(settings);
	obs_data_set_default_int(settings, "nz_type", 3);
	obs_data_set_default_int(settings, "nz_fractal", 1);
	obs_data_set_default_double(settings, "nz_scale", 300.0);
	obs_data_set_default_int(settings, "nz_octaves", 4);
	obs_data_set_default_double(settings, "nz_lacunarity", 2.0);
	obs_data_set_default_double(settings, "nz_gain", 50.0);
	obs_data_set_default_double(settings, "nz_warp", 30.0);
	obs_data_set_default_double(settings, "nz_evolve", 0.15);
	obs_data_set_default_double(settings, "nz_flow_x", 0.0);
	obs_data_set_default_double(settings, "nz_flow_y", 0.0);
	obs_data_set_default_double(settings, "nz_seed", 0.0);
	obs_data_set_default_double(settings, "nz_contrast", 100.0);
	obs_data_set_default_double(settings, "nz_brightness", 0.0);
	obs_data_set_default_double(settings, "nz_aspect", 100.0);
	obs_data_set_default_double(settings, "nz_opacity", 100.0);
	gradient_defaults(settings, NZ_PREFIX);
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static obs_properties_t *noise_properties(void *data)
{
	fx_filter_t *fx = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	fx_add_size_properties(props);

	p = obs_properties_add_list(props, "nz_type", obs_module_text("Noise.Type"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Noise.Type.Value"), 1);
	obs_property_list_add_int(p, obs_module_text("Noise.Type.Perlin"), 2);
	obs_property_list_add_int(p, obs_module_text("Noise.Type.Simplex"), 3);
	obs_property_list_add_int(p, obs_module_text("Noise.Type.Cells"), 4);
	obs_property_list_add_int(p, obs_module_text("Noise.Type.CellEdges"), 5);

	p = obs_properties_add_list(props, "nz_fractal", obs_module_text("Noise.Fractal"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Noise.Fractal.Fbm"), 1);
	obs_property_list_add_int(p, obs_module_text("Noise.Fractal.Turbulence"), 2);
	obs_property_list_add_int(p, obs_module_text("Noise.Fractal.Ridged"), 3);

	slider(props, "nz_scale", "Noise.Scale", 4.0, 4000.0, 1.0, "px");
	slider(props, "nz_aspect", "Gradient.Aspect", 10.0, 1000.0, 0.1, "%");
	obs_properties_add_int_slider(props, "nz_octaves", obs_module_text("Noise.Octaves"), 1, 8, 1);
	slider(props, "nz_lacunarity", "Noise.Lacunarity", 1.1, 4.0, 0.01, NULL);
	slider(props, "nz_gain", "Noise.Gain", 0.0, 100.0, 0.1, "%");
	slider(props, "nz_warp", "Noise.Warp", 0.0, 100.0, 0.1, "%");
	slider(props, "nz_evolve", "Noise.Evolve", 0.0, 5.0, 0.01, NULL);
	slider(props, "nz_flow_x", "Noise.FlowX", -1000.0, 1000.0, 0.1, "px/s");
	slider(props, "nz_flow_y", "Noise.FlowY", -1000.0, 1000.0, 0.1, "px/s");
	slider(props, "nz_seed", "Noise.Seed", 0.0, 1000.0, 1.0, NULL);
	slider(props, "nz_contrast", "Noise.Contrast", 0.0, 400.0, 0.1, "%");
	slider(props, "nz_brightness", "Noise.Brightness", -100.0, 100.0, 0.1, "%");

	gradient_properties_ex(props, NZ_PREFIX, fx ? &fx->gradient : NULL, GRADIENT_PROPS_MAP);

	slider(props, "nz_opacity", "Gradient.Opacity", 0.0, 100.0, 0.1, "%");
	spectrum_presets_properties(props, data ? ((fx_filter_t *)data)->context : NULL);
	return props;
}

struct obs_source_info spectrum_noise_source = {
	.id = "spectrum_noise_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.get_name = noise_name,
	.create = noise_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = fx_cb_render,
	.get_width = fx_cb_width,
	.get_height = fx_cb_height,
	.get_properties = noise_properties,
	.get_defaults = noise_defaults,
	.icon_type = OBS_ICON_TYPE_COLOR,
};
