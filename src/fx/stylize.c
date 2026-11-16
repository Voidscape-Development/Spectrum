// Spectrum Stylize: glitch, CRT curvature, chromatic aberration, posterize,
// halftone, scanlines, film grain and vignette, each off at zero.

#include "../common/fx-filter.h"

static const struct fx_filter_def stylize_def = {
	.effect_file = "shaders/fx/stylize.effect",
	.dither_key = "st_dither",
};

static const char *stylize_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("StylizeFilter");
}

static void *stylize_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&stylize_def, settings, source);
}

static void stylize_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "st_glitch_block", 24.0);
	obs_data_set_default_double(settings, "st_glitch_speed", 8.0);
	obs_data_set_default_bool(settings, "st_ca_radial", true);
	obs_data_set_default_double(settings, "st_halftone_size", 8.0);
	obs_data_set_default_double(settings, "st_halftone_angle", 45.0);
	obs_data_set_default_double(settings, "st_scanline_size", 3.0);
	obs_data_set_default_double(settings, "st_grain_size", 1.5);
	obs_data_set_default_double(settings, "st_vignette_size", 60.0);
	obs_data_set_default_double(settings, "st_vignette_softness", 50.0);
	obs_data_set_default_int(settings, "st_vignette_color", 0xFF000000);
	obs_data_set_default_bool(settings, "st_dither", true);
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static void group(obs_properties_t *props, const char *key, const char *text, obs_properties_t *content)
{
	obs_properties_add_group(props, key, obs_module_text(text), OBS_GROUP_NORMAL, content);
}

static obs_properties_t *stylize_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *g;

	g = obs_properties_create();
	slider(g, "st_ca", "Fx.Amount", 0.0, 50.0, 0.1, "px");
	obs_properties_add_bool(g, "st_ca_radial", obs_module_text("Stylize.CaRadial"));
	slider(g, "st_ca_angle", "LongShadow.Angle", -180.0, 180.0, 0.1, "°");
	group(props, "st_ca_group", "Stylize.Ca", g);

	g = obs_properties_create();
	slider(g, "st_glitch", "Fx.Amount", 0.0, 100.0, 0.1, "%");
	slider(g, "st_glitch_block", "Stylize.GlitchBlock", 1.0, 200.0, 1.0, "px");
	slider(g, "st_glitch_speed", "Distort.Speed", 0.0, 60.0, 0.1, "/s");
	group(props, "st_glitch_group", "Stylize.Glitch", g);

	g = obs_properties_create();
	slider(g, "st_crt_curve", "Stylize.CrtCurve", 0.0, 100.0, 0.1, "%");
	slider(g, "st_scanlines", "Stylize.Scanlines", 0.0, 100.0, 0.1, "%");
	slider(g, "st_scanline_size", "Stylize.ScanlineSize", 1.0, 20.0, 0.1, "px");
	group(props, "st_crt_group", "Stylize.Crt", g);

	g = obs_properties_create();
	slider(g, "st_halftone", "Fx.Amount", 0.0, 100.0, 0.1, "%");
	slider(g, "st_halftone_size", "Stylize.DotSize", 2.0, 64.0, 0.1, "px");
	slider(g, "st_halftone_angle", "LongShadow.Angle", -90.0, 90.0, 0.1, "°");
	obs_properties_add_bool(g, "st_halftone_cmyk", obs_module_text("Stylize.Cmyk"));
	group(props, "st_halftone_group", "Stylize.Halftone", g);

	g = obs_properties_create();
	slider(g, "st_posterize", "Stylize.Levels", 0.0, 32.0, 1.0, NULL);
	obs_properties_add_bool(g, "st_posterize_dither", obs_module_text("Stylize.PosterizeDither"));
	group(props, "st_posterize_group", "Stylize.Posterize", g);

	g = obs_properties_create();
	slider(g, "st_grain", "Fx.Amount", 0.0, 100.0, 0.1, "%");
	slider(g, "st_grain_size", "Gradient.GrainSize", 1.0, 8.0, 0.1, "px");
	group(props, "st_grain_group", "Stylize.Grain", g);

	g = obs_properties_create();
	slider(g, "st_vignette", "Fx.Amount", 0.0, 100.0, 0.1, "%");
	slider(g, "st_vignette_size", "Distort.Radius", 0.0, 150.0, 0.1, "%");
	slider(g, "st_vignette_softness", "Stylize.Softness", 0.0, 200.0, 0.1, "%");
	obs_properties_add_color_alpha(g, "st_vignette_color", obs_module_text("Stylize.VignetteColor"));
	group(props, "st_vignette_group", "Stylize.Vignette", g);

	obs_properties_add_bool(props, "st_dither", obs_module_text("Gradient.Dither"));
	return props;
}

struct obs_source_info spectrum_stylize_filter = {
	.id = "spectrum_stylize_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = stylize_name,
	.create = stylize_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = fx_cb_render,
	.get_properties = stylize_properties,
	.get_defaults = stylize_defaults,
};
