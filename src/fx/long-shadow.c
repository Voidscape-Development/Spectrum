// Spectrum Long Shadow: a flat-design long shadow or a 3D extrusion of the
// source along a direction, drawn outside the source's bounds.

#include "../common/fx-filter.h"

#include <math.h>
#include "../common/presets.h"

#define LS_PREFIX "ls_gradient_"

static uint32_t long_shadow_padding(obs_data_t *settings)
{
	const double len = obs_data_get_double(settings, "ls_length");
	return len > 0.0 ? (uint32_t)ceil(len) + 2 : 0;
}

static const struct fx_filter_def long_shadow_def = {
	.effect_file = "shaders/fx/long-shadow.effect",
	.gradient_prefix = LS_PREFIX,
	.dither_key = "ls_dither",
	.padding = long_shadow_padding,
};

static const char *long_shadow_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LongShadowFilter");
}

static void *long_shadow_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&long_shadow_def, settings, source);
}

static void long_shadow_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "ls_angle", 45.0);
	obs_data_set_default_double(settings, "ls_length", 120.0);
	obs_data_set_default_int(settings, "ls_fill", 1);
	obs_data_set_default_int(settings, "ls_color", 0xA0000000);
	obs_data_set_default_double(settings, "ls_shade", 60.0);
	obs_data_set_default_double(settings, "ls_fade", 100.0);
	obs_data_set_default_double(settings, "ls_threshold", 50.0);
	obs_data_set_default_bool(settings, "ls_behind_only", false);
	obs_data_set_default_double(settings, "ls_opacity", 100.0);
	obs_data_set_default_bool(settings, "ls_dither", true);
	gradient_defaults(settings, LS_PREFIX);
}

static bool fill_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int fill = (int)obs_data_get_int(settings, "ls_fill");
	obs_property_set_visible(obs_properties_get(props, "ls_color"), fill == 1);
	obs_property_set_visible(obs_properties_get(props, "ls_shade"), fill == 3);
	gradient_set_visible(props, LS_PREFIX, fill == 2);
	return true;
}

static obs_properties_t *long_shadow_properties(void *data)
{
	fx_filter_t *fx = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_float_slider(props, "ls_angle", obs_module_text("LongShadow.Angle"), -180.0, 180.0, 0.1);
	obs_property_float_set_suffix(p, "°");
	p = obs_properties_add_float_slider(props, "ls_length", obs_module_text("LongShadow.Length"), 0.0, 1000.0, 1.0);
	obs_property_float_set_suffix(p, "px");

	p = obs_properties_add_list(props, "ls_fill", obs_module_text("StrokeFilter.FillType"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("StrokeFilter.ColorFill"), 1);
	obs_property_list_add_int(p, obs_module_text("LongShadow.Fill.Gradient"), 2);
	obs_property_list_add_int(p, obs_module_text("LongShadow.Fill.Extrude"), 3);
	obs_property_set_modified_callback(p, fill_modified);
	obs_properties_add_color_alpha(props, "ls_color", obs_module_text("StrokeFilter.ColorFill"));
	gradient_properties_ex(props, LS_PREFIX, fx ? &fx->gradient : NULL, GRADIENT_PROPS_MAP);
	p = obs_properties_add_float_slider(props, "ls_shade", obs_module_text("LongShadow.Shade"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(props, "ls_fade", obs_module_text("LongShadow.Fade"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(props, "ls_threshold", obs_module_text("LongShadow.Threshold"), 0.0, 100.0,
					    0.1);
	obs_property_float_set_suffix(p, "%");
	p = obs_properties_add_float_slider(props, "ls_opacity", obs_module_text("Gradient.Opacity"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");
	obs_properties_add_bool(props, "ls_behind_only", obs_module_text("LongShadow.ShadowOnly"));
	obs_properties_add_bool(props, "ls_dither", obs_module_text("Gradient.Dither"));
	spectrum_presets_properties(props, data ? ((fx_filter_t *)data)->context : NULL);
	return props;
}

struct obs_source_info spectrum_long_shadow_filter = {
	.id = "spectrum_long_shadow_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = long_shadow_name,
	.create = long_shadow_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = fx_cb_render,
	.get_properties = long_shadow_properties,
	.get_defaults = long_shadow_defaults,
};
