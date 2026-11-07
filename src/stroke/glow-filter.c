#include "glow-filter.h"
#include "glow-types.h"
#include "blur/alpha-blur.h"
#include "blur/dual-kawase.h"
#include "glow.h"

#include <math.h>

// Version 1 changed the defaults (see glow_filter_defaults_v1). Filters and
// sources saved by earlier versions are loaded as version 0, which keeps the
// old defaults so existing scenes don't change; version 0 is hidden from the
// add filter/source menus.
#define GLOW_FILTER_INFO(info_id, info_version, extra_flags, name, create_fn, props, defaults)                                     \
	{                                                                     \
		.id = info_id,                                                \
		.version = info_version,                                      \
		.type = OBS_SOURCE_TYPE_FILTER,                               \
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB |          \
				(extra_flags),                                \
		.get_name = name,                                             \
		.create = create_fn,                                          \
		.destroy = glow_filter_destroy,                               \
		.update = glow_filter_update,                                 \
		.video_render = glow_filter_video_render,                     \
		.video_tick = glow_filter_video_tick,                         \
		.video_get_color_space = glow_filter_get_color_space,         \
		.get_width = glow_filter_width,                               \
		.get_height = glow_filter_height,                             \
		.get_properties = props,                                      \
		.get_defaults = defaults,                                     \
	}

#define GLOW_SOURCE_INFO(info_id, info_version, extra_flags, name, create_fn, props, defaults)                                     \
	{                                                                     \
		.id = info_id,                                                \
		.version = info_version,                                      \
		.type = OBS_SOURCE_TYPE_INPUT,                                \
		.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW |   \
				OBS_SOURCE_SRGB | (extra_flags),              \
		.get_name = name,                                             \
		.create = create_fn,                                          \
		.destroy = glow_filter_destroy,                               \
		.update = glow_filter_update,                                 \
		.video_render = glow_filter_video_render,                     \
		.video_tick = glow_filter_video_tick,                         \
		.get_width = glow_filter_width,                               \
		.get_height = glow_filter_height,                             \
		.get_properties = props,                                      \
		.get_defaults = defaults,                                     \
		.icon_type = OBS_ICON_TYPE_COLOR,                             \
	}

struct obs_source_info spectrum_glow_filter = GLOW_FILTER_INFO("spectrum_glow_filter", 1, 0, glow_filter_name,
							       glow_filter_create, glow_filter_properties,
							       glow_filter_defaults_v1);
struct obs_source_info spectrum_glow_source = GLOW_SOURCE_INFO("spectrum_glow_source", 1, 0, glow_filter_name,
							       glow_filter_create, glow_source_properties,
							       glow_filter_defaults_v1);
struct obs_source_info spectrum_shadow_filter = GLOW_FILTER_INFO("spectrum_shadow_filter", 1, 0, shadow_filter_name,
								 shadow_filter_create, shadow_filter_properties,
								 shadow_filter_defaults_v1);
struct obs_source_info spectrum_shadow_source = GLOW_SOURCE_INFO("spectrum_shadow_source", 1, 0, shadow_filter_name,
								 shadow_filter_create, shadow_source_properties,
								 shadow_filter_defaults_v1);

static const char *glow_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("GlowFilter");
}

static const char *shadow_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ShadowFilter");
}

static void *glow_filter_create(obs_data_t *settings, obs_source_t *source)
{
	glow_filter_data_t *filter = filter_create(source);
	filter->filter_type = FILTER_TYPE_GLOW;
	obs_source_update(source, settings);
	return filter;
}

static void *shadow_filter_create(obs_data_t *settings, obs_source_t *source)
{
	glow_filter_data_t *filter = filter_create(source);
	filter->filter_type = FILTER_TYPE_SHADOW;
	obs_source_update(source, settings);
	return filter;
}

