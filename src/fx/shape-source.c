// Spectrum Shape: a source drawing a shape (rectangle, ellipse, polygon,
// star, heart, ring, cross) with a color, gradient or source fill, a stroke
// and a drop shadow.

#include "../common/fx-filter.h"
#include "../stroke/obs-utils.h"
#include "../common/presets.h"

#define SH_PREFIX "sh_gradient_"

enum { FILL_NONE = 0, FILL_COLOR = 1, FILL_GRADIENT = 2, FILL_SOURCE = 3 };

struct shape_data {
	struct source_ref fill_ref;
	gs_texrender_t *fill_texrender;
	int fill_type;
};

static void shape_create_hook(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct shape_data *d = bzalloc(sizeof(struct shape_data));
	source_ref_init(&d->fill_ref);
	fx->user = d;
}

static void shape_update_hook(fx_filter_t *fx, obs_data_t *settings)
{
	struct shape_data *d = fx->user;
	d->fill_type = (int)obs_data_get_int(settings, "sh_fill_type");
	source_ref_set_name(&d->fill_ref, obs_data_get_string(settings, "sh_fill_source"));
}

static void shape_tick_hook(fx_filter_t *fx, float seconds)
{
	struct shape_data *d = fx->user;
	source_ref_tick(&d->fill_ref, seconds);
	source_ref_update_activation(&d->fill_ref, fx->context, d->fill_type == FILL_SOURCE, seconds);
}

static void shape_destroy_hook(fx_filter_t *fx)
{
	struct shape_data *d = fx->user;
	if (!d)
		return;
	obs_enter_graphics();
	if (d->fill_texrender)
		gs_texrender_destroy(d->fill_texrender);
	obs_leave_graphics();
	source_ref_free(&d->fill_ref);
	bfree(d);
	fx->user = NULL;
}

static void shape_set_params(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, obs_data_t *to, float t,
			     uint32_t width, uint32_t height)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(to);
	UNUSED_PARAMETER(t);
	struct shape_data *d = fx->user;

	gs_texture_t *fill = NULL;
	int fill_type = d->fill_type;
	if (fill_type == FILL_GRADIENT) {
		fill = gradient_render_fill(&fx->gradient, width, height);
	} else if (fill_type == FILL_SOURCE) {
		obs_source_t *source = source_ref_get(&d->fill_ref);
		if (source && !is_self_reference(fx->context, source))
			fill = render_source_to_texture(source, &d->fill_texrender);
		obs_source_release(source);
	}
	if ((fill_type == FILL_GRADIENT || fill_type == FILL_SOURCE) && !fill)
		fill_type = FILL_NONE;
	fx_set_int(effect, "sh_fill_type", fill_type);
	fx_set_texture(effect, "fill_tex", fill);
}

static const struct fx_filter_def shape_def = {
	.effect_file = "shaders/fx/shape.effect",
	.gradient_prefix = SH_PREFIX,
	.set_params = shape_set_params,
	.create = shape_create_hook,
	.update = shape_update_hook,
	.tick = shape_tick_hook,
	.destroy = shape_destroy_hook,
};

static const char *shape_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ShapeSource");
}

static void *shape_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&shape_def, settings, source);
}

static void shape_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "width", 400);
	obs_data_set_default_int(settings, "height", 300);
	obs_data_set_default_int(settings, "sh_shape", 1);
	obs_data_set_default_double(settings, "sh_radius", 24.0);
	obs_data_set_default_double(settings, "sh_rotation", 0.0);
	obs_data_set_default_int(settings, "sh_sides", 6);
	obs_data_set_default_int(settings, "sh_points", 5);
	obs_data_set_default_double(settings, "sh_inner", 40.0);
	obs_data_set_default_double(settings, "sh_thickness", 30.0);
	obs_data_set_default_bool(settings, "sh_stretch", false);
	obs_data_set_default_double(settings, "sh_padding", 0.0);
	obs_data_set_default_int(settings, "sh_fill_type", FILL_COLOR);
	obs_data_set_default_int(settings, "sh_fill_color", 0xFFE0A019);
	obs_data_set_default_double(settings, "sh_stroke_width", 0.0);
	obs_data_set_default_int(settings, "sh_stroke_position", 1);
	obs_data_set_default_int(settings, "sh_stroke_color", 0xFFFFFFFF);
	obs_data_set_default_bool(settings, "sh_shadow", false);
	obs_data_set_default_int(settings, "sh_shadow_color", 0x80000000);
	obs_data_set_default_double(settings, "sh_shadow_blur", 12.0);
	obs_data_set_default_double(settings, "sh_shadow_x", 0.0);
	obs_data_set_default_double(settings, "sh_shadow_y", 6.0);
	obs_data_set_default_double(settings, "sh_opacity", 100.0);
	gradient_defaults(settings, SH_PREFIX);
}

static void set_vis(obs_properties_t *props, const char *name, bool visible)
{
	obs_property_t *p = obs_properties_get(props, name);
	if (p)
		obs_property_set_visible(p, visible);
}

