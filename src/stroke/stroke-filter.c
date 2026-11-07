#include "stroke-filter.h"
#include "stroke-types.h"
#include "stroke.h"
#include <math.h>
#include "../common/presets.h"

// Version 1 changed the defaults (see stroke_filter_defaults_v1). Filters and
// sources saved by earlier versions are loaded as version 0, which keeps the
// old defaults so existing scenes don't change; version 0 is hidden from the
// add filter/source menus.
#define STROKE_SOURCE_INFO(info_id, info_version, extra_flags, defaults)    \
	{                                                                   \
		.id = info_id,                                              \
		.version = info_version,                                    \
		.type = OBS_SOURCE_TYPE_INPUT,                              \
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | \
				OBS_SOURCE_SRGB | (extra_flags),            \
		.get_name = stroke_filter_name,                             \
		.create = stroke_filter_create,                             \
		.destroy = stroke_filter_destroy,                           \
		.update = stroke_filter_update,                             \
		.video_render = stroke_filter_video_render,                 \
		.video_tick = stroke_filter_video_tick,                     \
		.get_width = stroke_filter_width,                           \
		.get_height = stroke_filter_height,                         \
		.get_properties = stroke_source_properties,                 \
		.get_defaults = defaults,                                   \
		.icon_type = OBS_ICON_TYPE_COLOR,                           \
	}

#define STROKE_FILTER_INFO(info_id, info_version, extra_flags, defaults) \
	{                                                                \
		.id = info_id,                                           \
		.version = info_version,                                 \
		.type = OBS_SOURCE_TYPE_FILTER,                          \
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB |     \
				OBS_SOURCE_CUSTOM_DRAW | (extra_flags),  \
		.get_name = stroke_filter_name,                          \
		.create = stroke_filter_create,                          \
		.destroy = stroke_filter_destroy,                        \
		.update = stroke_filter_update,                          \
		.video_render = stroke_filter_video_render,              \
		.video_tick = stroke_filter_video_tick,                  \
		.video_get_color_space = stroke_filter_get_color_space,  \
		.get_width = stroke_filter_width,                        \
		.get_height = stroke_filter_height,                      \
		.get_properties = stroke_filter_properties,              \
		.get_defaults = defaults,                                \
	}

struct obs_source_info spectrum_stroke_source =
	STROKE_SOURCE_INFO("spectrum_stroke_source", 1, 0, stroke_filter_defaults_v1);
struct obs_source_info spectrum_stroke_filter =
	STROKE_FILTER_INFO("spectrum_stroke_filter", 1, 0, stroke_filter_defaults_v1);

static const char *stroke_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("StrokeFilter");
}

static void *stroke_filter_create(obs_data_t *settings, obs_source_t *source)
{
	stroke_filter_data_t *filter = bzalloc(sizeof(stroke_filter_data_t));

	source_ref_init(&filter->input_ref);
	source_ref_init(&filter->fill_ref);
	gradient_init(&filter->gradient, source, STROKE_GRADIENT_PREFIX);

	filter->context = source;
	filter->input_texture_generated = false;
	filter->is_source = obs_source_get_type(filter->context) == OBS_SOURCE_TYPE_INPUT;

	filter->is_filter = obs_source_get_type(filter->context) == OBS_SOURCE_TYPE_FILTER;

	filter->reload = true;

	filter->jump_flood_threshold = 0.5;

	obs_source_update(source, settings);
	return filter;
}

static void stroke_filter_destroy(void *data)
{
	stroke_filter_data_t *filter = data;

	obs_enter_graphics();

	if (filter->effect_jump_flood_sdf) {
		gs_effect_destroy(filter->effect_jump_flood_sdf);
	}

	if (filter->input_texrender) {
		gs_texrender_destroy(filter->input_texrender);
	}
	if (filter->output_texrender) {
		gs_texrender_destroy(filter->output_texrender);
	}
	if (filter->buffer_a) {
		gs_texrender_destroy(filter->buffer_a);
	}
	if (filter->buffer_b) {
		gs_texrender_destroy(filter->buffer_b);
	}
	if (filter->source_texrender) {
		gs_texrender_destroy(filter->source_texrender);
	}
	if (filter->fill_texrender) {
		gs_texrender_destroy(filter->fill_texrender);
	}

	obs_leave_graphics();

	source_ref_free(&filter->input_ref);
	source_ref_free(&filter->fill_ref);
	gradient_free(&filter->gradient);
	bfree(filter);
}

