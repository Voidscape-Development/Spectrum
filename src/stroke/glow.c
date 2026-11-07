#include "glow.h"
#include <string.h>

void load_glow_effect(glow_filter_data_t *filter)
{
	const char *effect_file_path = filter->is_filter ? "/shaders/stroke/glow.effect"
							 : "/shaders/stroke/glow_source.effect";
	filter->effect_glow = stroke_load_shader_effect(filter->effect_glow, effect_file_path);
	if (filter->effect_glow) {
		size_t effect_count = gs_effect_get_num_params(filter->effect_glow);
		for (size_t effect_index = 0; effect_index < effect_count; effect_index++) {
			gs_eparam_t *param = gs_effect_get_param_by_idx(filter->effect_glow, effect_index);
			struct gs_effect_param_info info;
			gs_effect_get_param_info(param, &info);
			if (strcmp(info.name, "input_image") == 0) {
				filter->param_glow_image = param;
			} else if (strcmp(info.name, "glow_mask") == 0) {
				filter->param_glow_mask = param;
			} else if (strcmp(info.name, "glow_fill_source") == 0) {
				filter->param_glow_fill_source = param;
			} else if (strcmp(info.name, "glow_fill_color") == 0) {
				filter->param_glow_fill_color = param;
			} else if (strcmp(info.name, "intensity") == 0) {
				filter->param_glow_intensity = param;
			} else if (strcmp(info.name, "offset") == 0) {
				filter->param_offset_texel = param;
			} else if (strcmp(info.name, "fill_behind") == 0) {
				filter->param_glow_fill_behind = param;
			} else if (strcmp(info.name, "mul_val") == 0) {
				filter->param_mul_val = param;
			} else if (strcmp(info.name, "add_val") == 0) {
				filter->param_add_val = param;
			} else if (strcmp(info.name, "threshold") == 0) {
				filter->param_threshold = param;
			}
		}
	}
}

