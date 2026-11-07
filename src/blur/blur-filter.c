#include "blur-filter.h"
#include <util/dstr.h>

#include "algorithms/gaussian.h"
#include "algorithms/box.h"
#include "algorithms/pixelate.h"
#include "algorithms/dual_kawase.h"
#include "algorithms/temporal.h"

// How often (seconds) to retry resolving a named source that wasn't found.
#define SOURCE_RETRY_INTERVAL 1.0f
// Settings value meaning "pixelate origin / zoom center not yet initialized".
#define CENTER_UNINITIALIZED -1.e9
#define CENTER_UNINITIALIZED_THRESHOLD -1.e8

static const char *blur_filter_name(void *type_data);
static void *blur_filter_create(obs_data_t *settings, obs_source_t *source);
static void blur_filter_defaults(obs_data_t *settings);
static void blur_filter_destroy(void *data);
static uint32_t blur_filter_width(void *data);
static uint32_t blur_filter_height(void *data);
static void blur_filter_rename(void *data, calldata_t *call_data);
static void blur_filter_update(void *data, obs_data_t *settings);
static void blur_filter_video_render(void *data, gs_effect_t *effect);
static void blur_filter_video_tick(void *data, float seconds);
static enum gs_color_space blur_filter_get_color_space(void *data, size_t count,
						       const enum gs_color_space *preferred_spaces);
static obs_properties_t *blur_filter_properties(void *data);
static void blur_filter_reload_effect(blur_filter_data_t *filter);
static void load_static_effects(blur_filter_data_t *filter);

static bool setting_blur_algorithm_modified(void *data, obs_properties_t *props, obs_property_t *p,
					    obs_data_t *settings);
static bool setting_effect_mask_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings);
static bool setting_blur_types_modified(void *data, obs_properties_t *props, obs_property_t *p, obs_data_t *settings);
static bool setting_pixelate_animate_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings);
static bool setting_effect_mask_source_filter_modified(obs_properties_t *props, obs_property_t *p,
						       obs_data_t *settings);
static void blur_setting_visibility(const char *prop_name, bool visible, obs_properties_t *props);
static void set_blur_radius_settings(const char *name, float min_val, float max_val, float step_size,
				     obs_properties_t *props);
static bool settings_blur_area(obs_properties_t *props, obs_data_t *settings);
static bool settings_blur_directional(obs_properties_t *props);
static bool settings_blur_zoom(obs_properties_t *props);
static bool settings_blur_tilt_shift(obs_properties_t *props);
static bool settings_blur_vector(obs_properties_t *props, blur_filter_data_t *filter);

static void apply_effect_mask(blur_filter_data_t *filter);
static void apply_effect_mask_box(blur_filter_data_t *filter, float left, float right, float top, float bot,
				  float corner_radius_pct, float feathering_pct, bool invert);
static void apply_effect_mask_source(blur_filter_data_t *filter);
static void apply_effect_mask_circle(blur_filter_data_t *filter);
static void effect_mask_load_effect(blur_filter_data_t *filter);

static bool blur_filter_enable_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed);
static bool blur_filter_disable_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed);

struct obs_source_info spectrum_blur_filter = {.id = "spectrum_blur_filter",
					       .type = OBS_SOURCE_TYPE_FILTER,
					       .output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
					       .get_name = blur_filter_name,
					       .create = blur_filter_create,
					       .destroy = blur_filter_destroy,
					       .update = blur_filter_update,
					       .video_render = blur_filter_video_render,
					       .video_tick = blur_filter_video_tick,
					       .video_get_color_space = blur_filter_get_color_space,
					       .get_width = blur_filter_width,
					       .get_height = blur_filter_height,
					       .get_properties = blur_filter_properties,
					       .get_defaults = blur_filter_defaults};

static const enum gs_color_space pipeline_color_spaces[] = {
	GS_CS_SRGB,
	GS_CS_SRGB_16F,
	GS_CS_709_EXTENDED,
};

static const char *blur_filter_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("BlurFilter");
}

static void blur_filter_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "blur_algorithm", ALGO_GAUSSIAN);
	obs_data_set_default_int(settings, "blur_type", TYPE_AREA);
	obs_data_set_default_double(settings, "radius", 10.0);
	obs_data_set_default_string(settings, "background", "");
	obs_data_set_default_int(settings, "passes", 1);
	obs_data_set_default_int(settings, "kawase_passes", 10);
	obs_data_set_default_string(settings, "effect_mask_source_source", "");
	obs_data_set_default_double(settings, "effect_mask_source_filter_multiplier", 1.0);
	obs_data_set_default_double(settings, "pixelate_origin_x", CENTER_UNINITIALIZED);
	obs_data_set_default_double(settings, "pixelate_origin_y", CENTER_UNINITIALIZED);
	obs_data_set_default_double(settings, "pixelate_animation_speed", 50.0);
	obs_data_set_default_double(settings, "effect_mask_circle_center_x", 50.0);
	obs_data_set_default_double(settings, "effect_mask_circle_center_y", 50.0);
	obs_data_set_default_double(settings, "effect_mask_circle_radius", 40.0);

	obs_data_set_default_double(settings, "effect_mask_rect_center_x", 50.0);
	obs_data_set_default_double(settings, "effect_mask_rect_center_y", 50.0);
	obs_data_set_default_double(settings, "effect_mask_rect_width", 50.0);
	obs_data_set_default_double(settings, "effect_mask_rect_height", 50.0);

	obs_data_set_default_double(settings, "effect_mask_crop_top", 20.0);
	obs_data_set_default_double(settings, "effect_mask_crop_bottom", 20.0);
	obs_data_set_default_double(settings, "effect_mask_crop_left", 20.0);
	obs_data_set_default_double(settings, "effect_mask_crop_right", 20.0);
	obs_data_set_default_string(settings, "vector_source", "");
	obs_data_set_default_double(settings, "temporal_current_weight", 0.95);
	obs_data_set_default_double(settings, "temporal_clear_threshold", 5.0);
}

static void *blur_filter_create(obs_data_t *settings, obs_source_t *source)
{
	blur_filter_data_t *filter = bzalloc(sizeof(blur_filter_data_t));

	if (pthread_mutex_init(&filter->sources_mutex, NULL) != 0) {
		blog(LOG_ERROR, "[Spectrum] Failed to create sources mutex");
		bfree(filter);
		return NULL;
	}

	// bzalloc zeroes everything; only non-zero initial state is set here.
	filter->context = source;
	filter->hotkey = OBS_INVALID_HOTKEY_PAIR_ID;
	filter->blur_algorithm = ALGO_NONE;
	filter->blur_algorithm_last = -1;
	filter->blur_type = TYPE_NONE;
	filter->blur_type_last = -1;
	filter->pixelate_type_last = -1;
	filter->mask_type_last = -1;
	filter->passes = 1;
	filter->kawase_passes = 1.0f;
	filter->kernel_radius_last = -1.0f;
	filter->mask_source_multiplier = 1.0f;
	filter->mask_source_filter_type = EFFECT_MASK_SOURCE_FILTER_ALPHA;
	filter->color_space = GS_CS_SRGB;
	filter->color_format = GS_RGBA;
	filter->reload = true;

	da_init(filter->kernel);
	da_init(filter->offset);

	signal_handler_t *sh = obs_source_get_signal_handler(filter->context);
	signal_handler_connect_ref(sh, "rename", blur_filter_rename, filter);

	// The device type selects D3D vs OpenGL shader variants, so it must be
	// known before the first update loads any effects.
	obs_enter_graphics();
	filter->device_type = gs_get_device_type();
	obs_leave_graphics();

	load_static_effects(filter);
	blur_filter_update(filter, settings);
	return filter;
}

static void external_source_free(external_source_t *src)
{
	obs_weak_source_release(src->weak);
	src->weak = NULL;
	dstr_free(&src->name);
}

static void blur_filter_destroy(void *data)
{
	blur_filter_data_t *filter = data;

	signal_handler_t *sh = obs_source_get_signal_handler(filter->context);
	signal_handler_disconnect(sh, "rename", blur_filter_rename, filter);

	if (filter->hotkey != OBS_INVALID_HOTKEY_PAIR_ID) {
		obs_hotkey_pair_unregister(filter->hotkey);
	}

	gs_effect_t *effects[] = {
		filter->effect,        filter->effect_2,           filter->composite_effect,
		filter->mix_effect,    filter->effect_mask_effect, filter->pixelate_effect,
		filter->output_effect, filter->gradient_effect,    filter->gv_effect,
	};
	gs_texrender_t *texrenders[] = {
		filter->render,
		filter->render2,
		filter->background_texrender,
		filter->input_texrender,
		filter->output_texrender,
		filter->composite_render,
		filter->pixelate_texrender,
		filter->mask_source_texrender,
		filter->kawase_base_texrender,
		filter->temporal_history,
		filter->vb_gradient,
		filter->vb_smoothed_gradient,
		filter->vector_source_texrender,
	};

	obs_enter_graphics();
	for (size_t i = 0; i < OBS_COUNTOF(effects); i++) {
		if (effects[i])
			gs_effect_destroy(effects[i]);
	}
	for (size_t i = 0; i < OBS_COUNTOF(texrenders); i++) {
		if (texrenders[i])
			gs_texrender_destroy(texrenders[i]);
	}
	if (filter->kernel_texture) {
		gs_texture_destroy(filter->kernel_texture);
	}
	if (filter->mask_image)
		gs_image_file_free(filter->mask_image);
	obs_leave_graphics();
	bfree(filter->mask_image);

	external_source_free(&filter->background);
	external_source_free(&filter->mask_source);
	external_source_free(&filter->vector_source);

	da_free(filter->offset);
	da_free(filter->kernel);

	dstr_free(&filter->filter_name);
	dstr_free(&filter->mask_image_path);

	pthread_mutex_destroy(&filter->sources_mutex);
	bfree(filter);
}

static uint32_t blur_filter_width(void *data)
{
	struct blur_filter_data *filter = data;
	return filter->width;
}

static uint32_t blur_filter_height(void *data)
{
	struct blur_filter_data *filter = data;
	return filter->height;
}

