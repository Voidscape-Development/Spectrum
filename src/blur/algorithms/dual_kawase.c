#include "dual_kawase.h"

static gs_texture_t *mix_textures(blur_filter_data_t *data, gs_texture_t *base, gs_texture_t *residual, float ratio);

void set_dual_kawase_blur_types(obs_properties_t *props)
{
	obs_property_t *p = obs_properties_get(props, "blur_type");
	obs_property_list_clear(p);
	obs_property_list_add_int(p, obs_module_text(TYPE_AREA_LABEL), TYPE_AREA);
}

void dual_kawase_setup_callbacks(blur_filter_data_t *data)
{
	data->video_render = render_video_dual_kawase;
	data->load_effect = blur_load_effect_dual_kawase;
	data->update = NULL;
}

// Applies an in-progress Move transition (if any) to the pass count.
static float get_kawase_passes(blur_filter_data_t *data)
{
	float kawase_passes = data->kawase_passes;

	float f = 0.0f;
	obs_source_t *filter_to = NULL;
	if (move_get_transition_filter)
		f = move_get_transition_filter(data->context, &filter_to);
	if (f <= 0.0f)
		return kawase_passes;

	if (filter_to) {
		blur_filter_data_t *data_to = obs_obj_get_data(filter_to);
		if (data_to && data_to->blur_algorithm == data->blur_algorithm) {
			kawase_passes = data->kawase_passes * (1.0f - f) + data_to->kawase_passes * f;
		} else if (f > 0.5f) {
			kawase_passes *= 1.0f - (f - 0.5f) * 2.0f;
		} else {
			kawase_passes *= 1.0f - f * 2.0f;
		}
	} else {
		kawase_passes *= 1.0f - f;
	}
	return kawase_passes;
}

void render_video_dual_kawase(blur_filter_data_t *data)
{
	gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);
	if (!texture) {
		return;
	}

	const float kawase_passes = get_kawase_passes(data);
	if (kawase_passes <= 0.01f) {
		data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);
		blur_texrender_set_texture(texture, data->output_texrender);
		return;
	}

	texture = blend_composite(texture, data);
	dual_kawase_blur_texture(data, texture, kawase_passes);
}

void blur_load_effect_dual_kawase(blur_filter_data_t *filter)
{
	// The kawase shaders' parameters are looked up by name at draw time.
	load_effect_with_params(&filter->effect_2, "/shaders/blur/dual_kawase_down_sample.effect", NULL, 0, NULL, NULL);
	load_effect_with_params(&filter->effect, "/shaders/blur/dual_kawase_up_sample.effect", NULL, 0, NULL, NULL);
}

// Rotates data->render2 into data->render and resets it for drawing.
static void next_scratch(blur_filter_data_t *data)
{
	gs_texrender_t *tmp = data->render;
	data->render = data->render2;
	data->render2 = tmp;
	data->render = blur_create_or_reset_texrender(data->render, data->color_format);
}

static gs_texture_t *down_sample(blur_filter_data_t *data, gs_texture_t *input_texture, int divisor, float ratio)
{
	gs_effect_t *effect_down = data->effect_2;
	next_scratch(data);

	uint32_t w = data->width / divisor;
	uint32_t h = data->height / divisor;
	if (w == 0)
		w = 1;
	if (h == 0)
		h = 1;
	gs_eparam_t *image = gs_effect_get_param_by_name(effect_down, "image");
	gs_effect_set_texture(image, input_texture);

	gs_eparam_t *texel_step = gs_effect_get_param_by_name(effect_down, "texel_step");
	struct vec2 texel_step_size;

	texel_step_size.x = ratio / (float)w;
	texel_step_size.y = ratio / (float)h;
	gs_effect_set_vec2(texel_step, &texel_step_size);

	if (gs_texrender_begin(data->render, w, h)) {
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(effect_down, "Draw"))
			gs_draw_sprite(input_texture, 0, w, h);
		gs_texrender_end(data->render);
	}
	return gs_texrender_get_texture(data->render);
}

