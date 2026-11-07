#include "gaussian.h"
#include "dual_kawase.h"

static void gaussian_area_blur(blur_filter_data_t *data);
static void gaussian_directional_blur(blur_filter_data_t *data);
static void gaussian_zoom_blur(blur_filter_data_t *data);
static void gaussian_motion_blur(blur_filter_data_t *data);
static void gaussian_vector_blur(blur_filter_data_t *data);
static bool gaussian_vector_gradient(blur_filter_data_t *data);
static bool gaussian_vector_smooth_gradient(blur_filter_data_t *data);
static void gaussian_vector_apply_blur(blur_filter_data_t *data);

static void load_1d_gaussian_effect(blur_filter_data_t *filter);
static void load_motion_gaussian_effect(blur_filter_data_t *filter);
static void load_radial_gaussian_effect(blur_filter_data_t *filter);
static void load_vector_gaussian_effect(blur_filter_data_t *filter);
static void load_gradient_effect(blur_filter_data_t *filter);
static void sample_kernel(float radius, blur_filter_data_t *filter);

void set_gaussian_blur_types(obs_properties_t *props)
{
	obs_property_t *p = obs_properties_get(props, "blur_type");
	obs_property_list_clear(p);
	obs_property_list_add_int(p, obs_module_text(TYPE_AREA_LABEL), TYPE_AREA);
	obs_property_list_add_int(p, obs_module_text(TYPE_DIRECTIONAL_LABEL), TYPE_DIRECTIONAL);
	obs_property_list_add_int(p, obs_module_text(TYPE_ZOOM_LABEL), TYPE_ZOOM);
	obs_property_list_add_int(p, obs_module_text(TYPE_MOTION_LABEL), TYPE_MOTION);
	obs_property_list_add_int(p, obs_module_text(TYPE_VECTOR_LABEL), TYPE_VECTOR);
}

void gaussian_setup_callbacks(blur_filter_data_t *data)
{
	data->video_render = render_video_gaussian;
	data->load_effect = load_effect_gaussian;
	data->update = update_gaussian;
}

void update_gaussian(blur_filter_data_t *data)
{
	// One kernel is shared by every gaussian blur type, so sample it from
	// whichever setting the active type uses. kernel_radius_last is reset
	// whenever the effect is reloaded (including blur type changes).
	const float radius = data->blur_type == TYPE_VECTOR ? fabsf(data->vector_blur_amount) : data->radius;
	if (radius != data->kernel_radius_last) {
		data->kernel_radius_last = radius;
		sample_kernel(radius, data);
	}
}

void render_video_gaussian(blur_filter_data_t *data)
{
	switch (data->blur_type) {
	case TYPE_AREA:
		gaussian_area_blur(data);
		break;
	case TYPE_DIRECTIONAL:
		gaussian_directional_blur(data);
		break;
	case TYPE_ZOOM:
		gaussian_zoom_blur(data);
		break;
	case TYPE_MOTION:
		gaussian_motion_blur(data);
		break;
	case TYPE_VECTOR:
		gaussian_vector_blur(data);
		break;
	}
}

void load_effect_gaussian(blur_filter_data_t *filter)
{
	switch (filter->blur_type) {
	case TYPE_AREA:
		load_1d_gaussian_effect(filter);
		break;
	case TYPE_DIRECTIONAL:
		load_1d_gaussian_effect(filter);
		break;
	case TYPE_ZOOM:
		load_radial_gaussian_effect(filter);
		break;
	case TYPE_MOTION:
		load_motion_gaussian_effect(filter);
		break;
	case TYPE_VECTOR:
		load_vector_gaussian_effect(filter);
		load_gradient_effect(filter);
		blur_load_effect_dual_kawase(filter);
		break;
	}
}

bool vector_channel_modified(void *data, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	int channel = (int)obs_data_get_int(settings, "vector_blur_channel");
	blur_filter_data_t *filter = data;
	filter->vector_blur_channel = channel;
	if (filter->blur_algorithm == ALGO_GAUSSIAN && filter->blur_type == TYPE_VECTOR) {
		load_gradient_effect(filter);
	}
	return false;
}