void glow_render_padded_input(glow_filter_data_t *data)
{

	gs_effect_t *effect = data->effect_glow;
	if (!effect) {
		return;
	}

	if (data->is_source) { // this is for the source version.
		data->input_texture_generated = false;
		obs_source_t *input_source = source_ref_get(&data->input_ref);
		if (!input_source) {
			return;
		}

		// Sources draw an SDR stroke/glow of their input, so work in
		// SDR (OBS tonemaps an HDR input when rendering it as SDR).
		const enum gs_color_space space = GS_CS_SRGB;
		data->working_space = space;

		uint32_t source_width = obs_source_get_width(input_source);
		uint32_t source_height = obs_source_get_height(input_source);
		gs_texture_t *texture = render_source_to_texture(input_source, &data->source_texrender);
		obs_source_release(input_source);
		// No size yet (e.g. still loading): nothing to draw.
		if (!texture || !source_width || !source_height) {
			return;
		}

		// Set up our input_texrender to catch the output texture.
		data->input_texrender = stroke_create_or_reset_texrender(data->input_texrender);
		data->input_texture_generated = false;
		// Start the rendering process with our correct color space params,
		// And set up your texrender to recieve the created texture.

		data->width = source_width + data->pad_l + data->pad_r;
		data->height = source_height + data->pad_t + data->pad_b;

		data->mul_val.x = (float)data->width / (float)source_width;
		data->add_val.x = -(float)data->pad_l / (float)source_width;

		data->mul_val.y = (float)data->height / (float)source_height;
		data->add_val.y = -(float)data->pad_t / (float)source_height;

		// Grab the proper padding/
		if (data->param_mul_val) {
			gs_effect_set_vec2(data->param_mul_val, &data->mul_val);
		}
		if (data->param_add_val) {
			gs_effect_set_vec2(data->param_add_val, &data->add_val);
		}
		gs_eparam_t *param = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(param, texture);

		if (gs_texrender_begin_with_color_space(data->input_texrender, data->width, data->height, space)) {

			gs_blend_state_push();
			gs_reset_blend_state();
			gs_enable_blending(false);
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

			gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
			set_encode_input(effect, data->working_space);
			const char *technique = "DrawCropPad";
			while (gs_effect_loop(effect, technique))
				gs_draw_sprite(texture, 0, data->width, data->height);
			gs_texrender_end(data->input_texrender);
			gs_blend_state_pop();
			data->input_texture_generated = true;
		}
	} else {
		obs_source_t *input_source = data->context;

		// This is for the filter version
		// Set up our color space info.
		// Work in the space we're drawn into (see filter_working_space),
		// so HDR (float) content isn't clamped to 8-bit SDR.
		const enum gs_color_space current_space = gs_get_color_space();
		const enum gs_color_space source_space = filter_working_space(input_source, 1, &current_space);
		data->working_space = source_space;

		const enum gs_color_format format = gs_get_format_from_space(source_space);

		// Set up our input_texrender to catch the output texture.
		data->input_texrender = create_or_reset_texrender_format(data->input_texrender, format);
		data->input_texture_generated = false;

		// Start the rendering process with our correct color space params,
		// And set up your texrender to recieve the created texture.
		if (!obs_source_process_filter_begin_with_color_space(input_source, format, source_space,
								      OBS_NO_DIRECT_RENDERING))
			return;

		data->width = data->source_width + data->pad_l + data->pad_r;
		data->height = data->source_height + data->pad_t + data->pad_b;

		data->mul_val.x = (float)data->width / (float)data->source_width;
		data->add_val.x = -(float)data->pad_l / (float)data->source_width;

		data->mul_val.y = (float)data->height / (float)data->source_height;
		data->add_val.y = -(float)data->pad_t / (float)data->source_height;

		// Grab the proper padding/
		if (data->param_mul_val) {
			gs_effect_set_vec2(data->param_mul_val, &data->mul_val);
		}
		if (data->param_add_val) {
			gs_effect_set_vec2(data->param_add_val, &data->add_val);
		}

		if (gs_texrender_begin_with_color_space(data->input_texrender, data->width, data->height,
							source_space)) {

			gs_blend_state_push();
			gs_reset_blend_state();
			gs_enable_blending(false);
			gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

			gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
			set_encode_input(effect, data->working_space);
			const char *technique = "DrawCropPad";
			obs_source_process_filter_tech_end(input_source, effect, data->width, data->height, technique);
			gs_texrender_end(data->input_texrender);
			gs_blend_state_pop();
			data->input_texture_generated = true;
		}
	}
}