static gs_texture_t *up_sample(blur_filter_data_t *data, gs_texture_t *input_texture, int divisor, float ratio)
{
	gs_effect_t *effect_up = data->effect;
	next_scratch(data);

	uint32_t start_w = gs_texture_get_width(input_texture);
	uint32_t start_h = gs_texture_get_height(input_texture);

	uint32_t w = data->width / divisor;
	uint32_t h = data->height / divisor;
	if (w == 0)
		w = 1;
	if (h == 0)
		h = 1;
	gs_eparam_t *image = gs_effect_get_param_by_name(effect_up, "image");
	gs_effect_set_texture(image, input_texture);

	gs_eparam_t *texel_step = gs_effect_get_param_by_name(effect_up, "texel_step");
	struct vec2 texel_step_size;
	texel_step_size.x = ratio / (float)start_w;
	texel_step_size.y = ratio / (float)start_h;
	gs_effect_set_vec2(texel_step, &texel_step_size);

	if (gs_texrender_begin(data->render, w, h)) {
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(effect_up, "Draw"))
			gs_draw_sprite(input_texture, 0, w, h);
		gs_texrender_end(data->render);
	}
	return gs_texrender_get_texture(data->render);
}

static gs_texture_t *mix_textures(blur_filter_data_t *data, gs_texture_t *base, gs_texture_t *residual, float ratio)
{
	gs_effect_t *effect = data->mix_effect;
	next_scratch(data);

	uint32_t w = gs_texture_get_width(base);
	uint32_t h = gs_texture_get_height(base);

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, base);

	gs_eparam_t *image2 = gs_effect_get_param_by_name(effect, "image2");
	gs_effect_set_texture(image2, residual);

	gs_eparam_t *ratio_param = gs_effect_get_param_by_name(effect, "ratio");
	gs_effect_set_float(ratio_param, ratio);

	if (gs_texrender_begin(data->render, w, h)) {
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(base, 0, w, h);
		gs_texrender_end(data->render);
	}
	return gs_texrender_get_texture(data->render);
}

void dual_kawase_blur_texture(blur_filter_data_t *data, gs_texture_t *texture, float kawase_passes)
{
	if (!data->effect || !data->effect_2 || !data->mix_effect || !texture) {
		return;
	}

	if (kawase_passes <= 0.01f) {
		data->output_texrender = blur_create_or_reset_texrender(data->output_texrender, data->color_format);
		blur_texrender_set_texture(texture, data->output_texrender);
		return;
	}

	blur_set_blending_parameters();

	// TODO: Should we convert Kawase to be 1 based instead of 2.
	int last_pass = 0;
	// Down Sampling Loop
	for (int i = 2; i <= kawase_passes; i *= 2) {
		texture = down_sample(data, texture, i, 1.0);
		last_pass = i;
	}

	float residual = last_pass > 0 ? kawase_passes - (float)last_pass : kawase_passes;
	last_pass = last_pass > 0 ? last_pass : 1;
	if (residual > 0.0f) {
		int next_pass = last_pass * 2;
		float ratio = residual / (float)(next_pass - last_pass);

		// `texture` is the base level we mix the extra step back into.
		// If it lives in our scratch pool, move it somewhere the next
		// down/up samples won't overwrite it. Otherwise it is the
		// caller's input, which we never write to.
		gs_texture_t *base = texture;
		if (data->render && texture == gs_texrender_get_texture(data->render)) {
			gs_texrender_t *tmp = data->kawase_base_texrender;
			data->kawase_base_texrender = data->render;
			data->render = tmp;
		}

		// Downsample one more step
		texture = down_sample(data, texture, next_pass, 1.0);
		// Upsample one more step
		texture = up_sample(data, texture, last_pass, 1.0);
		// Mix the end of the downsample loop with additional step.
		// Use the residual ratio for mixing.
		texture = mix_textures(data, base, texture, ratio);
	}
	// Upsample Loop
	for (int i = last_pass / 2; i >= 1; i /= 2) {
		texture = up_sample(data, texture, i, 1.0);
	}

	gs_blend_state_pop();

	// The result is in data->render; hand it over as the output.
	gs_texrender_t *tmp = data->render;
	data->render = data->output_texrender;
	data->output_texrender = tmp;
}