/*
 *  Performs an area blur using the gaussian kernel. Blur is
 *  equal in both x and y directions.
 */
static void gaussian_area_blur(blur_filter_data_t *data)
{
	gs_effect_t *effect = data->effect;
	gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);

	if (!effect || !texture) {
		return;
	}

	if (data->radius < MIN_GAUSSIAN_BLUR_RADIUS) {
		data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);
		blur_texrender_set_texture(texture, data->output_texrender);
		return;
	}

	texture = blend_composite(texture, data);

	data->render2 = blur_create_or_reset_texrender(data->render2, data->color_format);

	// 1. First pass- apply 1D blur kernel to horizontal dir.
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	switch (data->device_type) {
	case GS_DEVICE_DIRECT3D_11:
		if (data->param_weight) {
			gs_effect_set_val(data->param_weight, data->kernel.array, data->kernel.num * sizeof(float));
		}
		if (data->param_offset) {
			gs_effect_set_val(data->param_offset, data->offset.array, data->offset.num * sizeof(float));
		}
		break;
	case GS_DEVICE_OPENGL:
		if (data->param_kernel_texture) {
			gs_effect_set_texture(data->param_kernel_texture, data->kernel_texture);
		}
	}

	const int k_size = (int)data->kernel_size;
	if (data->param_kernel_size) {
		gs_effect_set_int(data->param_kernel_size, k_size);
	}

	struct vec2 texel_step;
	texel_step.x = 1.0f / data->width;
	texel_step.y = 0.0f;
	if (data->param_texel_step) {
		gs_effect_set_vec2(data->param_texel_step, &texel_step);
	}

	blur_set_blending_parameters();

	if (gs_texrender_begin(data->render2, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture, 0, data->width, data->height);
		gs_texrender_end(data->render2);
	}

	// 2. Save texture from first pass in variable "texture"
	texture = gs_texrender_get_texture(data->render2);

	// 3. Second Pass- Apply 1D blur kernel vertically.
	image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	if (data->device_type == GS_DEVICE_OPENGL && data->param_kernel_texture) {
		gs_effect_set_texture(data->param_kernel_texture, data->kernel_texture);
	}

	texel_step.x = 0.0f;
	texel_step.y = 1.0f / data->height;
	if (data->param_texel_step) {
		gs_effect_set_vec2(data->param_texel_step, &texel_step);
	}

	data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);

	if (gs_texrender_begin(data->output_texrender, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture, 0, data->width, data->height);
		gs_texrender_end(data->output_texrender);
	}

	gs_blend_state_pop();
}

/*
 *  Performs a directional blur using the gaussian kernel.
 */
static void gaussian_directional_blur(blur_filter_data_t *data)
{
	gs_effect_t *effect = data->effect;
	gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);

	if (!effect || !texture) {
		return;
	}

	if (data->radius < MIN_GAUSSIAN_BLUR_RADIUS) {
		data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);
		blur_texrender_set_texture(texture, data->output_texrender);
		return;
	}

	texture = blend_composite(texture, data);
	//gs_texture_t *background_texture = NULL;
	//if (data->background) {
	//	get_background(data);
	//	background_texture =
	//		gs_texrender_get_texture(data->background_texrender);
	//	gs_eparam_t *background_img =
	//		gs_effect_get_param_by_name(effect, "background");
	//	gs_effect_set_texture(background_img, background_texture);
	//}

	// 1. Single pass- blur only in one direction
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	switch (data->device_type) {
	case GS_DEVICE_DIRECT3D_11:
		if (data->param_weight) {
			gs_effect_set_val(data->param_weight, data->kernel.array, data->kernel.num * sizeof(float));
		}
		if (data->param_offset) {
			gs_effect_set_val(data->param_offset, data->offset.array, data->offset.num * sizeof(float));
		}
		break;
	case GS_DEVICE_OPENGL:
		if (data->param_kernel_texture) {
			gs_effect_set_texture(data->param_kernel_texture, data->kernel_texture);
		}
		break;
	}

	const int k_size = (int)data->kernel_size;
	if (data->param_kernel_size) {
		gs_effect_set_int(data->param_kernel_size, k_size);
	}

	struct vec2 texel_step;
	float rads = -data->angle * (M_PI / 180.0f);
	texel_step.x = (float)cos(rads) / data->width;
	texel_step.y = (float)sin(rads) / data->height;
	if (data->param_texel_step) {
		gs_effect_set_vec2(data->param_texel_step, &texel_step);
	}

	blur_set_blending_parameters();

	data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);

	//const char *technique = data->background ? "DrawComposite" : "Draw";
	const char *technique = "Draw";

	if (gs_texrender_begin(data->output_texrender, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, technique))
			gs_draw_sprite(texture, 0, data->width, data->height);
		gs_texrender_end(data->output_texrender);
	}

	gs_blend_state_pop();
}

