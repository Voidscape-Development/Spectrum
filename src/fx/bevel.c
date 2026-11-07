// Spectrum Bevel & Emboss: lit bevels and embossing from the source's alpha.

#include "../common/fx-filter.h"

#include <math.h>
#include "../common/presets.h"

static uint32_t bevel_padding(obs_data_t *settings)
{
	const int style = (int)obs_data_get_int(settings, "bv_style");
	if (style == 1)
		return 0;
	return (uint32_t)ceil(obs_data_get_double(settings, "bv_size")) + 2;
}

static const struct fx_filter_def bevel_def = {
	.effect_file = "shaders/fx/bevel.effect",
	.dither_key = "bv_dither",
	.padding = bevel_padding,
};

static const char *bevel_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BevelFilter");
}

static void *bevel_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&bevel_def, settings, source);
}

static void bevel_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "bv_style", 1);
	obs_data_set_default_double(settings, "bv_size", 10.0);
	obs_data_set_default_double(settings, "bv_depth", 100.0);
	obs_data_set_default_double(settings, "bv_angle", 135.0);
	obs_data_set_default_double(settings, "bv_altitude", 30.0);
	obs_data_set_default_int(settings, "bv_highlight", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "bv_shadow", 0xFF000000);
	obs_data_set_default_double(settings, "bv_highlight_amount", 75.0);
	obs_data_set_default_double(settings, "bv_shadow_amount", 75.0);
	obs_data_set_default_bool(settings, "bv_invert", false);
	obs_data_set_default_bool(settings, "bv_dither", true);
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static obs_properties_t *bevel_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "bv_style", obs_module_text("Bevel.Style"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Bevel.Style.Inner"), 1);
	obs_property_list_add_int(p, obs_module_text("Bevel.Style.Outer"), 2);
	obs_property_list_add_int(p, obs_module_text("Bevel.Style.Emboss"), 3);
	obs_property_list_add_int(p, obs_module_text("Bevel.Style.Pillow"), 4);
	slider(props, "bv_size", "Bevel.Size", 0.5, 100.0, 0.5, "px");
	slider(props, "bv_depth", "Bevel.Depth", 0.0, 1000.0, 1.0, "%");
	obs_properties_add_bool(props, "bv_invert", obs_module_text("Bevel.Invert"));
	slider(props, "bv_angle", "Bevel.Angle", -180.0, 180.0, 0.1, "°");
	slider(props, "bv_altitude", "Bevel.Altitude", 0.0, 90.0, 0.1, "°");
	obs_properties_add_color(props, "bv_highlight", obs_module_text("Bevel.Highlight"));
	slider(props, "bv_highlight_amount", "Bevel.HighlightAmount", 0.0, 100.0, 0.1, "%");
	obs_properties_add_color(props, "bv_shadow", obs_module_text("Bevel.ShadowColor"));
	slider(props, "bv_shadow_amount", "Bevel.ShadowAmount", 0.0, 100.0, 0.1, "%");
	obs_properties_add_bool(props, "bv_dither", obs_module_text("Gradient.Dither"));
	spectrum_presets_properties(props, data ? ((fx_filter_t *)data)->context : NULL);
	return props;
}

struct obs_source_info spectrum_bevel_filter = {
	.id = "spectrum_bevel_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = bevel_name,
	.create = bevel_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = fx_cb_render,
	.get_properties = bevel_properties,
	.get_defaults = bevel_defaults,
};