static glow_filter_data_t *filter_create(obs_source_t *source)
{
	glow_filter_data_t *filter = bzalloc(sizeof(glow_filter_data_t));

	source_ref_init(&filter->input_ref);
	source_ref_init(&filter->fill_ref);
	gradient_init(&filter->gradient, source, GLOW_GRADIENT_PREFIX);
	filter->context = source;
	filter->input_texture_generated = false;
	filter->alpha_blur_data = bzalloc(sizeof(alpha_blur_data_t));

	filter->is_source = obs_source_get_type(filter->context) == OBS_SOURCE_TYPE_INPUT;

	filter->is_filter = obs_source_get_type(filter->context) == OBS_SOURCE_TYPE_FILTER;

	filter->param_glow_image = NULL;
	filter->param_glow_mask = NULL;
	filter->param_glow_fill_source = NULL;
	filter->param_glow_fill_color = NULL;
	filter->param_glow_intensity = NULL;
	filter->param_offset_texel = NULL;
	filter->param_glow_fill_behind = NULL;
	filter->param_threshold = NULL;

	filter->reload = true;

	alpha_blur_init(filter->alpha_blur_data);
	return filter;
}

static void glow_filter_destroy(void *data)
{
	glow_filter_data_t *filter = data;

	obs_enter_graphics();

	if (filter->effect_glow) {
		gs_effect_destroy(filter->effect_glow);
	}
	if (filter->input_texrender) {
		gs_texrender_destroy(filter->input_texrender);
	}
	if (filter->output_texrender) {
		gs_texrender_destroy(filter->output_texrender);
	}
	if (filter->alpha_mask_texrender) {
		gs_texrender_destroy(filter->alpha_mask_texrender);
	}

	if (filter->source_texrender) {
		gs_texrender_destroy(filter->source_texrender);
	}
	if (filter->fill_texrender) {
		gs_texrender_destroy(filter->fill_texrender);
	}

	alpha_blur_destroy(filter->alpha_blur_data);

	obs_leave_graphics();

	source_ref_free(&filter->input_ref);
	source_ref_free(&filter->fill_ref);
	gradient_free(&filter->gradient);
	bfree(filter->alpha_blur_data);
	bfree(filter);
}

static uint32_t glow_filter_width(void *data)
{
	glow_filter_data_t *filter = data;
	return filter->output_width;
}

static uint32_t glow_filter_height(void *data)
{
	glow_filter_data_t *filter = data;
	return filter->output_height;
}