/*
 *  Performs a motion blur using the gaussian kernel.
 */
static void gaussian_motion_blur(blur_filter_data_t *data)
{
	gs_effect_t *effect = data->effect;
	gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);

	if (!effect || !texture) {
		return;
	}

	if (data->radius < MIN_GAUSSIAN_BLUR_RADIUS) {
		data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);
		blur_texrender_set_texture(texture, data->output_texrender);
		return;
	}

	texture = blend_composite(texture, data);

	// 1. Single pass- blur only in one direction
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	switch (data->device_type) {
	case GS_DEVICE_DIRECT3D_11:
		if (data->param_weight) {
			gs_effect_set_val(data->param_weight, data->kernel.array, data->kernel.num * sizeof(float));
		}
		if (data->param_offset) {
			gs_effect_set_val(data->param_offset, data->offset.array, data->offset.num * sizeof(float));
		}
		break;
	case GS_DEVICE_OPENGL:
		if (data->param_kernel_texture) {
			gs_effect_set_texture(data->param_kernel_texture, data->kernel_texture);
		}
		break;
	}

	const int k_size = (int)data->kernel_size;
	if (data->param_kernel_size) {
		gs_effect_set_int(data->param_kernel_size, k_size);
	}

	struct vec2 texel_step;
	float rads = -data->angle * (M_PI / 180.0f);
	texel_step.x = (float)cos(rads) / data->width;
	texel_step.y = (float)sin(rads) / data->height;
	if (data->param_texel_step) {
		gs_effect_set_vec2(data->param_texel_step, &texel_step);
	}

	blur_set_blending_parameters();

	data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);

	if (gs_texrender_begin(data->output_texrender, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture, 0, data->width, data->height);
		gs_texrender_end(data->output_texrender);
	}

	gs_blend_state_pop();
}

/*
 *  Performs a zoom blur using the gaussian kernel. Blur for a pixel
 *  is performed in direction of zoom center point.
 */
static void gaussian_zoom_blur(blur_filter_data_t *data)
{
	gs_effect_t *effect = data->effect;
	gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);

	if (!effect || !texture) {
		return;
	}

	if (data->radius < MIN_GAUSSIAN_BLUR_RADIUS) {
		data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);
		blur_texrender_set_texture(texture, data->output_texrender);
		return;
	}

	texture = blend_composite(texture, data);

	// 1. Single pass- blur only in one direction
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	switch (data->device_type) {
	case GS_DEVICE_DIRECT3D_11:
		if (data->param_weight) {
			gs_effect_set_val(data->param_weight, data->kernel.array, data->kernel.num * sizeof(float));
		}
		if (data->param_offset) {
			gs_effect_set_val(data->param_offset, data->offset.array, data->offset.num * sizeof(float));
		}
		break;
	case GS_DEVICE_OPENGL:
		if (data->param_kernel_texture) {
			gs_effect_set_texture(data->param_kernel_texture, data->kernel_texture);
		}
		break;
	}

	const int k_size = (int)data->kernel_size;
	if (data->param_kernel_size) {
		gs_effect_set_int(data->param_kernel_size, k_size);
	}

	struct vec2 radial_center;
	radial_center.x = data->center_x;
	radial_center.y = data->center_y;
	if (data->param_radial_center) {
		gs_effect_set_vec2(data->param_radial_center, &radial_center);
	}

	if (data->param_inactive_radius) {
		gs_effect_set_float(data->param_inactive_radius, data->inactive_radius);
	}

	struct vec2 uv_size;
	uv_size.x = (float)data->width;
	uv_size.y = (float)data->height;
	if (data->param_uv_size) {
		gs_effect_set_vec2(data->param_uv_size, &uv_size);
	}

	blur_set_blending_parameters();

	data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);

	if (gs_texrender_begin(data->output_texrender, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture, 0, data->width, data->height);
		gs_texrender_end(data->output_texrender);
	}

	gs_blend_state_pop();
}