static void blur_filter_rename(void *data, calldata_t *call_data)
{
	blur_filter_data_t *filter = data;
	const char *new_name = calldata_string(call_data, "new_name");

	dstr_copy(&filter->filter_name, new_name);
	struct dstr enable;
	struct dstr disable;

	dstr_init_copy(&enable, obs_module_text("BlurFilter.Enable"));
	dstr_init_copy(&disable, obs_module_text("BlurFilter.Disable"));
	dstr_cat(&enable, " ");
	dstr_cat(&enable, new_name);
	dstr_cat(&disable, " ");
	dstr_cat(&disable, new_name);
	obs_hotkey_pair_set_descriptions(filter->hotkey, enable.array, disable.array);
	dstr_free(&enable);
	dstr_free(&disable);
}

/* ------------------------------------------------------------------------
 * External (named) sources
 * --------------------------------------------------------------------- */

// NULL-safe string comparison (an empty dstr has a NULL array).
static bool dstr_equals(const struct dstr *str, const char *value)
{
	return strcmp(str->array ? str->array : "", value ? value : "") == 0;
}

static obs_weak_source_t *weak_source_by_name(const char *name)
{
	if (!name || !*name)
		return NULL;
	obs_source_t *source = obs_get_source_by_name(name);
	if (!source)
		return NULL;
	obs_weak_source_t *weak = obs_source_get_weak_source(source);
	obs_source_release(source);
	return weak;
}

// Points `src` at the source named `name` (may be empty).
static void external_source_set(blur_filter_data_t *filter, external_source_t *src, const char *name)
{
	// Resolve outside the lock; obs_get_source_by_name takes OBS locks.
	obs_weak_source_t *weak = weak_source_by_name(name);

	pthread_mutex_lock(&filter->sources_mutex);
	obs_weak_source_t *old = src->weak;
	src->weak = weak;
	dstr_copy(&src->name, name ? name : "");
	pthread_mutex_unlock(&filter->sources_mutex);

	obs_weak_source_release(old);
}

// Re-resolves `src` by name if it is set but not currently resolved, e.g.
// because the source didn't exist yet when settings were loaded, or it was
// deleted and recreated.
static void external_source_refresh(blur_filter_data_t *filter, external_source_t *src)
{
	struct dstr name = {0};

	pthread_mutex_lock(&filter->sources_mutex);
	const bool needs_resolve = src->name.len > 0 && (!src->weak || obs_weak_source_expired(src->weak));
	if (needs_resolve)
		dstr_copy_dstr(&name, &src->name);
	pthread_mutex_unlock(&filter->sources_mutex);

	if (!needs_resolve)
		return;

	obs_weak_source_t *weak = weak_source_by_name(name.array);
	obs_weak_source_t *to_release = weak;

	if (weak) {
		pthread_mutex_lock(&filter->sources_mutex);
		// Only apply if the setting didn't change while resolving.
		if (dstr_equals(&src->name, name.array)) {
			to_release = src->weak;
			src->weak = weak;
		}
		pthread_mutex_unlock(&filter->sources_mutex);
	}

	obs_weak_source_release(to_release);
	dstr_free(&name);
}

obs_source_t *external_source_get(blur_filter_data_t *filter, external_source_t *src)
{
	pthread_mutex_lock(&filter->sources_mutex);
	obs_source_t *source = src->weak ? obs_weak_source_get_source(src->weak) : NULL;
	pthread_mutex_unlock(&filter->sources_mutex);
	return source;
}

gs_texture_t *render_source_to_texrender(obs_source_t *source, gs_texrender_t **texrender, enum gs_color_space space)
{
	const uint32_t width = obs_source_get_width(source);
	const uint32_t height = obs_source_get_height(source);
	if (width == 0 || height == 0)
		return NULL;

	*texrender = blur_create_or_reset_texrender(*texrender, gs_get_format_from_space(space));

	bool rendered = false;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin_with_color_space(*texrender, width, height, space)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		obs_source_video_render(source);
		gs_texrender_end(*texrender);
		rendered = true;
	}
	gs_blend_state_pop();

	return rendered ? gs_texrender_get_texture(*texrender) : NULL;
}

/* ------------------------------------------------------------------------
 * Settings
 * --------------------------------------------------------------------- */

// Loads the image mask file. Decoding happens on the calling thread; only
// the GPU upload and the swap take the graphics lock.
static void update_mask_image(blur_filter_data_t *filter, const char *path)
{
	if (dstr_equals(&filter->mask_image_path, path))
		return;
	dstr_copy(&filter->mask_image_path, path);

	gs_image_file_t *image = NULL;
	if (path && *path) {
		image = bzalloc(sizeof(gs_image_file_t));
		gs_image_file_init(image, path);
		if (!image->loaded) {
			blog(LOG_WARNING, "[Spectrum] Unable to load mask image: %s", path);
		}
	}

	obs_enter_graphics();
	gs_image_file_t *old = filter->mask_image;
	if (image)
		gs_image_file_init_texture(image);
	filter->mask_image = image;
	if (old)
		gs_image_file_free(old);
	obs_leave_graphics();
	bfree(old);
}

static void blur_filter_update(void *data, obs_data_t *settings)
{
	struct blur_filter_data *filter = data;

	const int blur_algorithm = (int)obs_data_get_int(settings, "blur_algorithm");
	const int blur_type = (int)obs_data_get_int(settings, "blur_type");
	const int pixelate_type = (int)obs_data_get_int(settings, "pixelate_type");
	const int mask_type = (int)obs_data_get_int(settings, "effect_mask");

	filter->temporal_current_weight =
		1.0f - (float)obs_data_get_double(settings, "temporal_current_weight") * 0.94f;

	filter->temporal_clear_threshold = (float)obs_data_get_double(settings, "temporal_clear_threshold") / 100.0f;

	filter->pixelate_smoothing_pct = (float)obs_data_get_double(settings, "pixelate_smoothing_pct");

	const double origin_x = obs_data_get_double(settings, "pixelate_origin_x");
	const double origin_y = obs_data_get_double(settings, "pixelate_origin_y");
	filter->center_uninitialized = origin_x < CENTER_UNINITIALIZED_THRESHOLD;
	if (!filter->center_uninitialized) {
		filter->pixelate_tessel_center.x = (float)origin_x;
		filter->pixelate_tessel_center.y = (float)origin_y;
	}

	filter->pixelate_animate = obs_data_get_bool(settings, "pixelate_animate");
	filter->pixelate_animation_speed = (float)obs_data_get_double(settings, "pixelate_animation_speed") / 100.0f;
	filter->pixelate_animation_time = (float)obs_data_get_double(settings, "pixelate_time");

	const double theta = M_PI * (float)obs_data_get_double(settings, "pixelate_rotation") / 180.0;
	filter->pixelate_cos_theta = (float)cos(theta);
	filter->pixelate_sin_theta = (float)sin(theta);
	filter->pixelate_sin_rtheta = (float)sin(-theta);
	filter->pixelate_cos_rtheta = (float)cos(-theta);

	filter->mask_crop_top = (float)obs_data_get_double(settings, "effect_mask_crop_top");
	filter->mask_crop_bot = (float)obs_data_get_double(settings, "effect_mask_crop_bottom");
	filter->mask_crop_left = (float)obs_data_get_double(settings, "effect_mask_crop_left");
	filter->mask_crop_right = (float)obs_data_get_double(settings, "effect_mask_crop_right");
	filter->mask_crop_corner_radius = (float)obs_data_get_double(settings, "effect_mask_crop_corner_radius");
	filter->mask_crop_feathering = (float)obs_data_get_double(settings, "effect_mask_crop_feathering");
	filter->mask_crop_invert = obs_data_get_bool(settings, "effect_mask_crop_invert");

	filter->mask_source_filter_type = (int)obs_data_get_int(settings, "effect_mask_source_filter_list");
	switch (filter->mask_source_filter_type) {
	case EFFECT_MASK_SOURCE_FILTER_ALPHA:
		vec4_set(&filter->mask_source_weights, 0.0f, 0.0f, 0.0f, 1.0f);
		break;
	case EFFECT_MASK_SOURCE_FILTER_GRAYSCALE:
		vec4_set(&filter->mask_source_weights, 0.33334f, 0.33333f, 0.33333f, 0.0f);
		break;
	case EFFECT_MASK_SOURCE_FILTER_LUMINOSITY:
		vec4_set(&filter->mask_source_weights, 0.299f, 0.587f, 0.114f, 0.0f);
		break;
	case EFFECT_MASK_SOURCE_FILTER_SLIDERS:
		vec4_set(&filter->mask_source_weights,
			 (float)obs_data_get_double(settings, "effect_mask_source_filter_red"),
			 (float)obs_data_get_double(settings, "effect_mask_source_filter_green"),
			 (float)obs_data_get_double(settings, "effect_mask_source_filter_blue"),
			 (float)obs_data_get_double(settings, "effect_mask_source_filter_alpha"));
		break;
	}

	external_source_set(filter, &filter->mask_source, obs_data_get_string(settings, "effect_mask_source_source"));

	update_mask_image(filter, obs_data_get_string(settings, "effect_mask_source_file"));

	filter->mask_source_multiplier = (float)obs_data_get_double(settings, "effect_mask_source_filter_multiplier");

	filter->mask_source_invert = obs_data_get_bool(settings, "effect_mask_source_invert");

	filter->mask_circle_center_x = (float)obs_data_get_double(settings, "effect_mask_circle_center_x");
	filter->mask_circle_center_y = (float)obs_data_get_double(settings, "effect_mask_circle_center_y");
	filter->mask_circle_radius = (float)obs_data_get_double(settings, "effect_mask_circle_radius");
	filter->mask_circle_feathering = (float)obs_data_get_double(settings, "effect_mask_circle_feathering");
	filter->mask_circle_inv = obs_data_get_bool(settings, "effect_mask_circle_invert");

	filter->mask_rect_center_x = (float)obs_data_get_double(settings, "effect_mask_rect_center_x");
	filter->mask_rect_center_y = (float)obs_data_get_double(settings, "effect_mask_rect_center_y");
	filter->mask_rect_width = (float)obs_data_get_double(settings, "effect_mask_rect_width");
	filter->mask_rect_height = (float)obs_data_get_double(settings, "effect_mask_rect_height");
	filter->mask_rect_corner_radius = (float)obs_data_get_double(settings, "effect_mask_rect_corner_radius");
	filter->mask_rect_feathering = (float)obs_data_get_double(settings, "effect_mask_rect_feathering");
	filter->mask_rect_inv = obs_data_get_bool(settings, "effect_mask_rect_invert");

	filter->radius = (float)obs_data_get_double(settings, "radius");
	filter->passes = (int)obs_data_get_int(settings, "passes");
	filter->kawase_passes = (float)obs_data_get_double(settings, "kawase_passes");

	if (!filter->center_uninitialized) {
		filter->center_x = (float)obs_data_get_double(settings, "center_x");
		filter->center_y = (float)obs_data_get_double(settings, "center_y");
	}
	filter->inactive_radius = (float)obs_data_get_double(settings, "inactive_radius");

	filter->angle = (float)obs_data_get_double(settings, "angle");
	filter->tilt_shift_center = (float)obs_data_get_double(settings, "tilt_shift_center");
	filter->tilt_shift_width = (float)obs_data_get_double(settings, "tilt_shift_width");
	filter->tilt_shift_angle = (float)obs_data_get_double(settings, "tilt_shift_angle");

	filter->vector_blur_channel = (int)obs_data_get_int(settings, "vector_blur_channel");
	filter->vector_blur_amount = (float)obs_data_get_double(settings, "vector_blur_amount");
	filter->vector_blur_smoothing = (float)obs_data_get_double(settings, "vector_blur_smoothing");
	filter->vector_blur_type = (int)obs_data_get_int(settings, "vector_gradient_type");

	external_source_set(filter, &filter->vector_source, obs_data_get_string(settings, "vector_source"));
	external_source_set(filter, &filter->background, obs_data_get_string(settings, "background"));

	// Changing the algorithm/type swaps effects, parameter handles and
	// render callbacks. Do it under the graphics lock so the render thread
	// never sees a mix of old and new state.
	if (blur_algorithm != filter->blur_algorithm_last || blur_type != filter->blur_type_last ||
	    pixelate_type != filter->pixelate_type_last) {
		filter->reload = true;
	}

	bool reloaded = false;
	if (filter->reload || mask_type != filter->mask_type_last) {
		obs_enter_graphics();
		filter->blur_algorithm = blur_algorithm;
		filter->blur_algorithm_last = blur_algorithm;
		filter->blur_type = blur_type;
		filter->blur_type_last = blur_type;
		filter->pixelate_type = pixelate_type;
		filter->pixelate_type_last = pixelate_type;
		if (filter->reload) {
			filter->reload = false;
			blur_filter_reload_effect(filter);
			reloaded = true;
		}
		if (mask_type != filter->mask_type_last) {
			filter->mask_type = mask_type;
			filter->mask_type_last = mask_type;
			effect_mask_load_effect(filter);
		}
		obs_leave_graphics();
	}

	if (reloaded) {
		obs_source_update_properties(filter->context);
	}

	if (filter->update) {
		filter->update(filter);
	}
}

