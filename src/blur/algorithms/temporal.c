#include "temporal.h"

void temporal_setup_callbacks(blur_filter_data_t *data)
{
	data->video_render = render_video_temporal;
	data->load_effect = load_effect_temporal;
	data->update = NULL;
}

void render_video_temporal(blur_filter_data_t *data)
{
	gs_effect_t *effect = data->effect;
	gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);

	if (!effect || !texture) {
		return;
	}

	texture = blend_composite(texture, data);

	if (data->param_temporal_image) {
		gs_effect_set_texture(data->param_temporal_image, texture);
	}

	if (data->param_temporal_current_weight) {
		gs_effect_set_float(data->param_temporal_current_weight, data->temporal_current_weight);
	}

	if (data->param_temporal_clear_threshold) {
		gs_effect_set_float(data->param_temporal_clear_threshold, data->temporal_clear_threshold);
	}

	// The history lives in its own texrender, so later pipeline stages
	// (e.g. masks, which swap output/render) can't leak into it.
	gs_texture_t *prior_texture = data->temporal_prior_stored && data->temporal_history
					      ? gs_texrender_get_texture(data->temporal_history)
					      : NULL;
	if (!prior_texture) {
		prior_texture = texture;
	}

	if (data->param_temporal_prior_image) {
		gs_effect_set_texture(data->param_temporal_prior_image, prior_texture);
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

	// Keep this frame's result as next frame's history.
	gs_texture_t *result = gs_texrender_get_texture(data->output_texrender);
	if (result) {
		data->temporal_history = blur_create_or_reset_texrender(data->temporal_history, data->color_format);
		blur_texrender_set_texture(result, data->temporal_history);
		data->temporal_prior_stored = true;
	}
}

void load_effect_temporal(blur_filter_data_t *filter)
{
	const struct effect_param_binding params[] = {
		EFFECT_PARAM("image", filter->param_temporal_image),
		EFFECT_PARAM("prior_image", filter->param_temporal_prior_image),
		EFFECT_PARAM("current_weight", filter->param_temporal_current_weight),
		EFFECT_PARAM("clear_threshold", filter->param_temporal_clear_threshold),
	};
	LOAD_EFFECT(&filter->effect, "/shaders/blur/temporal_blur.effect", params);
}