static void glow_filter_update(void *data, obs_data_t *settings)
{
	glow_filter_data_t *filter = data;
	filter->glow_size = (float)obs_data_get_double(settings, "glow_size");
	filter->intensity = (float)obs_data_get_double(settings, "glow_intensity") / 100.0f;
	filter->ignore_source_border = obs_data_get_bool(settings, "ignore_source_border");
	filter->fill = obs_data_get_bool(settings, "fill");

	filter->blur_type = (enum blur_type)obs_data_get_int(settings, "blur_type");

	vec4_from_rgba(&filter->glow_color, (uint32_t)obs_data_get_int(settings, "glow_fill_color"));

	vec4_from_rgba_srgb(&filter->glow_color_srgb, (uint32_t)obs_data_get_int(settings, "glow_fill_color"));

	filter->fill_type = (enum glow_fill_type)obs_data_get_int(settings, "glow_fill_type");

	filter->glow_position = (enum glow_position)obs_data_get_int(settings, "glow_position");

	// Resolved on the graphics thread, in video_tick.
	if (filter->is_source) {
		source_ref_set_name(&filter->input_ref, obs_data_get_string(settings, "glow_source"));
	}

	filter->padding_amount = obs_data_get_int(settings, "glow_padding") == PADDING_MANUAL
					 ? (uint32_t)obs_data_get_int(settings, "padding_amount")
					 : 0;

	if (obs_data_get_int(settings, "glow_padding") == PADDING_AUTO &&
	    filter->glow_position == GLOW_POSITION_OUTER) {
		double offset = filter->filter_type == FILTER_TYPE_SHADOW
					? obs_data_get_double(settings, "glow_offset_distance")
					: 0.0;
		filter->padding_amount = (uint32_t)(filter->glow_size * 3.5 + offset);
	}

	filter->overflow = obs_data_get_int(settings, "glow_padding") != PADDING_NONE &&
			   obs_data_get_bool(settings, "padding_overflow");

	filter->threshold = filter->glow_position == GLOW_POSITION_OUTER
				    ? (float)obs_data_get_double(settings, "threshold_outer") / 100.0f
				    : (float)obs_data_get_double(settings, "threshold_inner") / 100.0f;
	// The mask shader divides by the threshold.
	filter->threshold = fmaxf(filter->threshold, 0.0001f);

	if (filter->glow_position == GLOW_POSITION_OUTER) {
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

	if (filter->filter_type == FILTER_TYPE_SHADOW) {
		double offset_distance = obs_data_get_double(settings, "glow_offset_distance");

		double offset_angle = RAD(obs_data_get_double(settings, "glow_offset_angle"));

		double x_offset = offset_distance * cos(offset_angle);
		double y_offset = offset_distance * sin(offset_angle);

		filter->offset_texel.x = (float)x_offset;
		filter->offset_texel.y = (float)y_offset;
	} else {
		filter->offset_texel.x = 0.0f;
		filter->offset_texel.y = 0.0f;
	}

	source_ref_set_name(&filter->fill_ref, obs_data_get_string(settings, "glow_fill_source"));
	gradient_update(&filter->gradient, settings);

	if (filter->reload) {
		filter->reload = false;
		load_effects(filter);
	}
}

static void glow_filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	glow_filter_data_t *filter = data;

	// Reuse this frame's result for further views (preview, projectors),
	// unless one wants a different color space.
	if (filter->rendered && (filter->is_source || filter->working_space == gs_get_color_space())) {
		glow_render_cropped_output(filter);
		return;
	}

	bool skipRender = filter->rendering ||
			  (filter->filter_type == FILTER_TYPE_GLOW && filter->glow_size <= 0.01 && !filter->fill);
	if (skipRender && filter->is_filter) {
		obs_source_skip_video_filter(filter->context);
		return;
	} else if (skipRender) {
		return;
	}

	filter->rendering = true;

	// 1. Get the input source as a texture renderer
	//    accessed as filter->input_texrender after call
	glow_render_padded_input(filter);

	if (!filter->input_texture_generated) {
		if (filter->is_filter) {
			obs_source_skip_video_filter(filter->context);
		}
		filter->rendering = false;
		return;
	}

	// 2. Generate the alpha mask to be blurred based on user
	//    threshold input.
	render_glow_alpha_mask(filter);

	// 3. Apply effect to texture, and render texture to video
	if (filter->blur_type == BLUR_TYPE_TRIANGULAR) {
		alpha_blur(filter->glow_size, filter->ignore_source_border, filter->alpha_blur_data,
			   filter->alpha_mask_texrender, filter->alpha_blur_data->alpha_blur_output);
	} else {
		dual_kawase_blur((int)filter->glow_size, filter->ignore_source_border, filter->alpha_blur_data,
				 filter->alpha_mask_texrender);
	}

	// 4. Render glow effect to output
	render_glow_filter(filter);

	// 5. Draw result (filter->output_texrender) to source
	glow_render_cropped_output(filter);

	filter->rendered = true;
	filter->rendering = false;
}