/* ------------------------------------------------------------------------
 * Rendering
 * --------------------------------------------------------------------- */

static enum gs_color_space blur_filter_get_color_space(void *data, size_t count,
						       const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(count);
	UNUSED_PARAMETER(preferred_spaces);
	blur_filter_data_t *filter = data;

	// The filter renders in the same space as its input (see
	// get_input_source); report that so OBS can convert as needed.
	obs_source_t *target = obs_filter_get_target(filter->context);
	if (!target)
		return GS_CS_SRGB;
	return obs_source_get_color_space(target, OBS_COUNTOF(pipeline_color_spaces), pipeline_color_spaces);
}

// Renders the filter's input (everything upstream of this filter) into
// filter->input_texrender, as non-premultiplied color. This is the only
// place the upstream chain is rendered each frame.
static bool get_input_source(blur_filter_data_t *filter)
{
	gs_effect_t *pass_through = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	obs_source_t *target = obs_filter_get_target(filter->context);
	if (!target)
		return false;

	const enum gs_color_space space =
		obs_source_get_color_space(target, OBS_COUNTOF(pipeline_color_spaces), pipeline_color_spaces);
	const enum gs_color_format format = gs_get_format_from_space(space);

	if (space != filter->color_space) {
		// History is in the wrong format/space now.
		filter->temporal_prior_stored = false;
	}
	filter->color_space = space;
	filter->color_format = format;

	filter->input_texrender = blur_create_or_reset_texrender(filter->input_texrender, format);

	if (!obs_source_process_filter_begin_with_color_space(filter->context, format, space,
							      OBS_ALLOW_DIRECT_RENDERING)) {
		return false;
	}

	if (!gs_texrender_begin_with_color_space(filter->input_texrender, filter->width, filter->height, space)) {
		return false;
	}

	blur_set_blending_parameters();
	gs_ortho(0.0f, (float)filter->width, 0.0f, (float)filter->height, -100.0f, 100.0f);
	// The incoming source is pre-multiplied alpha, so use the
	// OBS default effect "DrawAlphaDivide" technique to convert
	// the colors back into non-pre-multiplied space.
	obs_source_process_filter_tech_end(filter->context, pass_through, filter->width, filter->height,
					   "DrawAlphaDivide");
	gs_blend_state_pop();
	gs_texrender_end(filter->input_texrender);
	return true;
}

// Draws the final result into the current render target. This draws the
// cached output texture directly rather than going through
// obs_source_process_filter_begin/end, which would render the whole
// upstream chain again.
static void draw_output_to_source(blur_filter_data_t *filter)
{
	gs_texture_t *texture = filter->output_texrender ? gs_texrender_get_texture(filter->output_texrender) : NULL;
	if (!texture && filter->input_texrender) {
		// The algorithm produced nothing (e.g. effect failed to load);
		// show the unfiltered input rather than nothing.
		texture = gs_texrender_get_texture(filter->input_texrender);
	}
	gs_effect_t *effect = filter->output_effect;
	if (!texture || !effect || !filter->param_output_image) {
		return;
	}

	// SDR pipeline textures hold sRGB-encoded values. When drawing into a
	// linear-aware target, decode them and let the framebuffer re-encode.
	// Float (HDR) pipelines already hold linear values.
	const bool linear_srgb = filter->color_space == GS_CS_SRGB && gs_get_linear_srgb();
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(linear_srgb);

	gs_effect_set_texture(filter->param_output_image, texture);

	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);

	const char *technique = linear_srgb ? "Draw" : "DrawPassThrough";
	while (gs_effect_loop(effect, technique))
		gs_draw_sprite(texture, 0, filter->width, filter->height);

	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);
}

static void blur_filter_video_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct blur_filter_data *filter = data;

	// Already rendered this frame (e.g. the source is shown in more than
	// one view); just draw the cached result.
	if (filter->rendered) {
		draw_output_to_source(filter);
		return;
	}

	if (filter->rendering || !filter->video_render || filter->width == 0 || filter->height == 0) {
		obs_source_skip_video_filter(filter->context);
		return;
	}

	filter->rendering = true;

	// 1. Get the input source as a texture renderer
	//    accessed as filter->input_texrender after call
	if (!get_input_source(filter)) {
		filter->rendering = false;
		obs_source_skip_video_filter(filter->context);
		return;
	}

	// 2. Apply effect to texture, result in filter->output_texrender
	filter->video_render(filter);

	// 3. Apply mask to texture if one is selected by the user.
	if (filter->mask_type != EFFECT_MASK_TYPE_NONE) {
		apply_effect_mask(filter);
	}

	// 4. Draw result (filter->output_texrender) to source
	draw_output_to_source(filter);
	filter->rendered = true;
	filter->rendering = false;
}

gs_texture_t *blend_composite(gs_texture_t *texture, blur_filter_data_t *data)
{
	if (!texture || !data->composite_effect)
		return texture;

	obs_source_t *source = external_source_get(data, &data->background);
	if (!source)
		return texture;

	// Render the background in the pipeline's color space so both inputs
	// to the composite are comparable.
	gs_texture_t *background = render_source_to_texrender(source, &data->background_texrender, data->color_space);
	obs_source_release(source);
	if (!background)
		return texture;

	data->composite_render = blur_create_or_reset_texrender(data->composite_render, data->color_format);

	// Composite in linear light: decode both inputs, re-encode on write.
	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);

	if (data->param_composite_image)
		gs_effect_set_texture_srgb(data->param_composite_image, texture);
	if (data->param_background)
		gs_effect_set_texture_srgb(data->param_background, background);

	blur_set_blending_parameters();
	if (gs_texrender_begin(data->composite_render, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(data->composite_effect, "Draw"))
			gs_draw_sprite(texture, 0, data->width, data->height);
		gs_texrender_end(data->composite_render);
		texture = gs_texrender_get_texture(data->composite_render);
	}
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous_srgb);

	return texture;
}

/* ------------------------------------------------------------------------
 * Effect masks
 * --------------------------------------------------------------------- */

// Moves the blurred result into filter->render and binds the inputs shared
// by every mask effect. Returns false (leaving the output untouched) if the
// mask cannot be applied.
static bool begin_mask_pass(blur_filter_data_t *filter)
{
	gs_effect_t *effect = filter->effect_mask_effect;
	gs_texture_t *texture = gs_texrender_get_texture(filter->input_texrender);
	gs_texture_t *filtered_texture = filter->output_texrender ? gs_texrender_get_texture(filter->output_texrender)
								  : NULL;

	if (!effect || !texture || !filtered_texture) {
		return false;
	}

	gs_texrender_t *tmp = filter->output_texrender;
	filter->output_texrender = filter->render;
	filter->render = tmp;

	if (filter->param_mask_image)
		gs_effect_set_texture(filter->param_mask_image, texture);
	if (filter->param_filtered_image)
		gs_effect_set_texture(filter->param_filtered_image, filtered_texture);
	return true;
}

// Draws the mask effect into filter->output_texrender.
static void draw_mask_pass(blur_filter_data_t *filter)
{
	gs_effect_t *effect = filter->effect_mask_effect;
	gs_texture_t *texture = gs_texrender_get_texture(filter->input_texrender);

	blur_set_blending_parameters();

	filter->output_texrender = blur_create_or_reset_texrender(filter->output_texrender, filter->color_format);

	if (gs_texrender_begin(filter->output_texrender, filter->width, filter->height)) {
		gs_ortho(0.0f, (float)filter->width, 0.0f, (float)filter->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture, 0, filter->width, filter->height);
		gs_texrender_end(filter->output_texrender);
	}
	gs_blend_state_pop();
}

