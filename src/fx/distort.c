// Spectrum Distort: wave, ripple, twirl, bulge/pinch, noise displacement and
// displacement by another source.

#include "../common/fx-filter.h"
#include "../stroke/obs-utils.h"

#define MODE_MAP 6

struct distort_data {
	struct source_ref map_ref;
	gs_texrender_t *map_texrender;
	int mode;
};

static void distort_create_hook(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct distort_data *d = bzalloc(sizeof(struct distort_data));
	source_ref_init(&d->map_ref);
	fx->user = d;
}

static void distort_update_hook(fx_filter_t *fx, obs_data_t *settings)
{
	struct distort_data *d = fx->user;
	d->mode = (int)obs_data_get_int(settings, "ds_mode");
	source_ref_set_name(&d->map_ref, obs_data_get_string(settings, "ds_map_source"));
}

static void distort_tick_hook(fx_filter_t *fx, float seconds)
{
	struct distort_data *d = fx->user;
	source_ref_tick(&d->map_ref, seconds);
	obs_source_t *owner = obs_filter_get_parent(fx->context);
	if (owner)
		source_ref_update_activation(&d->map_ref, owner, d->mode == MODE_MAP && obs_source_enabled(fx->context),
					     seconds);
}

static void distort_destroy_hook(fx_filter_t *fx)
{
	struct distort_data *d = fx->user;
	obs_enter_graphics();
	if (d->map_texrender)
		gs_texrender_destroy(d->map_texrender);
	obs_leave_graphics();
	source_ref_free(&d->map_ref);
	bfree(d);
}

// Renders the map source in float (linear light), so smooth height maps
// don't step in 1/255 increments, which shows as ridges once differentiated.
static gs_texture_t *render_map(obs_source_t *source, gs_texrender_t **texrender)
{
	const uint32_t width = obs_source_get_width(source);
	const uint32_t height = obs_source_get_height(source);
	if (!width || !height)
		return NULL;
	if (!*texrender)
		*texrender = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	gs_texrender_reset(*texrender);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin_with_color_space(*texrender, width, height, GS_CS_SRGB_16F)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		obs_source_video_render(source);
		gs_texrender_end(*texrender);
	}
	gs_blend_state_pop();
	return gs_texrender_get_texture(*texrender);
}

static void distort_set_params(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, obs_data_t *to, float t,
			       uint32_t width, uint32_t height)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(to);
	UNUSED_PARAMETER(t);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	struct distort_data *d = fx->user;
	gs_texture_t *map = NULL;
	if (d->mode == MODE_MAP) {
		obs_source_t *source = source_ref_get(&d->map_ref);
		if (source && !is_self_reference(fx->context, source))
			map = render_map(source, &d->map_texrender);
		obs_source_release(source);
	}
	fx_set_texture(effect, "map_tex", map);
	fx_set_float(effect, "map_enabled", map ? 1.0f : 0.0f);
}

static const struct fx_filter_def distort_def = {
	.effect_file = "shaders/fx/distort.effect",
	.dither_key = "ds_dither",
	.set_params = distort_set_params,
	.create = distort_create_hook,
	.update = distort_update_hook,
	.tick = distort_tick_hook,
	.destroy = distort_destroy_hook,
};

static const char *distort_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("DistortFilter");
}

static void *distort_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&distort_def, settings, source);
}

static void distort_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "ds_mode", 1);
	obs_data_set_default_double(settings, "ds_center_x", 50.0);
	obs_data_set_default_double(settings, "ds_center_y", 50.0);
	obs_data_set_default_double(settings, "ds_amount", 12.0);
	obs_data_set_default_double(settings, "ds_wavelength", 120.0);
	obs_data_set_default_double(settings, "ds_angle", 0.0);
	obs_data_set_default_double(settings, "ds_radius", 50.0);
	obs_data_set_default_double(settings, "ds_speed", 0.5);
	obs_data_set_default_double(settings, "ds_decay", 50.0);
	obs_data_set_default_double(settings, "ds_scale", 120.0);
	obs_data_set_default_int(settings, "ds_map_mode", 2);
	obs_data_set_default_string(settings, "ds_map_source", "");
	obs_data_set_default_int(settings, "ds_edge", 1);
	obs_data_set_default_bool(settings, "ds_dither", false);
}