/*
 *  Performs a vector blur using the gaussian kernel. Blur for a pixel
 *  is performed in direction of the image gradient.
 */
static void gaussian_vector_blur(blur_filter_data_t *data)
{
	if (!gaussian_vector_gradient(data) || !gaussian_vector_smooth_gradient(data)) {
		// No usable gradient map (e.g. the vector source is missing):
		// pass the input through unchanged.
		gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);
		if (!texture)
			return;
		data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);
		blur_texrender_set_texture(texture, data->output_texrender);
		return;
	}

	gaussian_vector_apply_blur(data);
}

// Computes the image gradient of the vector source (or the input if no
// vector source is set) into data->vb_gradient.
static bool gaussian_vector_gradient(blur_filter_data_t *data)
{
	gs_effect_t *effect = data->gradient_effect;
	gs_texture_t *texture = NULL;

	obs_source_t *source = external_source_get(data, &data->vector_source);
	if (source) {
		// Compute gradients from linear (float) values.
		const enum gs_color_space preferred_spaces[] = {
			GS_CS_SRGB_16F,
			GS_CS_709_EXTENDED,
		};
		const enum gs_color_space space =
			obs_source_get_color_space(source, OBS_COUNTOF(preferred_spaces), preferred_spaces);
		texture = render_source_to_texrender(source, &data->vector_source_texrender, space);
		obs_source_release(source);
	} else {
		bool has_vector_source;
		pthread_mutex_lock(&data->sources_mutex);
		has_vector_source = data->vector_source.name.len > 0;
		pthread_mutex_unlock(&data->sources_mutex);
		if (has_vector_source) {
			// Named but not (yet) available.
			return false;
		}
		texture = gs_texrender_get_texture(data->input_texrender);
	}

	if (!effect || !texture) {
		return false;
	}

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);
	if (data->param_gradient_image) {
		gs_effect_set_texture(data->param_gradient_image, texture);
	}

	if (data->param_gradient_channel) {
		gs_effect_set_int(data->param_gradient_channel, data->vector_blur_channel);
	}
	const char *technique = data->vector_blur_type == GRADIENT_TYPE_SOBEL                ? "DrawSobelGrad"
				: data->vector_blur_type == GRADIENT_TYPE_CENTRAL_LIMIT_DIFF ? "DrawCentralDiffGrad"
											     : "DrawForwardDiffGrad";
	struct vec2 uv_size;
	uv_size.x = (float)data->width;
	uv_size.y = (float)data->height;
	if (data->param_gradient_uv_size) {
		gs_effect_set_vec2(data->param_gradient_uv_size, &uv_size);
	}

	blur_set_blending_parameters();

	// Gradients are an encoded vector field, independent of the
	// pipeline's color space.
	data->vb_gradient = blur_create_or_reset_texrender(data->vb_gradient, GS_RGBA);

	bool rendered = false;
	if (gs_texrender_begin(data->vb_gradient, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, technique))
			gs_draw_sprite(texture, 0, data->width, data->height);
		gs_texrender_end(data->vb_gradient);
		rendered = true;
	}

	gs_blend_state_pop();
	return rendered;
}