static uint32_t stroke_filter_width(void *data)
{
	stroke_filter_data_t *filter = data;
	return filter->output_width;
}

static uint32_t stroke_filter_height(void *data)
{
	stroke_filter_data_t *filter = data;
	return filter->output_height;
}

static void stroke_filter_update(void *data, obs_data_t *settings)
{
	stroke_filter_data_t *filter = data;
	filter->stroke_size = (float)obs_data_get_double(settings, "stroke_size");

	filter->stroke_offset = (float)obs_data_get_double(settings, "stroke_offset");

	vec4_from_rgba(&filter->stroke_color, (uint32_t)obs_data_get_int(settings, "stroke_fill_color"));

	filter->fill_type = (enum stroke_fill_type)obs_data_get_int(settings, "stroke_fill_type");
	filter->stroke_position = (enum stroke_position)obs_data_get_int(settings, "stroke_position");
	filter->contour_spacing = (float)obs_data_get_double(settings, "contour_spacing");
	filter->contour_offset = (float)obs_data_get_double(settings, "contour_offset");
	filter->contour_falloff_start = (float)obs_data_get_double(settings, "contour_falloff_start");
	filter->contour_falloff_end = (float)obs_data_get_double(settings, "contour_falloff_end");
	filter->contour_spacing_power = (float)obs_data_get_double(settings, "contour_spacing_power");

	filter->padding_amount = obs_data_get_int(settings, "stroke_padding") == PADDING_MANUAL
					 ? (uint32_t)obs_data_get_int(settings, "padding_amount")
					 : 0;

	if (obs_data_get_int(settings, "stroke_padding") == PADDING_AUTO &&
	    filter->stroke_position == STROKE_POSITION_OUTER) {
		// Round up and leave a pixel for the anti-aliased outer edge.
		filter->padding_amount = (uint32_t)ceilf(filter->stroke_size + filter->stroke_offset) + 1;
	}

	filter->ignore_source_border = obs_data_get_bool(settings, "ignore_source_border");

	filter->overflow = obs_data_get_int(settings, "stroke_padding") != PADDING_NONE &&
			   obs_data_get_bool(settings, "padding_overflow");

	bool is_inner = filter->stroke_position == STROKE_POSITION_INNER ||
			filter->stroke_position == STROKE_POSITION_INNER_CONTOUR;

	if (is_inner) {
		filter->jump_flood_threshold =
			(float)fmin(fmax(obs_data_get_double(settings, "jump_flood_threshold_inner"), 0.01), 99.99) /
			100.0f;
	} else {
		filter->jump_flood_threshold =
			(float)fmin(fmax(obs_data_get_double(settings, "jump_flood_threshold_outer"), 0.01), 99.99) /
			100.0f;
	}

	if (!filter->ignore_source_border && is_inner) {
		filter->pad_b = 1;
		filter->pad_t = 1;
		filter->pad_l = 1;
		filter->pad_r = 1;
	} else if (filter->stroke_position == STROKE_POSITION_OUTER ||
		   filter->stroke_position == STROKE_POSITION_OUTER_CONTOUR) {
		filter->pad_b = filter->padding_amount;
		filter->pad_t = filter->padding_amount;
		filter->pad_l = filter->padding_amount;
		filter->pad_r = filter->padding_amount;
	} else {
		filter->pad_b = 0;
		filter->pad_t = 0;
		filter->pad_l = 0;
		filter->pad_r = 0;
	}

	filter->fill = obs_data_get_bool(settings, "fill");
	// Resolved on the graphics thread, in video_tick.
	source_ref_set_name(&filter->fill_ref, obs_data_get_string(settings, "stroke_fill_source"));
	gradient_update(&filter->gradient, settings);
	if (filter->is_source) {
		source_ref_set_name(&filter->input_ref, obs_data_get_string(settings, "stroke_source"));
	}

	if (filter->reload) {
		filter->reload = false;
		load_effects(filter);
	}
}