static void apply_effect_mask(blur_filter_data_t *filter)
{
	switch (filter->mask_type) {
	case EFFECT_MASK_TYPE_CROP:
		apply_effect_mask_box(filter, filter->mask_crop_left / 100.0f, filter->mask_crop_right / 100.0f,
				      filter->mask_crop_top / 100.0f, filter->mask_crop_bot / 100.0f,
				      filter->mask_crop_corner_radius, filter->mask_crop_feathering,
				      filter->mask_crop_invert);
		break;
	case EFFECT_MASK_TYPE_RECT: {
		const float half_w = filter->mask_rect_width / 2.0f;
		const float half_h = filter->mask_rect_height / 2.0f;
		const float left = (filter->mask_rect_center_x - half_w) / 100.0f;
		const float right = (100.0f - filter->mask_rect_center_x - half_w) / 100.0f;
		const float top = (filter->mask_rect_center_y - half_h) / 100.0f;
		const float bot = (100.0f - filter->mask_rect_center_y - half_h) / 100.0f;
		apply_effect_mask_box(filter, left, right, top, bot, filter->mask_rect_corner_radius,
				      filter->mask_rect_feathering, filter->mask_rect_inv);
		break;
	}
	case EFFECT_MASK_TYPE_CIRCLE:
		apply_effect_mask_circle(filter);
		break;
	case EFFECT_MASK_TYPE_SOURCE:
	case EFFECT_MASK_TYPE_IMAGE:
		apply_effect_mask_source(filter);
		break;
	}
}

static void apply_effect_mask_source(blur_filter_data_t *filter)
{
	gs_texture_t *alpha_texture = NULL;
	if (filter->mask_type == EFFECT_MASK_TYPE_SOURCE) {
		obs_source_t *source = external_source_get(filter, &filter->mask_source);
		if (!source) {
			return;
		}
		// The mask is used as weights, so always render it as SDR.
		alpha_texture = render_source_to_texrender(source, &filter->mask_source_texrender, GS_CS_SRGB);
		obs_source_release(source);
	} else {
		alpha_texture = filter->mask_image ? filter->mask_image->texture : NULL;
	}

	if (!begin_mask_pass(filter)) {
		return;
	}

	if (filter->param_mask_source_alpha_source) {
		gs_effect_set_texture(filter->param_mask_source_alpha_source, alpha_texture);
	}
	if (filter->param_mask_source_invert) {
		gs_effect_set_bool(filter->param_mask_source_invert, filter->mask_source_invert);
	}
	if (filter->param_mask_source_rgba_weights) {
		gs_effect_set_vec4(filter->param_mask_source_rgba_weights, &filter->mask_source_weights);
	}
	if (filter->param_mask_source_multiplier) {
		gs_effect_set_float(filter->param_mask_source_multiplier, filter->mask_source_multiplier);
	}

	draw_mask_pass(filter);
}

// Shared by the crop and rectangle masks. Margins are fractions [0,1] of the
// source size measured in from each edge.
static void apply_effect_mask_box(blur_filter_data_t *filter, float left, float right, float top, float bot,
				  float corner_radius_pct, float feathering_pct, bool invert)
{
	if (!begin_mask_pass(filter)) {
		return;
	}

	const float box_w = 1.0f - right - left;
	const float box_h = 1.0f - bot - top;
	const float min_dim = (float)fmin(filter->width, filter->height);

	struct vec2 scale;
	scale.x = 1.0f / (float)fmax(box_w, 1.e-6f);
	scale.y = 1.0f / (float)fmax(box_h, 1.e-6f);
	if (filter->param_mask_crop_scale) {
		gs_effect_set_vec2(filter->param_mask_crop_scale, &scale);
	}

	struct vec2 box_ar;
	box_ar.x = box_w * filter->width / min_dim;
	box_ar.y = box_h * filter->height / min_dim;
	if (filter->param_mask_crop_box_aspect_ratio) {
		gs_effect_set_vec2(filter->param_mask_crop_box_aspect_ratio, &box_ar);
	}

	struct vec2 offset;
	offset.x = box_w > 0.0f ? left : -1000.0f;
	offset.y = box_h > 0.0f ? top : -1000.0f;
	if (filter->param_mask_crop_offset) {
		gs_effect_set_vec2(filter->param_mask_crop_offset, &offset);
	}

	if (filter->param_mask_crop_invert) {
		gs_effect_set_bool(filter->param_mask_crop_invert, invert);
	}

	const float radius = corner_radius_pct / 100.0f * (float)fmin(box_ar.x, box_ar.y);
	if (filter->param_mask_crop_corner_radius) {
		gs_effect_set_float(filter->param_mask_crop_corner_radius, radius);
	}

	if (filter->param_mask_crop_feathering) {
		gs_effect_set_float(filter->param_mask_crop_feathering, feathering_pct / 100.0f);
	}

	draw_mask_pass(filter);
}

static void apply_effect_mask_circle(blur_filter_data_t *filter)
{
	if (!begin_mask_pass(filter)) {
		return;
	}

	struct vec2 center;
	center.x = filter->mask_circle_center_x / 100.0f;
	center.y = filter->mask_circle_center_y / 100.0f;
	if (filter->param_mask_circle_center) {
		gs_effect_set_vec2(filter->param_mask_circle_center, &center);
	}

	const float min_dim = (float)fmin(filter->width, filter->height);
	struct vec2 uv_scale;
	uv_scale.x = filter->width / min_dim;
	uv_scale.y = filter->height / min_dim;
	if (filter->param_mask_circle_uv_scale) {
		gs_effect_set_vec2(filter->param_mask_circle_uv_scale, &uv_scale);
	}

	if (filter->param_mask_circle_inv) {
		gs_effect_set_bool(filter->param_mask_circle_inv, filter->mask_circle_inv);
	}

	if (filter->param_mask_circle_radius) {
		gs_effect_set_float(filter->param_mask_circle_radius, filter->mask_circle_radius / 100.0f);
	}
	if (filter->param_mask_circle_feathering) {
		gs_effect_set_float(filter->param_mask_circle_feathering, filter->mask_circle_feathering / 100.0f);
	}

	draw_mask_pass(filter);
}