// Smooths data->vb_gradient into data->vb_smoothed_gradient.
static bool gaussian_vector_smooth_gradient(blur_filter_data_t *data)
{
	gs_texture_t *gradient = gs_texrender_get_texture(data->vb_gradient);
	if (!gradient)
		return false;

	gs_texrender_t *tmp = data->output_texrender;
	data->output_texrender = data->vb_smoothed_gradient;
	data->vb_smoothed_gradient = tmp;

	dual_kawase_blur_texture(data, gradient, data->vector_blur_smoothing + 1.0f);

	tmp = data->output_texrender;
	data->output_texrender = data->vb_smoothed_gradient;
	data->vb_smoothed_gradient = tmp;

	return data->vb_smoothed_gradient && gs_texrender_get_texture(data->vb_smoothed_gradient);
}

static void gaussian_vector_apply_blur(blur_filter_data_t *data)
{
	gs_effect_t *effect = data->gv_effect;
	gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);

	if (!effect || !texture) {
		return;
	}

	if (fabsf(data->vector_blur_amount) < MIN_GAUSSIAN_BLUR_RADIUS) {
		data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);
		blur_texrender_set_texture(texture, data->output_texrender);
		return;
	}

	texture = blend_composite(texture, data);

	// 1. Single pass- blur only in one direction
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	switch (data->device_type) {
	case GS_DEVICE_DIRECT3D_11:
		if (data->param_weight) {
			gs_effect_set_val(data->param_weight, data->kernel.array, data->kernel.num * sizeof(float));
		}
		if (data->param_offset) {
			gs_effect_set_val(data->param_offset, data->offset.array, data->offset.num * sizeof(float));
		}
		break;
	case GS_DEVICE_OPENGL:
		if (data->param_kernel_texture) {
			gs_effect_set_texture(data->param_kernel_texture, data->kernel_texture);
		}
		break;
	}

	if (data->param_radius) {
		gs_effect_set_float(data->param_radius, data->vector_blur_amount);
	}

	const int k_size = (int)data->kernel_size;
	if (data->param_kernel_size) {
		gs_effect_set_int(data->param_kernel_size, k_size);
	}

	struct vec2 uv_size;
	uv_size.x = (float)data->width;
	uv_size.y = (float)data->height;
	if (data->param_uv_size) {
		gs_effect_set_vec2(data->param_uv_size, &uv_size);
	}

	if (data->param_gradient_map) {
		gs_texture_t *gradient_map = gs_texrender_get_texture(data->vb_smoothed_gradient);
		gs_effect_set_texture(data->param_gradient_map, gradient_map);
	}

	blur_set_blending_parameters();

	data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);

	if (gs_texrender_begin(data->output_texrender, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture, 0, data->width, data->height);
		gs_texrender_end(data->output_texrender);
	}

	gs_blend_state_pop();
}

static void load_1d_gaussian_effect(blur_filter_data_t *filter)
{
	const struct effect_param_binding params[] = {
		EFFECT_PARAM("uv_size", filter->param_uv_size),
		EFFECT_PARAM("texel_step", filter->param_texel_step),
		EFFECT_PARAM("offset", filter->param_offset),
		EFFECT_PARAM("weight", filter->param_weight),
		EFFECT_PARAM("kernel_size", filter->param_kernel_size),
		EFFECT_PARAM("kernel_texture", filter->param_kernel_texture),
	};
	const char *effect_file_path = filter->device_type == GS_DEVICE_DIRECT3D_11
					       ? "/shaders/blur/gaussian_1d.effect"
					       : "/shaders/blur/gaussian_1d_texture.effect";
	LOAD_EFFECT(&filter->effect, effect_file_path, params);
}

static void load_motion_gaussian_effect(blur_filter_data_t *filter)
{
	const struct effect_param_binding params[] = {
		EFFECT_PARAM("uv_size", filter->param_uv_size),
		EFFECT_PARAM("texel_step", filter->param_texel_step),
		EFFECT_PARAM("offset", filter->param_offset),
		EFFECT_PARAM("weight", filter->param_weight),
		EFFECT_PARAM("kernel_size", filter->param_kernel_size),
		EFFECT_PARAM("kernel_texture", filter->param_kernel_texture),
	};
	const char *effect_file_path = filter->device_type == GS_DEVICE_DIRECT3D_11
					       ? "/shaders/blur/gaussian_motion.effect"
					       : "/shaders/blur/gaussian_motion_texture.effect";
	LOAD_EFFECT(&filter->effect, effect_file_path, params);
}