static void stroke_filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	stroke_filter_data_t *filter = data;

	// Reuse this frame's result for further views (preview, projectors),
	// unless one wants a different color space.
	if (filter->rendered && (filter->is_source || filter->working_space == gs_get_color_space())) {
		render_cropped_output(filter);
		return;
	}

	bool skipRender = filter->rendering || filter->stroke_size <= 0.01;
	if (skipRender && filter->is_filter) {
		obs_source_skip_video_filter(filter->context);
		return;
	} else if (skipRender) {
		return;
	}

	filter->rendering = true;

	// 1. Get the input source as a texture renderer
	//    accessed as filter->input_texrender after call
	render_padded_input(filter);
	if (!filter->input_texture_generated) {
		filter->rendering = false;
		if (filter->is_filter) {
			obs_source_skip_video_filter(filter->context);
		}
		return;
	}

	// 2. Find the nearest edge of the source for every pixel. Contours can
	//    extend across the whole source; strokes only out to their extent.
	render_jf_seed(filter);
	const bool contour = filter->stroke_position == STROKE_POSITION_OUTER_CONTOUR ||
			     filter->stroke_position == STROKE_POSITION_INNER_CONTOUR;
	const float extent = contour ? fmaxf((float)filter->width, (float)filter->height)
				     : filter->stroke_offset + filter->stroke_size;
	render_jf_passes(filter, extent);

	render_jf_distance(filter);

	// 3. Draw result (filter->output_texrender) to source
	render_cropped_output(filter);
	filter->rendered = true;
	filter->rendering = false;
}