static obs_properties_t *blur_filter_properties(void *data)
{
	blur_filter_data_t *filter = data;

	obs_properties_t *props = obs_properties_create();
	obs_properties_set_param(props, filter, NULL);

	obs_property_t *blur_algorithms = obs_properties_add_list(props, "blur_algorithm",
								  obs_module_text("BlurFilter.BlurAlgorithm"),
								  OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(blur_algorithms, obs_module_text(ALGO_GAUSSIAN_LABEL), ALGO_GAUSSIAN);
	obs_property_list_add_int(blur_algorithms, obs_module_text(ALGO_BOX_LABEL), ALGO_BOX);
	obs_property_list_add_int(blur_algorithms, obs_module_text(ALGO_DUAL_KAWASE_LABEL), ALGO_DUAL_KAWASE);
	obs_property_list_add_int(blur_algorithms, obs_module_text(ALGO_PIXELATE_LABEL), ALGO_PIXELATE);
	obs_property_list_add_int(blur_algorithms, obs_module_text(ALGO_TEMPORAL_LABEL), ALGO_TEMPORAL);

	obs_property_set_modified_callback2(blur_algorithms, setting_blur_algorithm_modified, data);

	obs_property_t *blur_types = obs_properties_add_list(props, "blur_type", obs_module_text("BlurFilter.BlurType"),
							     OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(blur_types, obs_module_text(TYPE_AREA_LABEL), TYPE_AREA);
	obs_property_list_add_int(blur_types, obs_module_text(TYPE_DIRECTIONAL_LABEL), TYPE_DIRECTIONAL);
	obs_property_list_add_int(blur_types, obs_module_text(TYPE_ZOOM_LABEL), TYPE_ZOOM);
	obs_property_list_add_int(blur_types, obs_module_text(TYPE_MOTION_LABEL), TYPE_MOTION);
	obs_property_list_add_int(blur_types, obs_module_text(TYPE_TILTSHIFT_LABEL), TYPE_TILTSHIFT);
	obs_property_set_modified_callback2(blur_types, setting_blur_types_modified, data);

	obs_property_t *pixelate_types = obs_properties_add_list(props, "pixelate_type",
								 obs_module_text("BlurFilter.PixelateType"),
								 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(pixelate_types, obs_module_text(PIXELATE_TYPE_SQUARE_LABEL), PIXELATE_TYPE_SQUARE);
	obs_property_list_add_int(pixelate_types, obs_module_text(PIXELATE_TYPE_HEXAGONAL_LABEL),
				  PIXELATE_TYPE_HEXAGONAL);
	obs_property_list_add_int(pixelate_types, obs_module_text(PIXELATE_TYPE_CIRCLE_LABEL), PIXELATE_TYPE_CIRCLE);
	obs_property_list_add_int(pixelate_types, obs_module_text(PIXELATE_TYPE_TRIANGLE_LABEL),
				  PIXELATE_TYPE_TRIANGLE);
	obs_property_list_add_int(pixelate_types, obs_module_text(PIXELATE_TYPE_VORONOI_LABEL), PIXELATE_TYPE_VORONOI);
	obs_property_list_add_int(pixelate_types, obs_module_text(PIXELATE_TYPE_RHOMBOID_LABEL),
				  PIXELATE_TYPE_RHOMBOID);
	obs_property_list_add_int(pixelate_types, obs_module_text(PIXELATE_TYPE_TRIAKIS_LABEL), PIXELATE_TYPE_TRIAKIS);

	obs_property_set_modified_callback(pixelate_types, setting_pixelate_animate_modified);

	obs_property_t *p =
		obs_properties_add_float_slider(props, "radius", obs_module_text("BlurFilter.Radius"), 0.0, 80.1, 0.1);
	obs_property_float_set_suffix(p, "px");

	obs_properties_t *vector_blur = obs_properties_create();

	obs_property_t *vector_source = obs_properties_add_list(vector_blur, "vector_source",
								obs_module_text("BlurFilter.VectorBlur.Source"),
								OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(blur_add_source_to_list, vector_source);
	obs_enum_scenes(blur_add_source_to_list, vector_source);
	obs_property_list_insert_string(vector_source, 0, "", "");

	obs_property_t *vector_channel = obs_properties_add_list(vector_blur, "vector_blur_channel",
								 obs_module_text("BlurFilter.VectorBlur.Channel"),
								 OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(vector_channel, obs_module_text(GRADIENT_CHANNEL_RED_LABEL), GRADIENT_CHANNEL_RED);
	obs_property_list_add_int(vector_channel, obs_module_text(GRADIENT_CHANNEL_GREEN_LABEL),
				  GRADIENT_CHANNEL_GREEN);
	obs_property_list_add_int(vector_channel, obs_module_text(GRADIENT_CHANNEL_BLUE_LABEL), GRADIENT_CHANNEL_BLUE);
	obs_property_list_add_int(vector_channel, obs_module_text(GRADIENT_CHANNEL_ALPHA_LABEL),
				  GRADIENT_CHANNEL_ALPHA);
	obs_property_list_add_int(vector_channel, obs_module_text(GRADIENT_CHANNEL_LUMINANCE_LABEL),
				  GRADIENT_CHANNEL_LUMINANCE);
	obs_property_list_add_int(vector_channel, obs_module_text(GRADIENT_CHANNEL_SATURATION_LABEL),
				  GRADIENT_CHANNEL_SATURATION);

	obs_property_set_modified_callback2(vector_channel, vector_channel_modified, data);

	obs_property_t *vector_gradient_type = obs_properties_add_list(
		vector_blur, "vector_gradient_type", obs_module_text("BlurFilter.VectorBlur.GradientType"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(vector_gradient_type, obs_module_text(GRADIENT_TYPE_SOBEL_LABEL),
				  GRADIENT_TYPE_SOBEL);
	obs_property_list_add_int(vector_gradient_type, obs_module_text(GRADIENT_TYPE_CENTRAL_LIMIT_DIFF_LABEL),
				  GRADIENT_TYPE_CENTRAL_LIMIT_DIFF);
	obs_property_list_add_int(vector_gradient_type, obs_module_text(GRADIENT_TYPE_FORWARD_DIFF_LABEL),
				  GRADIENT_TYPE_FORWARD_DIFF);

	p = obs_properties_add_float_slider(vector_blur, "vector_blur_amount",
					    obs_module_text("BlurFilter.VectorBlur.Amount"), -100.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");

	obs_properties_add_float_slider(vector_blur, "vector_blur_smoothing",
					obs_module_text("BlurFilter.VectorBlur.Smoothing"), 0.0, 1000.0, 0.1);

	obs_properties_add_group(props, "vector_group", obs_module_text("BlurFilter.VectorBlur"), OBS_GROUP_NORMAL,
				 vector_blur);

	obs_properties_add_float_slider(props, "temporal_current_weight", obs_module_text("BlurFilter.Temporal.Amount"),
					0.0, 1.0, 0.01);

	obs_property_t *clear_threshold =
		obs_properties_add_float_slider(props, "temporal_clear_threshold",
						obs_module_text("BlurFilter.Temporal.ClearThreshold"), 0.0, 100.0, 0.1);

	obs_property_float_set_suffix(clear_threshold, "%");

	p = obs_properties_add_float_slider(props, "pixelate_smoothing_pct",
					    obs_module_text("BlurFilter.Pixelate.Smoothing"), 0.0, 100.0, 0.1);
	obs_property_float_set_suffix(p, "%");

	p = obs_properties_add_float_slider(props, "pixelate_origin_x", obs_module_text("BlurFilter.Pixelate.Origin_X"),
					    -6000.0, 6000.0, 0.1);
	obs_property_float_set_suffix(p, "px");

	p = obs_properties_add_float_slider(props, "pixelate_origin_y", obs_module_text("BlurFilter.Pixelate.Origin_Y"),
					    -6000.0, 6000.0, 0.1);
	obs_property_float_set_suffix(p, "px");

	p = obs_properties_add_float_slider(props, "pixelate_rotation", obs_module_text("BlurFilter.Pixelate.Rotation"),
					    -360.0, 360.0, 0.1);
	obs_property_float_set_suffix(p, "deg");

	obs_property_t *animate_p =
		obs_properties_add_bool(props, "pixelate_animate", obs_module_text("BlurFilter.Pixelate.Animate"));

	obs_property_set_modified_callback(animate_p, setting_pixelate_animate_modified);

	p = obs_properties_add_float(props, "pixelate_time", obs_module_text("BlurFilter.Pixelate.Time"), 0.0,
				     1000000.0, 0.01);
	obs_property_float_set_suffix(p, "s");

	obs_properties_add_float_slider(props, "pixelate_animation_speed",
					obs_module_text("BlurFilter.Pixelate.AnimationSpeed"), 0.0, 1000.0, 0.1);

	obs_properties_add_int_slider(props, "passes", obs_module_text("BlurFilter.Box.Passes"), 1, 5, 1);

	p = obs_properties_add_float_slider(props, "kawase_passes", obs_module_text("BlurFilter.DualKawase.Passes"),
					    0.0, 1025.0, 0.01);
	obs_property_float_set_suffix(p, "px");

	p = obs_properties_add_float_slider(props, "angle", obs_module_text("BlurFilter.Angle"), -360.0, 360.1, 0.1);
	obs_property_float_set_suffix(p, "deg");

	obs_properties_t *center_coords = obs_properties_create();

	p = obs_properties_add_float_slider(center_coords, "center_x", obs_module_text("BlurFilter.Center.X"), -3840.0,
					    7680.0, 1.0);
	obs_property_float_set_suffix(p, "px");

	p = obs_properties_add_float_slider(center_coords, "center_y", obs_module_text("BlurFilter.Center.Y"), -2160.0,
					    4320.0, 1.0);
	obs_property_float_set_suffix(p, "px");

	p = obs_properties_add_float_slider(center_coords, "inactive_radius",
					    obs_module_text("BlurFilter.Center.InactiveRadius"), 0.0, 2500.0, 1.0);
	obs_property_float_set_suffix(p, "px");

	obs_properties_add_group(props, "center_coordinate", obs_module_text("BlurFilter.CenterCoordinate"),
				 OBS_GROUP_NORMAL, center_coords);

	obs_properties_t *tilt_shift_bounds = obs_properties_create();
	obs_properties_add_float_slider(tilt_shift_bounds, "tilt_shift_center",
					obs_module_text("BlurFilter.TiltShift.Center"), 0.0, 1.0, 0.01);

	p = obs_properties_add_float_slider(tilt_shift_bounds, "tilt_shift_angle",
					    obs_module_text("BlurFilter.TiltShift.Angle"), -180.0, 181.0, 0.1);
	obs_property_float_set_suffix(p, "deg");

	obs_properties_add_float_slider(tilt_shift_bounds, "tilt_shift_width",
					obs_module_text("BlurFilter.TiltShift.Width"), 0.0, 1.01, 0.01);

	obs_properties_add_group(props, "tilt_shift_bounds", obs_module_text("BlurFilter.TiltShift"), OBS_GROUP_NORMAL,
				 tilt_shift_bounds);

	p = obs_properties_add_list(props, "background", obs_module_text("BlurFilter.Background"),
				    OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(blur_add_source_to_list, p);
	obs_enum_scenes(blur_add_source_to_list, p);
	obs_property_list_insert_string(p, 0, obs_module_text("BlurFilter.Background.None"), "");

	obs_property_t *effect_mask_list = obs_properties_add_list(props, "effect_mask",
								   obs_module_text("BlurFilter.EffectMask"),
								   OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(effect_mask_list, obs_module_text(EFFECT_MASK_TYPE_NONE_LABEL),
				  EFFECT_MASK_TYPE_NONE);
	obs_property_list_add_int(effect_mask_list, obs_module_text(EFFECT_MASK_TYPE_CROP_LABEL),
				  EFFECT_MASK_TYPE_CROP);
	obs_property_list_add_int(effect_mask_list, obs_module_text(EFFECT_MASK_TYPE_SOURCE_LABEL),
				  EFFECT_MASK_TYPE_SOURCE);
	obs_property_list_add_int(effect_mask_list, obs_module_text(EFFECT_MASK_TYPE_IMAGE_LABEL),
				  EFFECT_MASK_TYPE_IMAGE);
	obs_property_list_add_int(effect_mask_list, obs_module_text(EFFECT_MASK_TYPE_RECT_LABEL),
				  EFFECT_MASK_TYPE_RECT);
	obs_property_list_add_int(effect_mask_list, obs_module_text(EFFECT_MASK_TYPE_CIRCLE_LABEL),
				  EFFECT_MASK_TYPE_CIRCLE);

	obs_property_set_modified_callback(effect_mask_list, setting_effect_mask_modified);

	obs_properties_t *effect_mask_circle = obs_properties_create();

	obs_properties_add_float_slider(effect_mask_circle, "effect_mask_circle_center_x",
					obs_module_text("BlurFilter.EffectMask.Circle.CenterX"), -500.01, 600.01, 0.01);

	obs_properties_add_float_slider(effect_mask_circle, "effect_mask_circle_center_y",
					obs_module_text("BlurFilter.EffectMask.Circle.CenterY"), -500.01, 600.01, 0.01);

	obs_properties_add_float_slider(effect_mask_circle, "effect_mask_circle_radius",
					obs_module_text("BlurFilter.EffectMask.Circle.Radius"), 0.0, 500.01, 0.01);

	obs_properties_add_float_slider(effect_mask_circle, "effect_mask_circle_feathering",
					obs_module_text("BlurFilter.EffectMask.Circle.Feathering"), 0.0, 100.01, 0.01);

	obs_properties_add_bool(effect_mask_circle, "effect_mask_circle_invert",
				obs_module_text("BlurFilter.EffectMask.Invert"));

	obs_properties_add_group(props, "effect_mask_circle", obs_module_text("BlurFilter.EffectMask.CircleParameters"),
				 OBS_GROUP_NORMAL, effect_mask_circle);

	obs_properties_t *effect_mask_rect = obs_properties_create();

	obs_properties_add_float_slider(effect_mask_rect, "effect_mask_rect_center_x",
					obs_module_text("BlurFilter.EffectMask.Rect.CenterX"), -500.01, 600.01, 0.01);

	obs_properties_add_float_slider(effect_mask_rect, "effect_mask_rect_center_y",
					obs_module_text("BlurFilter.EffectMask.Rect.CenterY"), -500.01, 600.01, 0.01);

	obs_properties_add_float_slider(effect_mask_rect, "effect_mask_rect_width",
					obs_module_text("BlurFilter.EffectMask.Rect.Width"), 0.0, 500.01, 0.01);

	obs_properties_add_float_slider(effect_mask_rect, "effect_mask_rect_height",
					obs_module_text("BlurFilter.EffectMask.Rect.Height"), 0.0, 500.01, 0.01);

	obs_properties_add_float_slider(effect_mask_rect, "effect_mask_rect_corner_radius",
					obs_module_text("BlurFilter.EffectMask.Rect.CornerRadius"), 0.0, 100.01, 0.01);

	obs_properties_add_float_slider(effect_mask_rect, "effect_mask_rect_feathering",
					obs_module_text("BlurFilter.EffectMask.Rect.Feathering"), 0.0, 100.01, 0.01);

	obs_properties_add_bool(effect_mask_rect, "effect_mask_rect_invert",
				obs_module_text("BlurFilter.EffectMask.Invert"));

	obs_properties_add_group(props, "effect_mask_rect", obs_module_text("BlurFilter.EffectMask.RectParameters"),
				 OBS_GROUP_NORMAL, effect_mask_rect);

	obs_properties_t *effect_mask_source = obs_properties_create();

	obs_properties_add_path(effect_mask_source, "effect_mask_source_file",
				obs_module_text("BlurFilter.EffectMask.Source.File"), OBS_PATH_FILE,
				"Textures (*.bmp *.tga *.png *.jpeg *.jpg *.gif);;", NULL);

	obs_property_t *effect_mask_source_source = obs_properties_add_list(
		effect_mask_source, "effect_mask_source_source", obs_module_text("BlurFilter.EffectMask.Source.Source"),
		OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(blur_add_source_to_list, effect_mask_source_source);
	obs_enum_scenes(blur_add_source_to_list, effect_mask_source_source);
	obs_property_list_insert_string(effect_mask_source_source, 0,
					obs_module_text("BlurFilter.EffectMask.Source.None"), "");

	obs_property_t *effect_mask_source_filter_list = obs_properties_add_list(
		effect_mask_source, "effect_mask_source_filter_list",
		obs_module_text("BlurFilter.EffectMask.Source.Filter"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);

	obs_property_list_add_int(effect_mask_source_filter_list,
				  obs_module_text(EFFECT_MASK_SOURCE_FILTER_ALPHA_LABEL),
				  EFFECT_MASK_SOURCE_FILTER_ALPHA);
	obs_property_list_add_int(effect_mask_source_filter_list,
				  obs_module_text(EFFECT_MASK_SOURCE_FILTER_GRAYSCALE_LABEL),
				  EFFECT_MASK_SOURCE_FILTER_GRAYSCALE);
	obs_property_list_add_int(effect_mask_source_filter_list,
				  obs_module_text(EFFECT_MASK_SOURCE_FILTER_LUMINOSITY_LABEL),
				  EFFECT_MASK_SOURCE_FILTER_LUMINOSITY);
	obs_property_list_add_int(effect_mask_source_filter_list,
				  obs_module_text(EFFECT_MASK_SOURCE_FILTER_SLIDERS_LABEL),
				  EFFECT_MASK_SOURCE_FILTER_SLIDERS);

	obs_property_set_modified_callback(effect_mask_source_filter_list, setting_effect_mask_source_filter_modified);

	obs_properties_add_float_slider(effect_mask_source, "effect_mask_source_filter_red",
					obs_module_text("BlurFilter.Channel.Red"), -100.01, 100.01, 0.01);

	obs_properties_add_float_slider(effect_mask_source, "effect_mask_source_filter_green",
					obs_module_text("BlurFilter.Channel.Green"), -100.01, 100.01, 0.01);

	obs_properties_add_float_slider(effect_mask_source, "effect_mask_source_filter_blue",
					obs_module_text("BlurFilter.Channel.Blue"), -100.01, 100.01, 0.01);

	obs_properties_add_float_slider(effect_mask_source, "effect_mask_source_filter_alpha",
					obs_module_text("BlurFilter.Channel.Alpha"), -100.01, 100.01, 0.01);

	obs_properties_add_float_slider(effect_mask_source, "effect_mask_source_filter_multiplier",
					obs_module_text("BlurFilter.EffectMask.Source.Multiplier"), -100.01, 100.01,
					0.01);

	obs_properties_add_bool(effect_mask_source, "effect_mask_source_invert",
				obs_module_text("BlurFilter.EffectMask.Invert"));

	obs_properties_add_group(props, "effect_mask_source", obs_module_text("BlurFilter.EffectMask.SourceParameters"),
				 OBS_GROUP_NORMAL, effect_mask_source);

	obs_properties_t *effect_mask_crop = obs_properties_create();

	obs_properties_add_float_slider(effect_mask_crop, "effect_mask_crop_top",
					obs_module_text("BlurFilter.EffectMask.Crop.Top"), 0.0, 100.01, 0.01);

	obs_properties_add_float_slider(effect_mask_crop, "effect_mask_crop_bottom",
					obs_module_text("BlurFilter.EffectMask.Crop.Bottom"), 0.0, 100.01, 0.01);

	obs_properties_add_float_slider(effect_mask_crop, "effect_mask_crop_left",
					obs_module_text("BlurFilter.EffectMask.Crop.Left"), 0.0, 100.01, 0.01);

	obs_properties_add_float_slider(effect_mask_crop, "effect_mask_crop_right",
					obs_module_text("BlurFilter.EffectMask.Crop.Right"), 0.0, 100.01, 0.01);

	obs_properties_add_float_slider(effect_mask_crop, "effect_mask_crop_corner_radius",
					obs_module_text("BlurFilter.EffectMask.CornerRadius"), 0.0, 50.01, 0.01);

	obs_properties_add_float_slider(effect_mask_crop, "effect_mask_crop_feathering",
					obs_module_text("BlurFilter.EffectMask.Feathering"), 0.0, 100.01, 0.01);

	obs_properties_add_bool(effect_mask_crop, "effect_mask_crop_invert",
				obs_module_text("BlurFilter.EffectMask.Invert"));

	obs_properties_add_group(props, "effect_mask_crop", obs_module_text("BlurFilter.EffectMask.CropParameters"),
				 OBS_GROUP_NORMAL, effect_mask_crop);

	return props;
}

static bool setting_effect_mask_source_filter_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	int source_filter_type = (int)obs_data_get_int(settings, "effect_mask_source_filter_list");
	switch (source_filter_type) {
	case EFFECT_MASK_SOURCE_FILTER_SLIDERS:
		blur_setting_visibility("effect_mask_source_filter_red", true, props);
		blur_setting_visibility("effect_mask_source_filter_green", true, props);
		blur_setting_visibility("effect_mask_source_filter_blue", true, props);
		blur_setting_visibility("effect_mask_source_filter_alpha", true, props);
		break;
	default:
		blur_setting_visibility("effect_mask_source_filter_red", false, props);
		blur_setting_visibility("effect_mask_source_filter_green", false, props);
		blur_setting_visibility("effect_mask_source_filter_blue", false, props);
		blur_setting_visibility("effect_mask_source_filter_alpha", false, props);
		break;
	}
	return true;
}

static bool setting_pixelate_animate_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	int pixelate_type = (int)obs_data_get_int(settings, "pixelate_type");
	if (pixelate_type == PIXELATE_TYPE_VORONOI) {
		bool animate = obs_data_get_bool(settings, "pixelate_animate");
		blur_setting_visibility("pixelate_time", !animate, props);
		blur_setting_visibility("pixelate_animation_speed", animate, props);
	} else {
		blur_setting_visibility("pixelate_animate", false, props);
		blur_setting_visibility("pixelate_time", false, props);
		blur_setting_visibility("pixelate_animation_speed", false, props);
	}

	return true;
}

static bool setting_effect_mask_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	int mask_type = (int)obs_data_get_int(settings, "effect_mask");
	switch (mask_type) {
	case EFFECT_MASK_TYPE_NONE:
		blur_setting_visibility("effect_mask_crop", false, props);
		blur_setting_visibility("effect_mask_source", false, props);
		blur_setting_visibility("effect_mask_circle", false, props);
		blur_setting_visibility("effect_mask_rect", false, props);
		break;
	case EFFECT_MASK_TYPE_CROP:
		blur_setting_visibility("effect_mask_crop", true, props);
		blur_setting_visibility("effect_mask_source", false, props);
		blur_setting_visibility("effect_mask_circle", false, props);
		blur_setting_visibility("effect_mask_rect", false, props);
		break;
	case EFFECT_MASK_TYPE_CIRCLE:
		blur_setting_visibility("effect_mask_crop", false, props);
		blur_setting_visibility("effect_mask_source", false, props);
		blur_setting_visibility("effect_mask_circle", true, props);
		blur_setting_visibility("effect_mask_rect", false, props);
		break;
	case EFFECT_MASK_TYPE_RECT:
		blur_setting_visibility("effect_mask_crop", false, props);
		blur_setting_visibility("effect_mask_source", false, props);
		blur_setting_visibility("effect_mask_circle", false, props);
		blur_setting_visibility("effect_mask_rect", true, props);
		break;
	case EFFECT_MASK_TYPE_SOURCE:
		blur_setting_visibility("effect_mask_crop", false, props);
		blur_setting_visibility("effect_mask_source", true, props);
		blur_setting_visibility("effect_mask_circle", false, props);
		blur_setting_visibility("effect_mask_rect", false, props);
		blur_setting_visibility("effect_mask_source_file", false, props);
		blur_setting_visibility("effect_mask_source_source", true, props);
		{
			obs_property_t *prop = obs_properties_get(props, "effect_mask_source");
			obs_property_set_description(prop, obs_module_text("BlurFilter.EffectMask.SourceParameters"));
		}
		break;
	case EFFECT_MASK_TYPE_IMAGE:
		blur_setting_visibility("effect_mask_crop", false, props);
		blur_setting_visibility("effect_mask_source", true, props);
		blur_setting_visibility("effect_mask_circle", false, props);
		blur_setting_visibility("effect_mask_rect", false, props);
		blur_setting_visibility("effect_mask_source_file", true, props);
		blur_setting_visibility("effect_mask_source_source", false, props);
		{
			obs_property_t *prop = obs_properties_get(props, "effect_mask_source");
			obs_property_set_description(prop, obs_module_text("BlurFilter.EffectMask.ImageParameters"));
		}
		break;
	}
	return true;
}

static bool setting_blur_algorithm_modified(void *data, obs_properties_t *props, obs_property_t *p,
					    obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	UNUSED_PARAMETER(data);
	int blur_algorithm = (int)obs_data_get_int(settings, "blur_algorithm");
	switch (blur_algorithm) {
	case ALGO_GAUSSIAN:
		blur_setting_visibility("radius", true, props);
		blur_setting_visibility("temporal_current_weight", false, props);
		blur_setting_visibility("temporal_clear_threshold", false, props);
		blur_setting_visibility("passes", false, props);
		blur_setting_visibility("kawase_passes", false, props);
		blur_setting_visibility("blur_type", true, props);
		blur_setting_visibility("pixelate_type", false, props);
		blur_setting_visibility("pixelate_smoothing_pct", false, props);
		blur_setting_visibility("pixelate_rotation", false, props);
		blur_setting_visibility("pixelate_origin_x", false, props);
		blur_setting_visibility("pixelate_origin_y", false, props);
		blur_setting_visibility("pixelate_animate", false, props);
		blur_setting_visibility("pixelate_time", false, props);
		blur_setting_visibility("pixelate_animation_speed", false, props);

		set_blur_radius_settings(obs_module_text("BlurFilter.Radius"), 0.0f, 80.01f, 0.1f, props);
		set_gaussian_blur_types(props);
		break;
	case ALGO_BOX:
		blur_setting_visibility("radius", true, props);
		blur_setting_visibility("temporal_current_weight", false, props);
		blur_setting_visibility("temporal_clear_threshold", false, props);
		blur_setting_visibility("kawase_passes", false, props);
		blur_setting_visibility("passes", true, props);
		blur_setting_visibility("blur_type", true, props);
		blur_setting_visibility("pixelate_type", false, props);
		blur_setting_visibility("pixelate_smoothing_pct", false, props);
		blur_setting_visibility("pixelate_rotation", false, props);
		blur_setting_visibility("pixelate_origin_x", false, props);
		blur_setting_visibility("pixelate_origin_y", false, props);
		blur_setting_visibility("pixelate_animate", false, props);
		blur_setting_visibility("pixelate_time", false, props);
		blur_setting_visibility("pixelate_animation_speed", false, props);

		set_blur_radius_settings(obs_module_text("BlurFilter.Radius"), 0.0f, 100.01f, 0.1f, props);
		set_box_blur_types(props);
		break;
	case ALGO_DUAL_KAWASE:
		blur_setting_visibility("radius", false, props);
		blur_setting_visibility("temporal_current_weight", false, props);
		blur_setting_visibility("temporal_clear_threshold", false, props);
		blur_setting_visibility("passes", false, props);
		blur_setting_visibility("kawase_passes", true, props);
		blur_setting_visibility("blur_type", false, props);
		blur_setting_visibility("pixelate_type", false, props);
		blur_setting_visibility("pixelate_smoothing_pct", false, props);
		blur_setting_visibility("pixelate_rotation", false, props);
		blur_setting_visibility("pixelate_origin_x", false, props);
		blur_setting_visibility("pixelate_origin_y", false, props);
		blur_setting_visibility("pixelate_animate", false, props);
		blur_setting_visibility("pixelate_time", false, props);
		blur_setting_visibility("pixelate_animation_speed", false, props);
		set_dual_kawase_blur_types(props);
		obs_data_set_int(settings, "blur_type", TYPE_AREA);
		settings_blur_area(props, settings);
		break;
	case ALGO_PIXELATE:
		blur_setting_visibility("radius", true, props);
		blur_setting_visibility("temporal_current_weight", false, props);
		blur_setting_visibility("temporal_clear_threshold", false, props);
		blur_setting_visibility("passes", false, props);
		blur_setting_visibility("kawase_passes", false, props);
		blur_setting_visibility("blur_type", false, props);
		blur_setting_visibility("pixelate_type", true, props);
		blur_setting_visibility("pixelate_smoothing_pct", true, props);
		blur_setting_visibility("pixelate_rotation", true, props);
		blur_setting_visibility("pixelate_origin_x", true, props);
		blur_setting_visibility("pixelate_origin_y", true, props);
		blur_setting_visibility("pixelate_animate", true, props);
		blur_setting_visibility("pixelate_time", true, props);
		blur_setting_visibility("pixelate_animation_speed", true, props);
		set_blur_radius_settings(obs_module_text("BlurFilter.Pixelate.PixelSize"), 1.0f, 1024.01f, 0.1f, props);
		set_pixelate_blur_types(props);
		obs_data_set_int(settings, "blur_type", TYPE_AREA);
		settings_blur_area(props, settings);
		setting_pixelate_animate_modified(props, p, settings);
		break;
	case ALGO_TEMPORAL:
		blur_setting_visibility("radius", false, props);
		blur_setting_visibility("temporal_current_weight", true, props);
		blur_setting_visibility("temporal_clear_threshold", true, props);
		blur_setting_visibility("kawase_passes", false, props);
		blur_setting_visibility("passes", false, props);
		blur_setting_visibility("blur_type", false, props);
		blur_setting_visibility("pixelate_type", false, props);
		blur_setting_visibility("pixelate_smoothing_pct", false, props);
		blur_setting_visibility("pixelate_rotation", false, props);
		blur_setting_visibility("pixelate_origin_x", false, props);
		blur_setting_visibility("pixelate_origin_y", false, props);
		blur_setting_visibility("pixelate_animate", false, props);
		blur_setting_visibility("pixelate_time", false, props);
		blur_setting_visibility("pixelate_animation_speed", false, props);
		break;
	}
	return true;
}

static bool setting_blur_types_modified(void *data, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	//UNUSED_PARAMETER(data);
	blur_filter_data_t *filter = data;
	int blur_type = (int)obs_data_get_int(settings, "blur_type");
	if (blur_type == TYPE_AREA) {
		return settings_blur_area(props, settings);
	} else if (blur_type == TYPE_DIRECTIONAL) {
		return settings_blur_directional(props);
	} else if (blur_type == TYPE_ZOOM) {
		return settings_blur_zoom(props);
	} else if (blur_type == TYPE_MOTION) {
		return settings_blur_directional(props);
	} else if (blur_type == TYPE_TILTSHIFT) {
		return settings_blur_tilt_shift(props);
	} else if (blur_type == TYPE_VECTOR) {
		return settings_blur_vector(props, filter);
	}
	return true;
}

static void blur_setting_visibility(const char *prop_name, bool visible, obs_properties_t *props)
{
	obs_property_t *p = obs_properties_get(props, prop_name);
	obs_property_set_enabled(p, visible);
	obs_property_set_visible(p, visible);
}

static void set_blur_radius_settings(const char *name, float min_val, float max_val, float step_size,
				     obs_properties_t *props)
{
	obs_property_t *p = obs_properties_get(props, "radius");
	obs_property_set_description(p, name);
	obs_property_float_set_limits(p, (double)min_val, (double)max_val, (double)step_size);
}

static bool settings_blur_area(obs_properties_t *props, obs_data_t *settings)
{
	int algorithm = (int)obs_data_get_int(settings, "blur_algorithm");
	blur_setting_visibility("radius", algorithm != ALGO_DUAL_KAWASE, props);
	blur_setting_visibility("angle", false, props);
	blur_setting_visibility("center_coordinate", false, props);
	blur_setting_visibility("background", true, props);
	blur_setting_visibility("tilt_shift_bounds", false, props);
	blur_setting_visibility("vector_group", false, props);
	return true;
}

static bool settings_blur_directional(obs_properties_t *props)
{
	blur_setting_visibility("radius", true, props);
	blur_setting_visibility("angle", true, props);
	blur_setting_visibility("center_coordinate", false, props);
	blur_setting_visibility("background", true, props);
	blur_setting_visibility("tilt_shift_bounds", false, props);
	blur_setting_visibility("vector_group", false, props);
	return true;
}

static bool settings_blur_zoom(obs_properties_t *props)
{
	blur_setting_visibility("radius", true, props);
	blur_setting_visibility("angle", false, props);
	blur_setting_visibility("center_coordinate", true, props);
	blur_setting_visibility("background", true, props);
	blur_setting_visibility("tilt_shift_bounds", false, props);
	blur_setting_visibility("vector_group", false, props);
	return true;
}

static bool settings_blur_tilt_shift(obs_properties_t *props)
{
	blur_setting_visibility("radius", true, props);
	blur_setting_visibility("angle", false, props);
	blur_setting_visibility("center_coordinate", false, props);
	blur_setting_visibility("background", true, props);
	blur_setting_visibility("tilt_shift_bounds", true, props);
	blur_setting_visibility("vector_group", false, props);
	return true;
}

static bool settings_blur_vector(obs_properties_t *props, blur_filter_data_t *filter)
{
	blur_setting_visibility("radius", false, props);
	blur_setting_visibility("angle", false, props);
	blur_setting_visibility("center_coordinate", false, props);
	blur_setting_visibility("background", true, props);
	blur_setting_visibility("tilt_shift_bounds", false, props);
	blur_setting_visibility("vector_group", true, props);
	UNUSED_PARAMETER(filter);
	return true;
}

static bool blur_filter_enable_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	blur_filter_data_t *filter = data;
	if (!pressed)
		return false;

	if (obs_source_enabled(filter->context))
		return false;
	obs_source_set_enabled(filter->context, true);

	return true;
}

static bool blur_filter_disable_hotkey(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	blur_filter_data_t *filter = data;
	if (!pressed)
		return false;
	if (!obs_source_enabled(filter->context))
		return false;
	obs_source_set_enabled(filter->context, false);

	return true;
}

/* ------------------------------------------------------------------------
 * Tick
 * --------------------------------------------------------------------- */

struct center_init_task_data {
	obs_weak_source_t *source;
	double x;
	double y;
};

// Runs on the UI thread: writes the initial pixelate origin / zoom center to
// the filter settings so the properties UI shows real values. Settings must
// not be modified from the graphics thread.
static void center_init_task(void *param)
{
	struct center_init_task_data *task = param;
	obs_source_t *source = obs_weak_source_get_source(task->source);
	if (source) {
		obs_data_t *settings = obs_source_get_settings(source);
		if (obs_data_get_double(settings, "pixelate_origin_x") < CENTER_UNINITIALIZED_THRESHOLD) {
			obs_data_t *values = obs_data_create();
			obs_data_set_double(values, "pixelate_origin_x", task->x);
			obs_data_set_double(values, "pixelate_origin_y", task->y);
			obs_data_set_double(values, "center_x", task->x);
			obs_data_set_double(values, "center_y", task->y);
			obs_source_update(source, values);
			obs_data_release(values);
		}
		obs_data_release(settings);
		obs_source_release(source);
	}
	obs_weak_source_release(task->source);
	bfree(task);
}

static void register_hotkeys(blur_filter_data_t *filter)
{
	obs_source_t *parent = obs_filter_get_parent(filter->context);
	if (!parent)
		return;

	const char *filter_name = obs_source_get_name(filter->context);
	struct dstr enable;
	struct dstr disable;

	dstr_init_copy(&enable, obs_module_text("BlurFilter.Enable"));
	dstr_init_copy(&disable, obs_module_text("BlurFilter.Disable"));
	dstr_cat(&enable, " ");
	dstr_cat(&enable, filter_name);
	dstr_cat(&disable, " ");
	dstr_cat(&disable, filter_name);

	filter->hotkey = obs_hotkey_pair_register_source(parent, "BlurFilter.Enable", enable.array,
							 "BlurFilter.Disable", disable.array, blur_filter_enable_hotkey,
							 blur_filter_disable_hotkey, filter, filter);
	dstr_free(&enable);
	dstr_free(&disable);
}

static void blur_filter_video_tick(void *data, float seconds)
{
	blur_filter_data_t *filter = data;
	if (filter->pixelate_animate) {
		filter->time += seconds * filter->pixelate_animation_speed;
	} else {
		filter->time = filter->pixelate_animation_time;
	}
	obs_source_t *target = obs_filter_get_target(filter->context);
	if (!target) {
		return;
	}

	// If nothing was drawn last frame (source hidden, filter disabled,
	// scene not visible) the temporal history is stale; start fresh
	// rather than ghosting an old frame in when it reappears.
	if (!filter->rendered) {
		filter->temporal_prior_stored = false;
	}

	// Named sources may not exist yet when settings load, or may be
	// deleted/recreated later. Retry periodically rather than every frame.
	filter->source_retry_timer -= seconds;
	if (filter->source_retry_timer <= 0.0f) {
		filter->source_retry_timer = SOURCE_RETRY_INTERVAL;
		external_source_refresh(filter, &filter->mask_source);
		external_source_refresh(filter, &filter->background);
		external_source_refresh(filter, &filter->vector_source);
	}

	if (filter->hotkey == OBS_INVALID_HOTKEY_PAIR_ID) {
		register_hotkeys(filter);
	}

	const uint32_t width = (uint32_t)obs_source_get_base_width(target);
	const uint32_t height = (uint32_t)obs_source_get_base_height(target);
	if (filter->width != width || filter->height != height) {
		filter->width = width;
		filter->height = height;
		filter->uv_size.x = (float)width;
		filter->uv_size.y = (float)height;
		filter->temporal_prior_stored = false;
	}

	if (filter->center_uninitialized && width > 0) {
		// Default the pixelate origin and zoom center to the source
		// center until real values have been written to settings.
		filter->center_x = (float)width / 2.0f;
		filter->center_y = (float)height / 2.0f;
		filter->pixelate_tessel_center.x = (float)width / 2.0f;
		filter->pixelate_tessel_center.y = (float)height / 2.0f;

		if (!filter->center_init_queued) {
			filter->center_init_queued = true;
			struct center_init_task_data *task = bzalloc(sizeof(*task));
			task->source = obs_source_get_weak_source(filter->context);
			task->x = (double)width / 2.0;
			task->y = (double)height / 2.0;
			obs_queue_task(OBS_TASK_UI, center_init_task, task, false);
		}
	}

	filter->rendered = false;
}

/* ------------------------------------------------------------------------
 * Effect loading
 * --------------------------------------------------------------------- */

// Clears every parameter handle that belongs to a blur algorithm effect, so
// no handle into a destroyed effect survives an algorithm/type switch.
static void clear_blur_params(blur_filter_data_t *filter)
{
	gs_eparam_t **params[] = {
		&filter->param_uv_size,
		&filter->param_radius,
		&filter->param_texel_step,
		&filter->param_kernel_size,
		&filter->param_offset,
		&filter->param_weight,
		&filter->param_kernel_texture,
		&filter->param_gradient_image,
		&filter->param_gradient_channel,
		&filter->param_gradient_uv_size,
		&filter->param_gradient_map,
		&filter->param_inactive_radius,
		&filter->param_pixel_size,
		&filter->param_pixel_center,
		&filter->param_pixel_rot,
		&filter->param_pixel_cos_theta,
		&filter->param_pixel_sin_theta,
		&filter->param_pixel_cos_rtheta,
		&filter->param_pixel_sin_rtheta,
		&filter->param_pixel_time,
		&filter->param_radial_center,
		&filter->param_focus_width,
		&filter->param_focus_center,
		&filter->param_focus_angle,
		&filter->param_temporal_image,
		&filter->param_temporal_prior_image,
		&filter->param_temporal_current_weight,
		&filter->param_temporal_clear_threshold,
	};
	for (size_t i = 0; i < OBS_COUNTOF(params); i++)
		*params[i] = NULL;
}

// Must be called with the graphics context entered.
static void blur_filter_reload_effect(blur_filter_data_t *filter)
{
	filter->video_render = NULL;
	filter->load_effect = NULL;
	filter->update = NULL;
	clear_blur_params(filter);
	filter->kernel_radius_last = -1.0f;
	filter->temporal_prior_stored = false;

	switch (filter->blur_algorithm) {
	case ALGO_GAUSSIAN:
		gaussian_setup_callbacks(filter);
		break;
	case ALGO_BOX:
		box_setup_callbacks(filter);
		break;
	case ALGO_PIXELATE:
		pixelate_setup_callbacks(filter);
		break;
	case ALGO_DUAL_KAWASE:
		dual_kawase_setup_callbacks(filter);
		break;
	case ALGO_TEMPORAL:
		temporal_setup_callbacks(filter);
		break;
	}

	if (filter->load_effect) {
		filter->load_effect(filter);
	}
}

// Effects that don't depend on any setting; loaded once at creation.
static void load_static_effects(blur_filter_data_t *filter)
{
	const struct effect_param_binding composite_params[] = {
		EFFECT_PARAM("image", filter->param_composite_image),
		EFFECT_PARAM("background", filter->param_background),
	};
	LOAD_EFFECT(&filter->composite_effect, "/shaders/blur/composite.effect", composite_params);

	load_effect_with_params(&filter->mix_effect, "/shaders/blur/mix.effect", NULL, 0, NULL, NULL);

	const struct effect_param_binding output_params[] = {
		EFFECT_PARAM("output_image", filter->param_output_image),
	};
	LOAD_EFFECT(&filter->output_effect, "/shaders/blur/render_output.effect", output_params);
}

// Must be called with the graphics context entered.
static void effect_mask_load_effect(blur_filter_data_t *filter)
{
	// Every mask parameter slot is (re)bound for whichever effect is
	// loaded; slots the effect doesn't have become NULL.
	const struct effect_param_binding mask_params[] = {
		EFFECT_PARAM("image", filter->param_mask_image),
		EFFECT_PARAM("filtered_image", filter->param_filtered_image),
		// Crop / rectangle
		EFFECT_PARAM("scale", filter->param_mask_crop_scale),
		EFFECT_PARAM("offset", filter->param_mask_crop_offset),
		EFFECT_PARAM("box_aspect_ratio", filter->param_mask_crop_box_aspect_ratio),
		EFFECT_PARAM("corner_radius", filter->param_mask_crop_corner_radius),
		EFFECT_PARAM("feathering", filter->param_mask_crop_feathering),
		EFFECT_PARAM("inv", filter->param_mask_crop_invert),
		// Source / image
		EFFECT_PARAM("alpha_source", filter->param_mask_source_alpha_source),
		EFFECT_PARAM("rgba_weights", filter->param_mask_source_rgba_weights),
		EFFECT_PARAM("multiplier", filter->param_mask_source_multiplier),
		EFFECT_PARAM("inv", filter->param_mask_source_invert),
		// Circle
		EFFECT_PARAM("center", filter->param_mask_circle_center),
		EFFECT_PARAM("circle_radius", filter->param_mask_circle_radius),
		EFFECT_PARAM("feathering", filter->param_mask_circle_feathering),
		EFFECT_PARAM("inv", filter->param_mask_circle_inv),
		EFFECT_PARAM("uv_scale", filter->param_mask_circle_uv_scale),
	};

	const char *path = NULL;
	switch (filter->mask_type) {
	case EFFECT_MASK_TYPE_CROP:
	case EFFECT_MASK_TYPE_RECT:
		path = "/shaders/blur/effect_mask_crop.effect";
		break;
	case EFFECT_MASK_TYPE_SOURCE:
	case EFFECT_MASK_TYPE_IMAGE:
		path = "/shaders/blur/effect_mask_source.effect";
		break;
	case EFFECT_MASK_TYPE_CIRCLE:
		path = "/shaders/blur/effect_mask_circle.effect";
		break;
	}

	if (path) {
		LOAD_EFFECT(&filter->effect_mask_effect, path, mask_params);
	} else {
		if (filter->effect_mask_effect) {
			gs_effect_destroy(filter->effect_mask_effect);
			filter->effect_mask_effect = NULL;
		}
		for (size_t i = 0; i < OBS_COUNTOF(mask_params); i++)
			*mask_params[i].param = NULL;
	}
}