static obs_properties_t *properties(void *data, bool is_source, enum filter_type filter_type)
{
	glow_filter_data_t *filter = data;

	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, filter, NULL);

	if (is_source) {
		obs_property_t *stroke_source =
			obs_properties_add_list(props, "glow_source", obs_module_text("StrokeSource.Source"),
						OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(stroke_source, obs_module_text("StrokeCommon.None"), "");
		obs_enum_sources(stroke_add_source_to_list, stroke_source);
		obs_enum_scenes(stroke_add_source_to_list, stroke_source);
	}

	obs_property_t *glow_position_list = obs_properties_add_list(props, "glow_position",
								     obs_module_text("GlowShadowFilter.Position"),
								     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	if (filter_type == FILTER_TYPE_GLOW) {
		obs_property_list_add_int(glow_position_list, obs_module_text(GLOW_POSITION_OUTER_LABEL),
					  GLOW_POSITION_OUTER);
		obs_property_list_add_int(glow_position_list, obs_module_text(GLOW_POSITION_INNER_LABEL),
					  GLOW_POSITION_INNER);
	} else {
		obs_property_list_add_int(glow_position_list, obs_module_text(SHADOW_POSITION_OUTER_LABEL),
					  GLOW_POSITION_OUTER);
		obs_property_list_add_int(glow_position_list, obs_module_text(SHADOW_POSITION_INNER_LABEL),
					  GLOW_POSITION_INNER);
	}

	obs_property_set_modified_callback2(glow_position_list, setting_glow_position_modified, (void *)is_source);

	obs_properties_add_bool(props, "ignore_source_border", obs_module_text("StrokeCommon.IgnoreSourceBorder"));

	if (is_source) {
		obs_properties_add_bool(props, "fill", obs_module_text("GlowShadowFilter.FillSource"));
	}

	obs_property_t *glow_padding_list = obs_properties_add_list(props, "glow_padding",
								    obs_module_text("StrokeFilter.Padding"),
								    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(glow_padding_list, obs_module_text(PADDING_NONE_LABEL), PADDING_NONE);

	obs_property_list_add_int(glow_padding_list, obs_module_text(PADDING_AUTO_LABEL), PADDING_AUTO);

	obs_property_list_add_int(glow_padding_list, obs_module_text(PADDING_MANUAL_LABEL), PADDING_MANUAL);

	obs_property_set_modified_callback(glow_padding_list, setting_glow_padding_modified);

	obs_property_t *padding_amt = obs_properties_add_int_slider(
		props, "padding_amount", obs_module_text("StrokeFilter.Padding.Amount"), 0, 4000, 1);
	obs_property_float_set_suffix(padding_amt, "px");

	obs_property_t *overflow =
		obs_properties_add_bool(props, "padding_overflow", obs_module_text(PADDING_OVERFLOW_LABEL));
	obs_property_set_long_description(overflow, obs_module_text(PADDING_OVERFLOW_DESCRIPTION));

	obs_property_t *blur_type_list = obs_properties_add_list(props, "blur_type",
								 obs_module_text("GlowShadowFilter.BlurType"),
								 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(blur_type_list, obs_module_text(BLUR_TYPE_TRIANGULAR_LABEL), BLUR_TYPE_TRIANGULAR);
	obs_property_list_add_int(blur_type_list, obs_module_text(BLUR_TYPE_DUAL_KAWASE_LABEL), BLUR_TYPE_DUAL_KAWASE);

	obs_property_t *p = obs_properties_add_float_slider(
		props, "threshold_inner", obs_module_text("StrokeFilter.MaskThreshold"), 0.0, 100.0, 0.01);
	obs_property_float_set_suffix(p, "%");

	p = obs_properties_add_float_slider(props, "threshold_outer", obs_module_text("StrokeFilter.MaskThreshold"),
					    0.0, 100.0, 0.01);
	obs_property_float_set_suffix(p, "%");

	obs_property_t *prop = obs_properties_add_float_slider(
		props, "glow_size", obs_module_text("GlowShadowFilter.Size"), 0.0, 100.0, 1.0);
	obs_property_float_set_suffix(prop, "px");

	prop = obs_properties_add_float_slider(props, "glow_intensity", obs_module_text("GlowShadowFilter.Intensity"),
					       0.0, 400.0, 0.1);
	obs_property_float_set_suffix(prop, "%");

	if (filter_type == FILTER_TYPE_SHADOW) {
		prop = obs_properties_add_float_slider(props, "glow_offset_angle",
						       obs_module_text("ShadowFilter.OffsetAngle"), -180.0, 180.0, 0.1);
		obs_property_float_set_suffix(prop, "°");

		prop = obs_properties_add_float_slider(props, "glow_offset_distance",
						       obs_module_text("ShadowFilter.OffsetDistance"), 0, 50, 1.0);
		obs_property_float_set_suffix(prop, "px");
	}

	obs_property_t *glow_fill_method_list = obs_properties_add_list(props, "glow_fill_type",
									obs_module_text("StrokeFilter.FillType"),
									OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(glow_fill_method_list, obs_module_text(GLOW_FILL_TYPE_COLOR_LABEL),
				  GLOW_FILL_TYPE_COLOR);
	obs_property_list_add_int(glow_fill_method_list, obs_module_text(GLOW_FILL_TYPE_SOURCE_LABEL),
				  GLOW_FILL_TYPE_SOURCE);
	obs_property_list_add_int(glow_fill_method_list, obs_module_text(GLOW_FILL_TYPE_GRADIENT_LABEL),
				  GLOW_FILL_TYPE_GRADIENT);
	//obs_property_list_add_int(
	//	glow_fill_method_list,
	//	obs_module_text(GLOW_FILL_TYPE_IMAGE_LABEL),
	//	GLOW_FILL_TYPE_IMAGE);

	obs_property_set_modified_callback(glow_fill_method_list, setting_fill_type_modified);

	obs_properties_add_color_alpha(props, "glow_fill_color", obs_module_text("StrokeFilter.ColorFill"));

	obs_properties_add_path(props, "glow_fill_image", obs_module_text("StrokeFilter.ImageFill"), OBS_PATH_FILE,
				"Textures (*.bmp *.tga *.png *.jpeg *.jpg *.gif);;", NULL);

	obs_property_t *glow_fill_source_source =
		obs_properties_add_list(props, "glow_fill_source", obs_module_text("StrokeFilter.SourceFill"),
					OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(stroke_add_source_to_list, glow_fill_source_source);
	obs_enum_scenes(stroke_add_source_to_list, glow_fill_source_source);
	obs_property_list_insert_string(glow_fill_source_source, 0, "None", "");

	gradient_properties(props, GLOW_GRADIENT_PREFIX, data ? &((glow_filter_data_t *)data)->gradient : NULL);

	return props;
}

static obs_properties_t *glow_filter_properties(void *data)
{
	return properties(data, false, FILTER_TYPE_GLOW);
}

static obs_properties_t *glow_source_properties(void *data)
{
	return properties(data, true, FILTER_TYPE_GLOW);
}

static obs_properties_t *shadow_filter_properties(void *data)
{
	return properties(data, false, FILTER_TYPE_SHADOW);
}

static obs_properties_t *shadow_source_properties(void *data)
{
	return properties(data, true, FILTER_TYPE_SHADOW);
}

static void glow_filter_video_tick(void *data, float seconds)
{
	glow_filter_data_t *filter = data;

	source_ref_tick(&filter->fill_ref, seconds);
	gradient_tick(&filter->gradient, seconds);
	if (filter->is_source) {
		source_ref_tick(&filter->input_ref, seconds);
	}

	// Keep the sources we draw showing/active while we are, as a scene
	// would. A filter follows its parent, while it is enabled.
	obs_source_t *owner = filter->is_filter ? obs_filter_get_parent(filter->context) : filter->context;
	const bool enabled = !filter->is_filter || obs_source_enabled(filter->context);
	source_ref_update_activation(&filter->fill_ref, owner, enabled && filter->fill_type == GLOW_FILL_TYPE_SOURCE,
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
		filter->width = base_width;
		filter->height = base_height;
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

	if (filter->overflow || !base_width || !base_height) {
		filter->output_width = base_width;
		filter->output_height = base_height;
	} else {
		filter->output_width = base_width + filter->pad_l + filter->pad_r;
		filter->output_height = base_height + filter->pad_t + filter->pad_b;
	}
	filter->rendered = false;
}

static void glow_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "glow_size", 4.0);
	obs_data_set_default_double(settings, "glow_intensity", 100.0);
	obs_data_set_default_int(settings, "glow_fill_type", GLOW_FILL_TYPE_COLOR);
	obs_data_set_default_int(settings, "glow_fill_color", DEFAULT_COLOR_GLOW);
	obs_data_set_default_int(settings, "glow_position", GLOW_POSITION_OUTER);
	obs_data_set_default_double(settings, "glow_offset_angle", 0.0);
	obs_data_set_default_double(settings, "glow_offset_distance", 0.0);
	obs_data_set_default_double(settings, "threshold_outer", 100.0);
	obs_data_set_default_double(settings, "threshold_inner", 0.0);
	gradient_defaults(settings, GLOW_GRADIENT_PREFIX);
}

static void shadow_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "glow_size", 4.0);
	obs_data_set_default_double(settings, "glow_intensity", 100.0);
	obs_data_set_default_int(settings, "glow_fill_type", GLOW_FILL_TYPE_COLOR);
	obs_data_set_default_int(settings, "glow_fill_color", DEFAULT_COLOR_SHADOW);
	obs_data_set_default_int(settings, "glow_position", GLOW_POSITION_OUTER);
	obs_data_set_default_int(settings, "blur_type", BLUR_TYPE_TRIANGULAR);
	obs_data_set_default_double(settings, "glow_offset_angle", 45.0);
	obs_data_set_default_double(settings, "glow_offset_distance", 10.0);
	obs_data_set_default_bool(settings, "ignore_source_border", true);
	obs_data_set_default_double(settings, "threshold_outer", 100.0);
	obs_data_set_default_double(settings, "threshold_inner", 0.0);
	gradient_defaults(settings, GLOW_GRADIENT_PREFIX);
}

// Defaults for filters/sources created by this version: pad automatically
// and draw the padding outside the source's bounds, so a new outer glow or
// shadow isn't clipped at the source's edge and the source doesn't move or
// resize.
static void set_padding_defaults_v1(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "glow_padding", PADDING_AUTO);
	obs_data_set_default_bool(settings, "padding_overflow", true);
}

static void glow_filter_defaults_v1(obs_data_t *settings)
{
	glow_filter_defaults(settings);
	set_padding_defaults_v1(settings);
}

static void shadow_filter_defaults_v1(obs_data_t *settings)
{
	shadow_filter_defaults(settings);
	set_padding_defaults_v1(settings);
}

// The filter works in the space it is drawn into where it can (including
// HDR), so report that. Sources keep the default (SDR sRGB): they
// draw a glow color or SDR-rendered fill.
static enum gs_color_space glow_filter_get_color_space(void *data, size_t count,
						       const enum gs_color_space *preferred_spaces)
{
	glow_filter_data_t *filter = data;
	return filter_working_space(filter->context, count, preferred_spaces);
}

static void load_effects(glow_filter_data_t *filter)
{
	load_1d_alpha_blur_effect(filter->alpha_blur_data);
	stroke_load_effect_dual_kawase(filter->alpha_blur_data);
	load_glow_effect(filter);
}

static bool setting_fill_type_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	enum glow_fill_type fill_type = (enum glow_fill_type)obs_data_get_int(settings, "glow_fill_type");
	gradient_set_visible(props, GLOW_GRADIENT_PREFIX, fill_type == GLOW_FILL_TYPE_GRADIENT);
	if (fill_type == GLOW_FILL_TYPE_GRADIENT) {
		gradient_refresh_visibility(props, GLOW_GRADIENT_PREFIX, settings);
	}
	switch (fill_type) {
	case GLOW_FILL_TYPE_COLOR:
		stroke_setting_visibility("glow_fill_color", true, props);
		stroke_setting_visibility("glow_fill_source", false, props);
		stroke_setting_visibility("glow_fill_image", false, props);
		break;
	case GLOW_FILL_TYPE_SOURCE:
		stroke_setting_visibility("glow_fill_color", false, props);
		stroke_setting_visibility("glow_fill_source", true, props);
		stroke_setting_visibility("glow_fill_image", false, props);
		break;
	case GLOW_FILL_TYPE_IMAGE:
		stroke_setting_visibility("glow_fill_color", false, props);
		stroke_setting_visibility("glow_fill_source", false, props);
		stroke_setting_visibility("glow_fill_image", true, props);
		break;
	case GLOW_FILL_TYPE_GRADIENT:
		stroke_setting_visibility("glow_fill_color", false, props);
		stroke_setting_visibility("glow_fill_source", false, props);
		stroke_setting_visibility("glow_fill_image", false, props);
		break;
	}
	return true;
}