static obs_properties_t *properties(void *data, bool is_source)
{
	stroke_filter_data_t *filter = data;

	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, filter, NULL);

	if (is_source) {
		obs_property_t *stroke_source =
			obs_properties_add_list(props, "stroke_source", obs_module_text("StrokeSource.Source"),
						OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(stroke_source, obs_module_text("StrokeCommon.None"), "");
		obs_enum_sources(stroke_add_source_to_list, stroke_source);
		obs_enum_scenes(stroke_add_source_to_list, stroke_source);
	}

	obs_property_t *stroke_position_list = obs_properties_add_list(props, "stroke_position",
								       obs_module_text("StrokeFilter.StrokePosition"),
								       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(stroke_position_list, obs_module_text(STROKE_POSITION_OUTER_LABEL),
				  STROKE_POSITION_OUTER);
	obs_property_list_add_int(stroke_position_list, obs_module_text(STROKE_POSITION_INNER_LABEL),
				  STROKE_POSITION_INNER);
	obs_property_list_add_int(stroke_position_list, obs_module_text(STROKE_POSITION_OUTER_CONTOUR_LABEL),
				  STROKE_POSITION_OUTER_CONTOUR);
	obs_property_list_add_int(stroke_position_list, obs_module_text(STROKE_POSITION_INNER_CONTOUR_LABEL),
				  STROKE_POSITION_INNER_CONTOUR);

	obs_property_set_modified_callback2(stroke_position_list, setting_stroke_position_modified, (void *)is_source);

	obs_properties_add_bool(props, "ignore_source_border", obs_module_text("StrokeCommon.IgnoreSourceBorder"));

	obs_properties_add_bool(props, "fill", obs_module_text("StrokeFilter.FillSource"));

	obs_property_t *stroke_padding_list = obs_properties_add_list(props, "stroke_padding",
								      obs_module_text("StrokeFilter.Padding"),
								      OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(stroke_padding_list, obs_module_text(PADDING_NONE_LABEL), PADDING_NONE);

	obs_property_list_add_int(stroke_padding_list, obs_module_text(PADDING_AUTO_LABEL), PADDING_AUTO);

	obs_property_list_add_int(stroke_padding_list, obs_module_text(PADDING_MANUAL_LABEL), PADDING_MANUAL);

	obs_property_set_modified_callback(stroke_padding_list, setting_stroke_padding_modified);

	obs_property_t *p = obs_properties_add_int_slider(props, "padding_amount",
							  obs_module_text("StrokeFilter.Padding.Amount"), 0, 4000, 1);

	obs_property_float_set_suffix(p, "px");

	p = obs_properties_add_bool(props, "padding_overflow", obs_module_text(PADDING_OVERFLOW_LABEL));
	obs_property_set_long_description(p, obs_module_text(PADDING_OVERFLOW_DESCRIPTION));

	p = obs_properties_add_float_slider(props, "jump_flood_threshold_inner",
					    obs_module_text("StrokeFilter.MaskThreshold"), 0.0, 100.0, 0.01);
	obs_property_float_set_suffix(p, "%");

	p = obs_properties_add_float_slider(props, "jump_flood_threshold_outer",
					    obs_module_text("StrokeFilter.MaskThreshold"), 0.0, 100.0, 0.01);
	obs_property_float_set_suffix(p, "%");

	p = obs_properties_add_float_slider(props, "stroke_size", obs_module_text("StrokeFilter.StrokeSize"), 0.0,
					    2000.0, 1.0);
	obs_property_float_set_suffix(p, "px");

	p = obs_properties_add_float_slider(props, "stroke_offset", obs_module_text("StrokeFilter.StrokeOffset"), 0.0,
					    2000.0, 1.0);
	obs_property_float_set_suffix(p, "px");

	p = obs_properties_add_float_slider(props, "contour_spacing", obs_module_text("StrokeFilter.ContourSpacing"),
					    0.0, 2000.0, 1.0);
	obs_property_float_set_suffix(p, "px");

	obs_properties_add_float_slider(props, "contour_spacing_power",
					obs_module_text("StrokeFilter.ContourSpacingPower"), 0.01, 2.0, 0.01);

	obs_property_t *c_offset = obs_properties_add_float_slider(
		props, "contour_offset", obs_module_text("StrokeFilter.ContourOffset"), -500.0, 500.0, 0.01);

	obs_property_float_set_suffix(c_offset, "%");

	p = obs_properties_add_float_slider(props, "contour_falloff_start",
					    obs_module_text("StrokeFilter.ContourFalloffStart"), 0.0, 2000.0, 1.0);
	obs_property_float_set_suffix(p, "px");

	p = obs_properties_add_float_slider(props, "contour_falloff_end",
					    obs_module_text("StrokeFilter.ContourFalloffEnd"), 0.0, 2000.0, 1.0);
	obs_property_float_set_suffix(p, "px");

	obs_property_t *stroke_fill_method_list = obs_properties_add_list(props, "stroke_fill_type",
									  obs_module_text("StrokeFilter.FillType"),
									  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(stroke_fill_method_list, obs_module_text(STROKE_FILL_TYPE_COLOR_LABEL),
				  STROKE_FILL_TYPE_COLOR);
	obs_property_list_add_int(stroke_fill_method_list, obs_module_text(STROKE_FILL_TYPE_SOURCE_LABEL),
				  STROKE_FILL_TYPE_SOURCE);
	obs_property_list_add_int(stroke_fill_method_list, obs_module_text(STROKE_FILL_TYPE_GRADIENT_LABEL),
				  STROKE_FILL_TYPE_GRADIENT);
	obs_property_list_add_int(stroke_fill_method_list, obs_module_text(STROKE_FILL_TYPE_GRADIENT_ACROSS_LABEL),
				  STROKE_FILL_TYPE_GRADIENT_ACROSS);
	//obs_property_list_add_int(
	//	stroke_fill_method_list,
	//	obs_module_text(STROKE_FILL_TYPE_IMAGE_LABEL),
	//	STROKE_FILL_TYPE_IMAGE);

	obs_property_set_modified_callback(stroke_fill_method_list, setting_fill_type_modified);

	obs_properties_add_color_alpha(props, "stroke_fill_color", obs_module_text("StrokeFilter.ColorFill"));

	obs_properties_add_path(props, "stroke_fill_image", obs_module_text("StrokeFilter.ImageFill"), OBS_PATH_FILE,
				"Textures (*.bmp *.tga *.png *.jpeg *.jpg *.gif);;", NULL);

	obs_property_t *stroke_fill_source_source =
		obs_properties_add_list(props, "stroke_fill_source", obs_module_text("StrokeFilter.SourceFill"),
					OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(stroke_add_source_to_list, stroke_fill_source_source);
	obs_enum_scenes(stroke_add_source_to_list, stroke_fill_source_source);
	obs_property_list_insert_string(stroke_fill_source_source, 0, "None", "");

	gradient_properties(props, STROKE_GRADIENT_PREFIX, filter ? &filter->gradient : NULL);

	spectrum_presets_properties(props, data ? ((stroke_filter_data_t *)data)->context : NULL);
	return props;
}

static obs_properties_t *stroke_filter_properties(void *data)
{
	return properties(data, false);
}

static obs_properties_t *stroke_source_properties(void *data)
{
	return properties(data, true);
}

// The filter works in the space it is drawn into where it can (including
// HDR), so report that. Sources keep the default (SDR sRGB): they
// draw a stroke color or SDR-rendered fill.
static enum gs_color_space stroke_filter_get_color_space(void *data, size_t count,
							 const enum gs_color_space *preferred_spaces)
{
	stroke_filter_data_t *filter = data;
	return filter_working_space(filter->context, count, preferred_spaces);
}

static void stroke_filter_video_tick(void *data, float seconds)
{
	stroke_filter_data_t *filter = data;

	source_ref_tick(&filter->fill_ref, seconds);
	gradient_tick(&filter->gradient, seconds);
	if (filter->is_source) {
		source_ref_tick(&filter->input_ref, seconds);
	}

	// Keep the sources we draw showing/active while we are, as a scene
	// would. A filter follows its parent, while it is enabled.
	obs_source_t *owner = filter->is_filter ? obs_filter_get_parent(filter->context) : filter->context;
	const bool enabled = !filter->is_filter || obs_source_enabled(filter->context);
	source_ref_update_activation(&filter->fill_ref, owner, enabled && filter->fill_type == STROKE_FILL_TYPE_SOURCE,
				     seconds);
	if (filter->is_source) {
		source_ref_update_activation(&filter->input_ref, owner, true, seconds);
	}

	uint32_t base_width = 0;
	uint32_t base_height = 0;
	if (filter->is_filter) {
		obs_source_t *target = obs_filter_get_target(filter->context);
		if (!target) {
			return;
		}
		base_width = (uint32_t)obs_source_get_base_width(target);
		base_height = (uint32_t)obs_source_get_base_height(target);
		filter->source_width = base_width;
		filter->source_height = base_height;
	} else {
		obs_source_t *input = source_ref_get(&filter->input_ref);
		if (input) {
			base_width = obs_source_get_width(input);
			base_height = obs_source_get_height(input);
			obs_source_release(input);
		}
	}

	// Inner strokes only pad to treat the source border as an edge, and
	// crop that padding back off when drawing.
	const bool inner = filter->stroke_position == STROKE_POSITION_INNER ||
			   filter->stroke_position == STROKE_POSITION_INNER_CONTOUR;
	if (inner || filter->overflow || !base_width || !base_height) {
		filter->output_width = base_width;
		filter->output_height = base_height;
	} else {
		filter->output_width = base_width + filter->pad_l + filter->pad_r;
		filter->output_height = base_height + filter->pad_t + filter->pad_b;
	}
	filter->rendered = false;
}

static void stroke_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "stroke_size", 4.0);
	obs_data_set_default_double(settings, "stroke_offset", 0.0);
	obs_data_set_default_int(settings, "stroke_fill_type", STROKE_FILL_TYPE_COLOR);
	obs_data_set_default_string(settings, "stroke_fill_source", "");
	obs_data_set_default_int(settings, "stroke_fill_color", DEFAULT_COLOR);
	obs_data_set_default_int(settings, "stroke_position", STROKE_POSITION_OUTER);
	obs_data_set_default_bool(settings, "ignore_source_border", true);
	obs_data_set_default_bool(settings, "fill", true);
	obs_data_set_default_double(settings, "contour_spacing", 10.0);
	obs_data_set_default_double(settings, "contour_offset", 0.0);
	obs_data_set_default_double(settings, "contour_spacing_power", 1.0);
	obs_data_set_default_double(settings, "jump_flood_threshold_outer", 100.0);
	obs_data_set_default_double(settings, "jump_flood_threshold_inner", 0.0);
	obs_data_set_default_double(settings, "contour_falloff_start", 0.0);
	obs_data_set_default_double(settings, "contour_falloff_end", 2000.0);
	gradient_defaults(settings, STROKE_GRADIENT_PREFIX);
}

// Defaults for filters/sources created by this version: pad automatically
// and draw the padding outside the source's bounds, so a new outer stroke
// isn't clipped at the source's edge and the source doesn't move or resize.
static void stroke_filter_defaults_v1(obs_data_t *settings)
{
	stroke_filter_defaults(settings);
	obs_data_set_default_int(settings, "stroke_padding", PADDING_AUTO);
	obs_data_set_default_bool(settings, "padding_overflow", true);
}

static void load_effects(stroke_filter_data_t *filter)
{
	load_jump_flood_sdf_effect(filter);
}

static bool setting_fill_type_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	enum stroke_fill_type fill_type = (enum stroke_fill_type)obs_data_get_int(settings, "stroke_fill_type");
	gradient_set_visible(props, STROKE_GRADIENT_PREFIX,
			     fill_type == STROKE_FILL_TYPE_GRADIENT || fill_type == STROKE_FILL_TYPE_GRADIENT_ACROSS);
	if (fill_type == STROKE_FILL_TYPE_GRADIENT) {
		gradient_refresh_visibility(props, STROKE_GRADIENT_PREFIX, settings);
	} else if (fill_type == STROKE_FILL_TYPE_GRADIENT_ACROSS) {
		gradient_hide_geometry(props, STROKE_GRADIENT_PREFIX);
	}
	switch (fill_type) {
	case STROKE_FILL_TYPE_COLOR:
		stroke_setting_visibility("stroke_fill_color", true, props);
		stroke_setting_visibility("stroke_fill_source", false, props);
		stroke_setting_visibility("stroke_fill_image", false, props);
		break;
	case STROKE_FILL_TYPE_SOURCE:
		stroke_setting_visibility("stroke_fill_color", false, props);
		stroke_setting_visibility("stroke_fill_source", true, props);
		stroke_setting_visibility("stroke_fill_image", false, props);
		break;
	case STROKE_FILL_TYPE_IMAGE:
		stroke_setting_visibility("stroke_fill_color", false, props);
		stroke_setting_visibility("stroke_fill_source", false, props);
		stroke_setting_visibility("stroke_fill_image", true, props);
		break;
	case STROKE_FILL_TYPE_GRADIENT:
	case STROKE_FILL_TYPE_GRADIENT_ACROSS:
		stroke_setting_visibility("stroke_fill_color", false, props);
		stroke_setting_visibility("stroke_fill_source", false, props);
		stroke_setting_visibility("stroke_fill_image", false, props);
		break;
	}
	return true;
}

static bool setting_stroke_padding_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	enum padding_type paddingType = (enum padding_type)obs_data_get_int(settings, "stroke_padding");

	stroke_setting_visibility("padding_amount", paddingType == PADDING_MANUAL, props);
	stroke_setting_visibility("padding_overflow", paddingType != PADDING_NONE, props);
	return true;
}

