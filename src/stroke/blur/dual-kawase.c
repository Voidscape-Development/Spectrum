#include "dual-kawase.h"

static void load_dual_kawase_down_sample_effect(alpha_blur_data_t *filter);
static void load_dual_kawase_up_sample_effect(alpha_blur_data_t *filter);
static void load_dual_kawase_mix_effect(alpha_blur_data_t *filter);

static gs_texture_t *mix_textures(alpha_blur_data_t *data, gs_texture_t *base, gs_texture_t *residual, float ratio);
static gs_texture_t *down_sample(alpha_blur_data_t *data, gs_texture_t *input_texture, int divisor, float ratio,
				 uint32_t width, uint32_t height, const char *technique);
static gs_texture_t *up_sample(alpha_blur_data_t *data, gs_texture_t *input_texture, int divisor, float ratio,
			       uint32_t width, uint32_t height, const char *technique);

void stroke_load_effect_dual_kawase(alpha_blur_data_t *filter)
{
	load_dual_kawase_down_sample_effect(filter);
	load_dual_kawase_up_sample_effect(filter);
	load_dual_kawase_mix_effect(filter);
}

// Size of a downsampled level. Rounds up and never reaches 0: a zero-sized
// level fails to render, which left large blurs on small sources (e.g. text)
// with an empty mask.
static uint32_t level_size(uint32_t size, int divisor)
{
	uint32_t s = (size + (uint32_t)divisor - 1) / (uint32_t)divisor;
	return s > 0 ? s : 1;
}

static gs_texture_t *down_sample(alpha_blur_data_t *data, gs_texture_t *input_texture, int divisor, float ratio,
				 uint32_t width, uint32_t height, const char *technique)
{
	gs_effect_t *effect_down = data->effect_dual_kawase_downsample;
	// Swap renderers
	gs_texrender_t *tmp = data->render;
	data->render = data->render2;
	data->render2 = tmp;

	data->render = create_or_reset_texrender_format(data->render, GS_R16F);

	uint32_t w = level_size(width, divisor);
	uint32_t h = level_size(height, divisor);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect_down, "image");
	gs_effect_set_texture(image, input_texture);

	gs_eparam_t *texel_step = gs_effect_get_param_by_name(effect_down, "texel_step");
	struct vec2 texel_step_size;

	texel_step_size.x = ratio / (float)w;
	texel_step_size.y = ratio / (float)h;
	gs_effect_set_vec2(texel_step, &texel_step_size);

	if (gs_texrender_begin(data->render, w, h)) {
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(effect_down, technique))
			gs_draw_sprite(input_texture, 0, w, h);
		gs_texrender_end(data->render);
	}
	return gs_texrender_get_texture(data->render);
}

static gs_texture_t *up_sample(alpha_blur_data_t *data, gs_texture_t *input_texture, int divisor, float ratio,
			       uint32_t width, uint32_t height, const char *technique)
{
	gs_effect_t *effect_up = data->effect_dual_kawase_upsample;
	// Swap renderers
	gs_texrender_t *tmp = data->render;
	data->render = data->render2;
	data->render2 = tmp;

	data->render = create_or_reset_texrender_format(data->render, GS_R16F);

	uint32_t start_w = gs_texture_get_width(input_texture);
	uint32_t start_h = gs_texture_get_height(input_texture);

	uint32_t w = level_size(width, divisor);
	uint32_t h = level_size(height, divisor);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect_up, "image");
	gs_effect_set_texture(image, input_texture);

	gs_eparam_t *texel_step = gs_effect_get_param_by_name(effect_up, "texel_step");
	struct vec2 texel_step_size;
	texel_step_size.x = ratio / (float)start_w;
	texel_step_size.y = ratio / (float)start_h;
	gs_effect_set_vec2(texel_step, &texel_step_size);

	if (gs_texrender_begin(data->render, w, h)) {
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(effect_up, technique))
			gs_draw_sprite(input_texture, 0, w, h);
		gs_texrender_end(data->render);
	}
	return gs_texrender_get_texture(data->render);
}

static gs_texture_t *mix_textures(alpha_blur_data_t *data, gs_texture_t *base, gs_texture_t *residual, float ratio)
{
	gs_effect_t *effect = data->effect_dual_kawase_mix;
	// Swap renderers
	gs_texrender_t *tmp = data->render;
	data->render = data->render2;
	data->render2 = tmp;

	data->render = create_or_reset_texrender_format(data->render, GS_R16F);

	uint32_t w = gs_texture_get_width(base);
	uint32_t h = gs_texture_get_height(base);

	if (data->param_mix_image) {
		gs_effect_set_texture(data->param_mix_image, base);
	}

	if (data->param_mix_image2) {
		gs_effect_set_texture(data->param_mix_image2, residual);
	}

	if (data->param_mix_ratio) {
		gs_effect_set_float(data->param_mix_ratio, ratio);
	}

	if (gs_texrender_begin(data->render, w, h)) {
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "Draw"))
			gs_draw_sprite(base, 0, w, h);
		gs_texrender_end(data->render);
	}
	return gs_texrender_get_texture(data->render);
}