static bool setting_glow_position_modified(void *data, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	bool is_source = data;

	enum glow_position position = (enum glow_position)obs_data_get_int(settings, "glow_position");
	enum padding_type paddingType = (enum padding_type)obs_data_get_int(settings, "glow_padding");

	switch (position) {
	case GLOW_POSITION_INNER:
		stroke_setting_visibility("ignore_source_border", true, props);
		stroke_setting_visibility("fill", false, props);
		stroke_setting_visibility("glow_padding", false, props);
		stroke_setting_visibility("padding_amount", false, props);
		stroke_setting_visibility("padding_overflow", false, props);
		stroke_setting_visibility("threshold_inner", true, props);
		stroke_setting_visibility("threshold_outer", false, props);
		break;
	case GLOW_POSITION_OUTER:
		stroke_setting_visibility("ignore_source_border", false, props);
		stroke_setting_visibility("fill", is_source, props);
		stroke_setting_visibility("glow_padding", true, props);
		stroke_setting_visibility("padding_amount", paddingType == PADDING_MANUAL, props);
		stroke_setting_visibility("padding_overflow", paddingType != PADDING_NONE, props);
		stroke_setting_visibility("threshold_inner", false, props);
		stroke_setting_visibility("threshold_outer", true, props);
		break;
	default:
		break;
	}
	return true;
}

static bool setting_glow_padding_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	enum padding_type paddingType = (enum padding_type)obs_data_get_int(settings, "glow_padding");

	stroke_setting_visibility("padding_amount", paddingType == PADDING_MANUAL, props);
	stroke_setting_visibility("padding_overflow", paddingType != PADDING_NONE, props);
	return true;
}