static bool setting_stroke_position_modified(void *data, obs_properties_t *props, obs_property_t *p,
					     obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	bool is_source = data;

	enum stroke_position position = (enum stroke_position)obs_data_get_int(settings, "stroke_position");
	enum padding_type paddingType = (enum padding_type)obs_data_get_int(settings, "stroke_padding");
	switch (position) {
	case STROKE_POSITION_INNER:
		stroke_setting_visibility("ignore_source_border", true, props);
		stroke_setting_visibility("jump_flood_threshold_inner", true, props);
		stroke_setting_visibility("jump_flood_threshold_outer", false, props);
		stroke_setting_visibility("contour_spacing", false, props);
		stroke_setting_visibility("contour_offset", false, props);
		stroke_setting_visibility("contour_spacing_power", false, props);
		stroke_setting_visibility("contour_falloff_start", false, props);
		stroke_setting_visibility("contour_falloff_end", false, props);
		stroke_setting_visibility("stroke_offset", true, props);
		stroke_setting_visibility("fill", false, props);
		stroke_setting_visibility("stroke_padding", false, props);
		stroke_setting_visibility("padding_amount", false, props);
		stroke_setting_visibility("padding_overflow", false, props);
		break;
	case STROKE_POSITION_OUTER:
		stroke_setting_visibility("ignore_source_border", false, props);
		stroke_setting_visibility("jump_flood_threshold_inner", false, props);
		stroke_setting_visibility("jump_flood_threshold_outer", true, props);
		stroke_setting_visibility("contour_spacing", false, props);
		stroke_setting_visibility("contour_offset", false, props);
		stroke_setting_visibility("contour_spacing_power", false, props);
		stroke_setting_visibility("contour_falloff_start", false, props);
		stroke_setting_visibility("contour_falloff_end", false, props);
		stroke_setting_visibility("stroke_offset", true, props);
		stroke_setting_visibility("fill", is_source, props);
		stroke_setting_visibility("stroke_padding", true, props);
		stroke_setting_visibility("padding_amount", paddingType == PADDING_MANUAL, props);
		stroke_setting_visibility("padding_overflow", paddingType != PADDING_NONE, props);
		break;
	case STROKE_POSITION_OUTER_CONTOUR:
		stroke_setting_visibility("ignore_source_border", false, props);
		stroke_setting_visibility("jump_flood_threshold_inner", false, props);
		stroke_setting_visibility("jump_flood_threshold_outer", true, props);
		stroke_setting_visibility("contour_spacing", true, props);
		stroke_setting_visibility("contour_offset", true, props);
		stroke_setting_visibility("contour_spacing_power", true, props);
		stroke_setting_visibility("contour_falloff_start", true, props);
		stroke_setting_visibility("contour_falloff_end", true, props);
		stroke_setting_visibility("stroke_offset", false, props);
		stroke_setting_visibility("fill", false, props);
		stroke_setting_visibility("stroke_padding", true, props);
		stroke_setting_visibility("padding_amount", paddingType == PADDING_MANUAL, props);
		stroke_setting_visibility("padding_overflow", paddingType != PADDING_NONE, props);
		break;
	case STROKE_POSITION_INNER_CONTOUR:
		stroke_setting_visibility("ignore_source_border", true, props);
		stroke_setting_visibility("jump_flood_threshold_inner", true, props);
		stroke_setting_visibility("jump_flood_threshold_outer", false, props);
		stroke_setting_visibility("contour_spacing", true, props);
		stroke_setting_visibility("contour_offset", true, props);
		stroke_setting_visibility("contour_spacing_power", true, props);
		stroke_setting_visibility("contour_falloff_start", true, props);
		stroke_setting_visibility("contour_falloff_end", true, props);
		stroke_setting_visibility("stroke_offset", false, props);
		stroke_setting_visibility("fill", false, props);
		stroke_setting_visibility("stroke_padding", false, props);
		stroke_setting_visibility("padding_amount", false, props);
		stroke_setting_visibility("padding_overflow", false, props);
		break;
	default:
		break;
	}
	return true;
}