static bool shape_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int shape = (int)obs_data_get_int(settings, "sh_shape");
	set_vis(props, "sh_sides", shape == 3);
	set_vis(props, "sh_points", shape == 4);
	set_vis(props, "sh_inner", shape == 4);
	set_vis(props, "sh_thickness", shape == 6 || shape == 7);
	set_vis(props, "sh_stretch", shape >= 3 && shape != 6);
	set_vis(props, "sh_radius", shape != 2 && shape != 6);

	const int fill = (int)obs_data_get_int(settings, "sh_fill_type");
	set_vis(props, "sh_fill_color", fill == FILL_COLOR);
	set_vis(props, "sh_fill_source", fill == FILL_SOURCE);
	gradient_set_visible(props, SH_PREFIX, fill == FILL_GRADIENT);
	if (fill == FILL_GRADIENT)
		gradient_refresh_visibility(props, SH_PREFIX, settings);

	const bool shadow = obs_data_get_bool(settings, "sh_shadow");
	set_vis(props, "sh_shadow_color", shadow);
	set_vis(props, "sh_shadow_blur", shadow);
	set_vis(props, "sh_shadow_x", shadow);
	set_vis(props, "sh_shadow_y", shadow);
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

static obs_properties_t *shape_properties(void *data)
{
	fx_filter_t *fx = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	fx_add_size_properties(props);

	p = obs_properties_add_list(props, "sh_shape", obs_module_text("Shape.Shape"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Shape.Rectangle"), 1);
	obs_property_list_add_int(p, obs_module_text("Shape.Ellipse"), 2);
	obs_property_list_add_int(p, obs_module_text("Shape.Polygon"), 3);
	obs_property_list_add_int(p, obs_module_text("Shape.Star"), 4);
	obs_property_list_add_int(p, obs_module_text("Shape.Heart"), 5);
	obs_property_list_add_int(p, obs_module_text("Shape.Ring"), 6);
	obs_property_list_add_int(p, obs_module_text("Shape.Cross"), 7);
	obs_property_set_modified_callback(p, shape_modified);

	slider(props, "sh_radius", "Shape.Radius", 0.0, 2000.0, 0.5, "px");
	slider(props, "sh_rotation", "Shape.Rotation", -360.0, 360.0, 0.1, "°");
	obs_properties_add_int_slider(props, "sh_sides", obs_module_text("Shape.Sides"), 3, 32, 1);
	obs_properties_add_int_slider(props, "sh_points", obs_module_text("Shape.Points"), 3, 32, 1);
	slider(props, "sh_inner", "Shape.Inner", 0.0, 100.0, 0.1, "%");
	slider(props, "sh_thickness", "Shape.Thickness", 1.0, 100.0, 0.1, "%");
	obs_properties_add_bool(props, "sh_stretch", obs_module_text("Shape.Stretch"));
	slider(props, "sh_padding", "Shape.Padding", 0.0, 1000.0, 0.5, "px");

	p = obs_properties_add_list(props, "sh_fill_type", obs_module_text("StrokeFilter.FillType"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Shape.Fill.None"), FILL_NONE);
	obs_property_list_add_int(p, obs_module_text("StrokeFilter.ColorFill"), FILL_COLOR);
	obs_property_list_add_int(p, obs_module_text("StrokeFilter.GradientFill"), FILL_GRADIENT);
	obs_property_list_add_int(p, obs_module_text("StrokeFilter.SourceFill"), FILL_SOURCE);
	obs_property_set_modified_callback(p, shape_modified);
	obs_properties_add_color_alpha(props, "sh_fill_color", obs_module_text("StrokeFilter.ColorFill"));
	p = obs_properties_add_list(props, "sh_fill_source", obs_module_text("StrokeFilter.SourceFill"),
				    OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(stroke_add_source_to_list, p);
	obs_enum_scenes(stroke_add_source_to_list, p);
	obs_property_list_insert_string(p, 0, "None", "");

	gradient_properties(props, SH_PREFIX, fx ? &fx->gradient : NULL);

	slider(props, "sh_stroke_width", "Shape.StrokeWidth", 0.0, 200.0, 0.5, "px");
	p = obs_properties_add_list(props, "sh_stroke_position", obs_module_text("StrokeFilter.StrokePosition"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Shape.Stroke.Inside"), 1);
	obs_property_list_add_int(p, obs_module_text("Shape.Stroke.Center"), 2);
	obs_property_list_add_int(p, obs_module_text("Shape.Stroke.Outside"), 3);
	obs_properties_add_color_alpha(props, "sh_stroke_color", obs_module_text("Shape.StrokeColor"));

	p = obs_properties_add_bool(props, "sh_shadow", obs_module_text("Shape.Shadow"));
	obs_property_set_modified_callback(p, shape_modified);
	obs_properties_add_color_alpha(props, "sh_shadow_color", obs_module_text("Shape.ShadowColor"));
	slider(props, "sh_shadow_blur", "Shape.ShadowBlur", 0.0, 200.0, 0.5, "px");
	slider(props, "sh_shadow_x", "Shape.ShadowX", -500.0, 500.0, 0.5, "px");
	slider(props, "sh_shadow_y", "Shape.ShadowY", -500.0, 500.0, 0.5, "px");

	slider(props, "sh_opacity", "Gradient.Opacity", 0.0, 100.0, 0.1, "%");
	spectrum_presets_properties(props, data ? ((fx_filter_t *)data)->context : NULL);
	return props;
}

struct obs_source_info spectrum_shape_source = {
	.id = "spectrum_shape_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.get_name = shape_name,
	.create = shape_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = fx_cb_render,
	.get_width = fx_cb_width,
	.get_height = fx_cb_height,
	.get_properties = shape_properties,
	.get_defaults = shape_defaults,
	.icon_type = OBS_ICON_TYPE_COLOR,
};