static void vis(obs_properties_t *props, const char *key, bool visible)
{
	obs_property_set_visible(obs_properties_get(props, key), visible);
}

static bool mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int m = (int)obs_data_get_int(settings, "ds_mode");
	const bool centered = m == 2 || m == 3 || m == 4;
	vis(props, "ds_center_x", centered);
	vis(props, "ds_center_y", centered);
	vis(props, "ds_radius", m == 2 || m == 3 || m == 4);
	vis(props, "ds_wavelength", m == 1 || m == 2);
	vis(props, "ds_angle", m == 1);
	vis(props, "ds_speed", m == 1 || m == 2 || m == 5);
	vis(props, "ds_decay", m == 2);
	vis(props, "ds_scale", m == 5);
	vis(props, "ds_map_source", m == MODE_MAP);
	vis(props, "ds_map_mode", m == MODE_MAP);

	obs_property_t *amount = obs_properties_get(props, "ds_amount");
	obs_property_set_description(amount, obs_module_text(m == 3   ? "Distort.TwirlAngle"
							     : m == 4 ? "Distort.Strength"
								      : "Distort.Amount"));
	obs_property_float_set_suffix(amount, m == 3 ? "°" : (m == 4 ? "%" : "px"));
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

static obs_properties_t *distort_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "ds_mode", obs_module_text("Distort.Mode"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Distort.Mode.Wave"), 1);
	obs_property_list_add_int(p, obs_module_text("Distort.Mode.Ripple"), 2);
	obs_property_list_add_int(p, obs_module_text("Distort.Mode.Twirl"), 3);
	obs_property_list_add_int(p, obs_module_text("Distort.Mode.Bulge"), 4);
	obs_property_list_add_int(p, obs_module_text("Distort.Mode.Noise"), 5);
	obs_property_list_add_int(p, obs_module_text("Distort.Mode.Map"), MODE_MAP);
	obs_property_set_modified_callback(p, mode_modified);

	p = obs_properties_add_list(props, "ds_map_source", obs_module_text("Distort.MapSource"),
				    OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(stroke_add_source_to_list, p);
	obs_enum_scenes(stroke_add_source_to_list, p);
	obs_property_list_insert_string(p, 0, "None", "");
	p = obs_properties_add_list(props, "ds_map_mode", obs_module_text("Distort.MapMode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Distort.MapMode.Offsets"), 1);
	obs_property_list_add_int(p, obs_module_text("Distort.MapMode.Refract"), 2);

	slider(props, "ds_amount", "Distort.Amount", -500.0, 500.0, 0.1, "px");
	slider(props, "ds_wavelength", "Distort.Wavelength", 2.0, 2000.0, 0.5, "px");
	slider(props, "ds_angle", "LongShadow.Angle", -180.0, 180.0, 0.1, "°");
	slider(props, "ds_center_x", "Gradient.CenterX", -50.0, 150.0, 0.1, "%");
	slider(props, "ds_center_y", "Gradient.CenterY", -50.0, 150.0, 0.1, "%");
	slider(props, "ds_radius", "Distort.Radius", 1.0, 200.0, 0.1, "%");
	slider(props, "ds_decay", "Distort.Decay", 0.0, 100.0, 0.1, "%");
	slider(props, "ds_scale", "Noise.Scale", 4.0, 2000.0, 1.0, "px");
	slider(props, "ds_speed", "Distort.Speed", -10.0, 10.0, 0.01, "/s");

	p = obs_properties_add_list(props, "ds_edge", obs_module_text("Distort.Edge"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Distort.Edge.Clamp"), 1);
	obs_property_list_add_int(p, obs_module_text("Distort.Edge.Transparent"), 2);
	obs_property_list_add_int(p, obs_module_text("Distort.Edge.Mirror"), 3);
	obs_properties_add_bool(props, "ds_dither", obs_module_text("Gradient.Dither"));
	return props;
}

struct obs_source_info spectrum_distort_filter = {
	.id = "spectrum_distort_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = distort_name,
	.create = distort_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = fx_cb_render,
	.get_properties = distort_properties,
	.get_defaults = distort_defaults,
};