void dual_kawase_blur(int radius, bool ignore_border, alpha_blur_data_t *data, gs_texrender_t *input)
{
	gs_texture_t *texture = gs_texrender_get_texture(input);
	if (!texture) {
		return;
	}
	// Unless the source border is ignored, treat everything outside the
	// source as transparent, as the triangular blur does.
	const char *technique = ignore_border ? "Draw" : "DrawBorder";
	if (radius <= 1) {
		data->alpha_blur_output = create_or_reset_texrender_format(data->alpha_blur_output, GS_R16F);
		stroke_texrender_set_texture(texture, data->alpha_blur_output);
		return;
	}
	gs_effect_t *effect_up = data->effect_dual_kawase_upsample;
	gs_effect_t *effect_down = data->effect_dual_kawase_downsample;
	gs_texrender_t *base_render = NULL;

	if (!effect_down || !effect_up || !texture) {
		return;
	}

	uint32_t width = gs_texture_get_width(texture);
	uint32_t height = gs_texture_get_height(texture);
	stroke_set_blending_parameters();

	int last_pass = 1;
	// Down Sampling Loop
	for (int i = 2; i <= radius; i *= 2) {
		texture = down_sample(data, texture, i, 1.0, width, height, technique);
		last_pass = i;
	}
	int residual = radius - last_pass;
	if (residual > 0) {
		int next_pass = last_pass * 2;
		float ratio = (float)residual / (float)(next_pass - last_pass);

		// Downsample one more step
		texture = down_sample(data, texture, next_pass, 1.0, width, height, technique);
		// Extract renderer from end of down sampling loop
		base_render = data->render2;
		data->render2 = NULL;
		// Upsample one more step
		texture = up_sample(data, texture, last_pass, 1.0, width, height, technique);
		gs_texture_t *base = gs_texrender_get_texture(base_render);
		// Mix the end of the downsample loop with additional step.
		// Use the residual ratio for mixing.
		texture = mix_textures(data, base, texture, ratio);
	}
	// Upsample Loop
	for (int i = last_pass / 2; i >= 1; i /= 2) {
		texture = up_sample(data, texture, i, 1.0, width, height, technique);
	}

	gs_blend_state_pop();

	data->alpha_blur_output = create_or_reset_texrender_format(data->alpha_blur_output, GS_R16F);
	stroke_texrender_set_texture(texture, data->alpha_blur_output);
	// Destroy base_render if used (if there was a residual)
	if (base_render) {
		gs_texrender_destroy(base_render);
	}
}

static void load_dual_kawase_down_sample_effect(alpha_blur_data_t *filter)
{
	const char *effect_file_path = "/shaders/stroke/dual_kawase_down_sample.effect";
	filter->effect_dual_kawase_downsample =
		stroke_load_shader_effect(filter->effect_dual_kawase_downsample, effect_file_path);
	if (filter->effect_dual_kawase_downsample) {
		size_t effect_count = gs_effect_get_num_params(filter->effect_dual_kawase_downsample);
		for (size_t effect_index = 0; effect_index < effect_count; effect_index++) {
			gs_eparam_t *param =
				gs_effect_get_param_by_idx(filter->effect_dual_kawase_downsample, effect_index);
			struct gs_effect_param_info info;
			gs_effect_get_param_info(param, &info);
			if (strcmp(info.name, "texel_step") == 0) {
				filter->param_downsample_texel_step = param;
			}
		}
	}
}

static void load_dual_kawase_up_sample_effect(alpha_blur_data_t *filter)
{
	const char *effect_file_path = "/shaders/stroke/dual_kawase_up_sample.effect";
	filter->effect_dual_kawase_upsample =
		stroke_load_shader_effect(filter->effect_dual_kawase_upsample, effect_file_path);
	if (filter->effect_dual_kawase_upsample) {
		size_t effect_count = gs_effect_get_num_params(filter->effect_dual_kawase_upsample);
		for (size_t effect_index = 0; effect_index < effect_count; effect_index++) {
			gs_eparam_t *param =
				gs_effect_get_param_by_idx(filter->effect_dual_kawase_upsample, effect_index);
			struct gs_effect_param_info info;
			gs_effect_get_param_info(param, &info);
			if (strcmp(info.name, "texel_step") == 0) {
				filter->param_upsample_texel_step = param;
			}
		}
	}
}

static void load_dual_kawase_mix_effect(alpha_blur_data_t *filter)
{
	const char *effect_file_path = "/shaders/stroke/mix.effect";
	filter->effect_dual_kawase_mix = stroke_load_shader_effect(filter->effect_dual_kawase_mix, effect_file_path);
	if (filter->effect_dual_kawase_mix) {
		size_t effect_count = gs_effect_get_num_params(filter->effect_dual_kawase_mix);
		for (size_t effect_index = 0; effect_index < effect_count; effect_index++) {
			gs_eparam_t *param = gs_effect_get_param_by_idx(filter->effect_dual_kawase_mix, effect_index);
			struct gs_effect_param_info info;
			gs_effect_get_param_info(param, &info);
			if (strcmp(info.name, "image") == 0) {
				filter->param_mix_image = param;
			} else if (strcmp(info.name, "image2") == 0) {
				filter->param_mix_image2 = param;
			} else if (strcmp(info.name, "ratio") == 0) {
				filter->param_mix_ratio = param;
			}
		}
	}
}