static void load_radial_gaussian_effect(blur_filter_data_t *filter)
{
	const struct effect_param_binding params[] = {
		EFFECT_PARAM("uv_size", filter->param_uv_size),
		EFFECT_PARAM("offset", filter->param_offset),
		EFFECT_PARAM("weight", filter->param_weight),
		EFFECT_PARAM("kernel_size", filter->param_kernel_size),
		EFFECT_PARAM("kernel_texture", filter->param_kernel_texture),
		EFFECT_PARAM("radial_center", filter->param_radial_center),
		EFFECT_PARAM("inactive_radius", filter->param_inactive_radius),
	};
	const char *effect_file_path = filter->device_type == GS_DEVICE_DIRECT3D_11
					       ? "/shaders/blur/gaussian_radial.effect"
					       : "/shaders/blur/gaussian_radial_texture.effect";
	LOAD_EFFECT(&filter->effect, effect_file_path, params);
}

static void load_vector_gaussian_effect(blur_filter_data_t *filter)
{
	const struct effect_param_binding params[] = {
		EFFECT_PARAM("uv_size", filter->param_uv_size),
		EFFECT_PARAM("gradient", filter->param_gradient_map),
		EFFECT_PARAM("offset", filter->param_offset),
		EFFECT_PARAM("weight", filter->param_weight),
		EFFECT_PARAM("kernel_size", filter->param_kernel_size),
		EFFECT_PARAM("kernel_texture", filter->param_kernel_texture),
		EFFECT_PARAM("blur_radius", filter->param_radius),
	};
	const char *effect_file_path = filter->device_type == GS_DEVICE_DIRECT3D_11
					       ? "/shaders/blur/gaussian_vector.effect"
					       : "/shaders/blur/gaussian_vector_texture.effect";
	LOAD_EFFECT(&filter->gv_effect, effect_file_path, params);
}

static void load_gradient_effect(blur_filter_data_t *filter)
{
	const struct effect_param_binding params[] = {
		EFFECT_PARAM("uv_size", filter->param_gradient_uv_size),
		EFFECT_PARAM("image", filter->param_gradient_image),
		EFFECT_PARAM("channel", filter->param_gradient_channel),
	};
	const char *sample_type = filter->vector_blur_channel == GRADIENT_CHANNEL_LUMINANCE    ? "luminance"
				  : filter->vector_blur_channel == GRADIENT_CHANNEL_SATURATION ? "saturation"
											       : "sample_channel";
	load_effect_with_params(&filter->gradient_effect, "/shaders/blur/gradient_map.effect", params,
				OBS_COUNTOF(params), "<SAMPLE_FUNCTION>", sample_type);
}

