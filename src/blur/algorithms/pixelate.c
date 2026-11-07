#include "pixelate.h"
#include "dual_kawase.h"

void set_pixelate_blur_types(obs_properties_t *props)
{
	obs_property_t *p = obs_properties_get(props, "blur_type");
	obs_property_list_clear(p);
	obs_property_list_add_int(p, obs_module_text(TYPE_AREA_LABEL), TYPE_AREA);
}

void pixelate_setup_callbacks(blur_filter_data_t *data)
{
	data->video_render = render_video_pixelate;
	data->load_effect = load_effect_pixelate;
	data->update = NULL;
}

static float get_pixel_size(blur_filter_data_t *data)
{
	float radius = (float)fmax((float)data->radius, 1.0f);
	float f = 0.0f;
	obs_source_t *filter_to = NULL;
	if (move_get_transition_filter)
		f = move_get_transition_filter(data->context, &filter_to);
	if (f <= 0.0f)
		return radius;

	if (filter_to) {
		blur_filter_data_t *data_to = obs_obj_get_data(filter_to);
		if (data_to && data_to->blur_algorithm == data->blur_algorithm) {
			radius = radius * (1.0f - f) + (float)fmax((float)data_to->radius, 1.0f) * f;
		} else if (f > 0.5f) {
			radius = (radius - 1.0f) * (1.0f - (f - 0.5f) * 2.0f) + 1.0f;
		} else {
			radius = (radius - 1.0f) * (1.0f - f * 2.0f) + 1.0f;
		}
	} else {
		radius = (radius - 1.0f) * (1.0f - f) + 1.0f;
	}
	return radius;
}

void render_video_pixelate(blur_filter_data_t *data)
{
	gs_effect_t *effect = data->pixelate_effect;
	gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);

	if (!effect || !texture) {
		return;
	}

	const float radius = get_pixel_size(data);

	if (radius < MIN_PIXELATE_BLUR_SIZE) {
		data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);
		blur_texrender_set_texture(texture, data->output_texrender);
		return;
	}

	texture = blend_composite(texture, data);

	// Optional pre-smoothing with dual kawase. The pass count is derived
	// from the pixel size and passed explicitly, so the user's own
	// kawase setting is never overwritten.
	const float smoothing_passes = data->pixelate_smoothing_pct / 100.0f * radius;
	if (smoothing_passes > 0.01f) {
		dual_kawase_blur_texture(data, texture, smoothing_passes);
		gs_texrender_t *tmp = data->pixelate_texrender;
		data->pixelate_texrender = data->output_texrender;
		data->output_texrender = tmp;
		gs_texture_t *smoothed = data->pixelate_texrender ? gs_texrender_get_texture(data->pixelate_texrender)
								  : NULL;
		if (smoothed)
			texture = smoothed;
	}

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, texture);

	if (data->param_pixel_size) {
		gs_effect_set_float(data->param_pixel_size, radius);
	}

	struct vec2 uv_size;
	uv_size.x = (float)data->width;
	uv_size.y = (float)data->height;
	if (data->param_uv_size) {
		gs_effect_set_vec2(data->param_uv_size, &uv_size);
	}

	if (data->param_pixel_center) {
		gs_effect_set_vec2(data->param_pixel_center, &data->pixelate_tessel_center);
	}

	if (data->param_pixel_cos_theta) {
		gs_effect_set_float(data->param_pixel_cos_theta, data->pixelate_cos_theta);
	}

	if (data->param_pixel_cos_rtheta) {
		gs_effect_set_float(data->param_pixel_cos_rtheta, data->pixelate_cos_rtheta);
	}

	if (data->param_pixel_sin_theta) {
		gs_effect_set_float(data->param_pixel_sin_theta, data->pixelate_sin_theta);
	}

	if (data->param_pixel_sin_rtheta) {
		gs_effect_set_float(data->param_pixel_sin_rtheta, data->pixelate_sin_rtheta);
	}
	if (data->param_pixel_time) {
		gs_effect_set_float(data->param_pixel_time, data->time);
	}

	data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);

	blur_set_blending_parameters();

	if (gs_texrender_begin(data->output_texrender, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(texture, 0, data->width, data->height);
		gs_texrender_end(data->output_texrender);
	}

	gs_blend_state_pop();
}

void load_effect_pixelate(blur_filter_data_t *filter)
{
	// All pixelate shaders share one parameter layout; only voronoi uses
	// `time`. Unmatched parameters are bound to NULL.
	const struct effect_param_binding params[] = {
		EFFECT_PARAM("uv_size", filter->param_uv_size),
		EFFECT_PARAM("pixel_size", filter->param_pixel_size),
		EFFECT_PARAM("tess_origin", filter->param_pixel_center),
		EFFECT_PARAM("tess_rot", filter->param_pixel_rot),
		EFFECT_PARAM("cos_theta", filter->param_pixel_cos_theta),
		EFFECT_PARAM("sin_theta", filter->param_pixel_sin_theta),
		EFFECT_PARAM("cos_rtheta", filter->param_pixel_cos_rtheta),
		EFFECT_PARAM("sin_rtheta", filter->param_pixel_sin_rtheta),
		EFFECT_PARAM("time", filter->param_pixel_time),
	};

	const char *path = NULL;
	switch (filter->pixelate_type) {
	case PIXELATE_TYPE_SQUARE:
		path = "/shaders/blur/pixelate_square.effect";
		break;
	case PIXELATE_TYPE_HEXAGONAL:
		path = "/shaders/blur/pixelate_hexagonal.effect";
		break;
	case PIXELATE_TYPE_CIRCLE:
		path = "/shaders/blur/pixelate_circle.effect";
		break;
	case PIXELATE_TYPE_TRIANGLE:
		path = "/shaders/blur/pixelate_triangle.effect";
		break;
	case PIXELATE_TYPE_VORONOI:
		path = "/shaders/blur/pixelate_voronoi.effect";
		break;
	case PIXELATE_TYPE_RHOMBOID:
		path = "/shaders/blur/pixelate_rhomboid.effect";
		break;
	case PIXELATE_TYPE_TRIAKIS:
		path = "/shaders/blur/pixelate_triakis.effect";
		break;
	}
	if (path) {
		LOAD_EFFECT(&filter->pixelate_effect, path, params);
	}
	blur_load_effect_dual_kawase(filter);
}