void glow_render_cropped_output(glow_filter_data_t *data)
{
	gs_effect_t *effect = data->effect_glow;
	gs_texture_t *texture = data->output_texrender ? gs_texrender_get_texture(data->output_texrender) : NULL;
	if (!effect || !texture) {
		if (data->is_filter) {
			obs_source_skip_video_filter(data->context);
		}
		return;
	}

	// The output texture is the padded size; draw all of it. Inner
	// glows have no padding. Don't modify data->width/height here, this
	// is also called for repeat renders within the same frame.
	const uint32_t width = gs_texture_get_width(texture);
	const uint32_t height = gs_texture_get_height(texture);

	data->mul_val.x = 1.0f;
	data->mul_val.y = 1.0f;
	data->add_val.x = 0.0f;
	data->add_val.y = 0.0f;

	if (data->param_mul_val) {
		gs_effect_set_vec2(data->param_mul_val, &data->mul_val);
	}
	if (data->param_add_val) {
		gs_effect_set_vec2(data->param_add_val, &data->add_val);
	}

	struct vec2 dither_dims;
	vec2_set(&dither_dims, (float)width, (float)height);
	gs_eparam_t *param_dither_size = gs_effect_get_param_by_name(effect, "dither_size");
	if (param_dither_size) {
		gs_effect_set_vec2(param_dither_size, &dither_dims);
	}

	if (data->is_source) {
		gs_eparam_t *param = gs_effect_get_param_by_name(effect, "output_image");
		gs_effect_set_texture(param, texture);

		const bool previous = gs_framebuffer_srgb_enabled();
		const bool linear_srgb = gs_get_linear_srgb() ||
					 (data->fill_type == GLOW_FILL_TYPE_COLOR && data->glow_color.w < 1.0f);
		gs_enable_framebuffer_srgb(linear_srgb);

		const bool color_fill = data->fill_type == GLOW_FILL_TYPE_COLOR;
		const float content_linear = (color_fill && linear_srgb) ? 1.0f : 0.0f;
		const float decode_output = (!color_fill && linear_srgb) ? 1.0f : 0.0f;
		gs_eparam_t *param_content_linear = gs_effect_get_param_by_name(effect, "content_linear");
		if (param_content_linear) {
			gs_effect_set_float(param_content_linear, content_linear);
		}
		gs_eparam_t *param_decode_output = gs_effect_get_param_by_name(effect, "decode_output");
		if (param_decode_output) {
			gs_effect_set_float(param_decode_output, decode_output);
		}

		set_hdr_output(effect, GS_CS_SRGB);

		gs_matrix_push();
		if (data->overflow) {
			gs_matrix_translate3f(-(float)data->pad_l, -(float)data->pad_t, 0.0f);
		}

		gs_technique_t *tech = gs_effect_get_technique(effect, "DrawOutput");
		gs_technique_begin(tech);
		gs_technique_begin_pass(tech, 0);

		gs_draw_sprite(0, 0, width, height);

		gs_technique_end_pass(tech);
		gs_technique_end(tech);

		gs_matrix_pop();
		gs_enable_framebuffer_srgb(previous);
	} else {

		const enum gs_color_space source_space = data->working_space;

		const enum gs_color_format format = gs_get_format_from_space(source_space);

		if (!obs_source_process_filter_begin_with_color_space(data->context, format, source_space,
								      OBS_NO_DIRECT_RENDERING)) {
			return;
		}

		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "output_image");
		gs_effect_set_texture(image, texture);

		gs_eparam_t *input_image = gs_effect_get_param_by_name(effect, "input_image");
		if (input_image) {
			gs_effect_set_texture(input_image, gs_texrender_get_texture(data->input_texrender));
		}

		set_hdr_output(effect, source_space);

		gs_matrix_push();
		if (data->overflow) {
			gs_matrix_translate3f(-(float)data->pad_l, -(float)data->pad_t, 0.0f);
		}
		const char *technique = "DrawOutput";
		obs_source_process_filter_tech_end(data->context, effect, width, height, technique);
		gs_matrix_pop();
	}
}

void render_glow_alpha_mask(glow_filter_data_t *data)
{
	gs_effect_t *effect = data->effect_glow;
	gs_texture_t *input_texture = gs_texrender_get_texture(data->input_texrender);
	if (!effect || !input_texture) {
		return;
	}

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, input_texture);

	if (data->param_threshold) {
		gs_effect_set_float(data->param_threshold, data->threshold);
	}

	data->alpha_mask_texrender = create_or_reset_texrender_format(data->alpha_mask_texrender, GS_R16F);

	stroke_set_blending_parameters();

	if (gs_texrender_begin(data->alpha_mask_texrender, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "ThresholdMask"))
			gs_draw_sprite(NULL, 0, data->width, data->height);
		gs_texrender_end(data->alpha_mask_texrender);
	}

	gs_blend_state_pop();
}