static void sample_kernel(float radius, blur_filter_data_t *filter)
{
	const size_t max_size = 128;
	const float max_radius = 250.0;
	const float min_radius = 0.0;

	fDarray d_weights;
	da_init(d_weights);

	radius *= 3.0f;
	radius = (float)fmax(fmin(radius, max_radius), min_radius);

	// 1. Calculate discrete weights
	const float bins_per_pixel = ((2.f * (float)gaussian_kernel_size - 1.f)) / (1.f + 2.f * radius);
	size_t current_bin = 0;
	float fractional_bin = 0.5f;
	float ceil_radius = (radius - (float)floor(radius)) < 0.001f ? radius : (float)ceil(radius);
	float fractional_extra = 1.0f - (ceil_radius - radius);

	for (int i = 0; i <= (int)ceil_radius; i++) {
		float fractional_pixel = i < (int)ceil_radius        ? 1.0f
					 : fractional_extra < 0.002f ? 1.0f
								     : fractional_extra;
		float bpp_mult = i == 0 ? 0.5f : 1.0f;
		float weight = 1.0f / bpp_mult * fractional_bin * gaussian_kernel[current_bin];
		float remaining_bins = bpp_mult * fractional_pixel * bins_per_pixel - fractional_bin;
		while ((int)floor(remaining_bins) > 0) {
			current_bin++;
			weight += 1.0f / bpp_mult * gaussian_kernel[current_bin];
			remaining_bins -= 1.f;
		}
		current_bin++;
		if (remaining_bins > 1.e-6f) {
			weight += 1.0f / bpp_mult * gaussian_kernel[current_bin] * remaining_bins;
			fractional_bin = 1.0f - remaining_bins;
		} else {
			fractional_bin = 1.0f;
		}
		if (weight > 1.0001f || weight < 0.0f) {
			blog(LOG_WARNING, "   === BAD WEIGHT VALUE FOR GAUSSIAN === [%d] %f", (int)(d_weights.num + 1),
			     weight);
			weight = 0.0;
		}
		da_push_back(d_weights, &weight);
	}
	fDarray d_offsets;
	da_init(d_offsets);

	// 2. Calculate discrete offsets
	for (int i = 0; i <= (int)ceil_radius; i++) {
		float val = (float)i;
		da_push_back(d_offsets, &val);
	}

	fDarray weights;
	da_init(weights);

	fDarray offsets;
	da_init(offsets);

	DARRAY(float) weight_offset_texture;
	da_init(weight_offset_texture);

	// 3. Calculate linear sampled weights and offsets
	da_push_back(weights, &d_weights.array[0]);
	da_push_back(offsets, &d_offsets.array[0]);

	da_push_back(weight_offset_texture, &d_weights.array[0]);
	da_push_back(weight_offset_texture, &d_offsets.array[0]);

	for (size_t i = 1; i < d_weights.num - 1; i += 2) {
		const float weight = d_weights.array[i] + d_weights.array[i + 1];
		const float offset =
			(d_offsets.array[i] * d_weights.array[i] + d_offsets.array[i + 1] * d_weights.array[i + 1]) /
			weight;

		da_push_back(weights, &weight);
		da_push_back(offsets, &offset);

		da_push_back(weight_offset_texture, &weight);
		da_push_back(weight_offset_texture, &offset);
	}
	if (d_weights.num % 2 == 0) {
		const float weight = d_weights.array[d_weights.num - 1];
		const float offset = d_offsets.array[d_offsets.num - 1];
		da_push_back(weights, &weight);
		da_push_back(offsets, &offset);

		da_push_back(weight_offset_texture, &weight);
		da_push_back(weight_offset_texture, &offset);
	}

	// 4. Pad out kernel arrays to length of max_size
	const size_t kernel_size = weights.num;
	const size_t padding = max_size > weights.num ? max_size - weights.num : 0;

	for (size_t i = 0; i < padding; i++) {
		float pad = 0.0f;
		da_push_back(weights, &pad);
	}
	for (size_t i = 0; i < padding; i++) {
		float pad = 0.0f;
		da_push_back(offsets, &pad);
	}

	// Swap in the new kernel while holding the graphics lock: the render
	// thread reads filter->kernel/offset/kernel_texture while drawing.
	//
	// The kernel and offsets are also uploaded as a texture for OpenGL
	// systems where the red value is the kernel weight and the green value
	// is the offset value.
	obs_enter_graphics();
	fDarray old_kernel = filter->kernel;
	fDarray old_offset = filter->offset;
	filter->kernel = weights;
	filter->offset = offsets;
	filter->kernel_size = kernel_size;

	if (filter->kernel_texture) {
		gs_texture_destroy(filter->kernel_texture);
	}

	filter->kernel_texture = gs_texture_create((uint32_t)weight_offset_texture.num / 2u, 1u, GS_RG32F, 1u,
						   (const uint8_t **)&weight_offset_texture.array, 0);

	if (!filter->kernel_texture) {
		blog(LOG_WARNING, "[Spectrum] Gaussian kernel texture couldn't be created.");
	}
	obs_leave_graphics();

	da_free(old_kernel);
	da_free(old_offset);
	da_free(d_weights);
	da_free(d_offsets);
	da_free(weight_offset_texture);
}