void render_glow_filter(glow_filter_data_t *data)
{
	gs_effect_t *effect = data->effect_glow;

	gs_texture_t *blur_mask_texture = gs_texrender_get_texture(data->alpha_blur_data->alpha_blur_output);
	gs_texture_t *input_texture = gs_texrender_get_texture(data->input_texrender);

	if (!effect || !input_texture || !blur_mask_texture) {
		return;
	}

	const bool linear_srgb = gs_get_linear_srgb() || data->glow_color.w < 1.0f;

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, input_texture);

	gs_eparam_t *glow_mask = gs_effect_get_param_by_name(effect, "glow_mask");
	gs_effect_set_texture(glow_mask, blur_mask_texture);

	struct vec2 scaled_texel;
	scaled_texel.x = data->offset_texel.x / data->width;
	scaled_texel.y = data->offset_texel.y / data->height;

	if (data->param_offset_texel) {
		gs_effect_set_vec2(data->param_offset_texel, &scaled_texel);
	}

	if (data->param_glow_intensity) {
		gs_effect_set_float(data->param_glow_intensity, data->intensity);
	}

	if (data->param_glow_fill_behind) {
		gs_effect_set_float(data->param_glow_fill_behind, data->fill ? 1.0f : 0.0f);
	}
	if (data->param_threshold) {
		gs_effect_set_float(data->param_threshold, data->threshold);
	}

	// Render the fill source. If it is missing (not found, removed or no
	// size), fall back to a transparent color fill rather than skipping the
	// frame.
	gs_texture_t *fill_texture = NULL;
	if (data->fill_type == GLOW_FILL_TYPE_SOURCE) {
		obs_source_t *fill_source = source_ref_get(&data->fill_ref);
		// Rendering our own source here would recurse back into this
		// filter (which then fails to draw), so treat it as no source.
		if (is_self_reference(data->context, fill_source)) {
			obs_source_release(fill_source);
			fill_source = NULL;
		}
		if (fill_source) {
			fill_texture = render_source_to_texture(fill_source, &data->fill_texrender);
			obs_source_release(fill_source);
		}
	} else if (data->fill_type == GLOW_FILL_TYPE_GRADIENT) {
		// Float, so the gradient is only quantised (and dithered) on
		// output.
		fill_texture = gradient_render_fill(&data->gradient, data->width, data->height);
	}
	const bool source_available = fill_texture != NULL;

	if (source_available) {
		if (data->param_glow_fill_source) {
			gs_effect_set_texture(data->param_glow_fill_source, fill_texture);
		}
	} else if (data->fill_type != GLOW_FILL_TYPE_COLOR) {
		if (data->param_glow_fill_color) {
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_effect_set_vec4(data->param_glow_fill_color, &clear_color);
		}
	} else if (data->fill_type == GLOW_FILL_TYPE_COLOR && data->param_glow_fill_color) {
		const struct vec4 *fill_color = data->is_filter
							? &data->glow_color
							: (linear_srgb ? &data->glow_color_srgb : &data->glow_color);
		gs_effect_set_vec4(data->param_glow_fill_color, fill_color);
	}

	data->output_texrender = create_or_reset_texrender_format(data->output_texrender, GS_RGBA16F);

	const bool gradient_strength = data->fill_type == GLOW_FILL_TYPE_GRADIENT_STRENGTH &&
				       gradient_bind_lut(&data->gradient, effect);
	gs_eparam_t *param_fill_gradient = gs_effect_get_param_by_name(effect, "fill_gradient");
	if (param_fill_gradient)
		gs_effect_set_float(param_fill_gradient, gradient_strength ? 1.0f : 0.0f);

	bool fill_color = (data->fill_type == GLOW_FILL_TYPE_COLOR) || !source_available;
	const char *fill_type = fill_color ? "Color" : "Source";
	const char *position = data->glow_position == GLOW_POSITION_OUTER ? "FilterOuterGlow" : "FilterInnerGlow";

	char shader_id[100] = "";
	snprintf(shader_id, sizeof(shader_id), "%s%s", position, fill_type);

	stroke_set_blending_parameters();

	if (gs_texrender_begin(data->output_texrender, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, shader_id))
			gs_draw_sprite(NULL, 0, data->width, data->height);
		gs_texrender_end(data->output_texrender);
	}

	gs_blend_state_pop();
}
