#include "stroke.h"
#include <string.h>

void load_jump_flood_sdf_effect(stroke_filter_data_t *filter)
{
	const char *effect_file_path = filter->is_source ? "/shaders/stroke/jump-flood-sdf-source.effect"
							 : "/shaders/stroke/jump-flood-sdf.effect";
	filter->effect_jump_flood_sdf = stroke_load_shader_effect(filter->effect_jump_flood_sdf, effect_file_path);
	if (filter->effect_jump_flood_sdf) {
		size_t effect_count = gs_effect_get_num_params(filter->effect_jump_flood_sdf);
		for (size_t effect_index = 0; effect_index < effect_count; effect_index++) {
			gs_eparam_t *param = gs_effect_get_param_by_idx(filter->effect_jump_flood_sdf, effect_index);
			struct gs_effect_param_info info;
			gs_effect_get_param_info(param, &info);
			if (strcmp(info.name, "threshold") == 0) {
				filter->param_jump_flood_threshold = param;
			} else if (strcmp(info.name, "uv_size") == 0) {
				filter->param_stroke_uv_size = param;
			} else if (strcmp(info.name, "offset") == 0) {
				filter->param_jump_flood_offset = param;
			} else if (strcmp(info.name, "stroke_offset") == 0) {
				filter->param_jump_flood_stroke_offset = param;
			} else if (strcmp(info.name, "stroke_extent") == 0) {
				filter->param_jump_flood_stroke_extent = param;
			} else if (strcmp(info.name, "overlay") == 0) {
				filter->param_jump_flood_overlay = param;
			} else if (strcmp(info.name, "distance_field") == 0) {
				filter->param_distance_field = param;
			} else if (strcmp(info.name, "stroke_fill_color") == 0) {
				filter->param_jump_flood_stroke_color = param;
			} else if (strcmp(info.name, "stroke_fill_source") == 0) {
				filter->param_jump_flood_fill_source = param;
			} else if (strcmp(info.name, "contour_offset") == 0) {
				filter->param_jump_flood_contour_offset = param;
			} else if (strcmp(info.name, "contour_spacing") == 0) {
				filter->param_jump_flood_contour_spacing = param;
			} else if (strcmp(info.name, "contour_falloff_start") == 0) {
				filter->param_jump_flood_contour_falloff_start = param;
			} else if (strcmp(info.name, "contour_falloff_end") == 0) {
				filter->param_jump_flood_contour_falloff_end = param;
			} else if (strcmp(info.name, "contour_spacing_power") == 0) {
				filter->param_jump_flood_contour_spacing_power = param;
			} else if (strcmp(info.name, "mul_val") == 0) {
				filter->param_mul_val = param;
			} else if (strcmp(info.name, "add_val") == 0) {
				filter->param_add_val = param;
			} else if (strcmp(info.name, "infill") == 0) {
				filter->param_infill = param;
			}
		}
	}
}

static void set_uv_size(stroke_filter_data_t *data)
{
	if (data->param_stroke_uv_size) {
		struct vec2 uv_size;
		uv_size.x = (float)data->width;
		uv_size.y = (float)data->height;
		gs_effect_set_vec2(data->param_stroke_uv_size, &uv_size);
	}
}

void render_padded_input(stroke_filter_data_t *data)
{
	gs_effect_t *effect = data->effect_jump_flood_sdf;
	if (!effect) {
		return;
	}
	if (data->is_source) {
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
		// No size yet (e.g. still loading): nothing to stroke.
		if (!texture || !source_width || !source_height) {
			return;
		}
		data->source_width = source_width;
		data->source_height = source_height;

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
			set_uv_size(data);
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
			set_uv_size(data);
			set_encode_input(effect, data->working_space);
			const char *technique = "DrawCropPad";
			obs_source_process_filter_tech_end(input_source, effect, data->width, data->height, technique);
			gs_texrender_end(data->input_texrender);
			gs_blend_state_pop();
			data->input_texture_generated = true;
		}
	}
}

// Sets up mul_val/add_val to crop the padding back off for inner strokes
// (which only pad to treat the source border as an edge), and returns the
// size of the area to draw. Must not modify data->width/height, as this is
// also called for repeat renders within the same frame.
static void set_output_crop(stroke_filter_data_t *data, uint32_t *out_width, uint32_t *out_height)
{
	const uint32_t base_width = data->width;
	const uint32_t base_height = data->height;
	const bool inner = data->stroke_position == STROKE_POSITION_INNER ||
			   data->stroke_position == STROKE_POSITION_INNER_CONTOUR;

	if (inner && base_width > data->pad_l + data->pad_r && base_height > data->pad_t + data->pad_b) {
		*out_width = base_width - data->pad_l - data->pad_r;
		*out_height = base_height - data->pad_t - data->pad_b;

		data->mul_val.x = (float)*out_width / (float)base_width;
		data->add_val.x = (float)data->pad_l / (float)base_width;
		data->mul_val.y = (float)*out_height / (float)base_height;
		data->add_val.y = (float)data->pad_t / (float)base_height;
	} else {
		*out_width = base_width;
		*out_height = base_height;
		data->mul_val.x = 1.0f;
		data->mul_val.y = 1.0f;
		data->add_val.x = 0.0f;
		data->add_val.y = 0.0f;
	}

	if (data->param_mul_val) {
		gs_effect_set_vec2(data->param_mul_val, &data->mul_val);
	}
	if (data->param_add_val) {
		gs_effect_set_vec2(data->param_add_val, &data->add_val);
	}
	set_uv_size(data);
}

void render_cropped_output(stroke_filter_data_t *data)
{
	gs_effect_t *effect = data->effect_jump_flood_sdf;
	gs_texture_t *texture = data->output_texrender ? gs_texrender_get_texture(data->output_texrender) : NULL;
	if (!effect || !texture) {
		if (data->is_filter) {
			obs_source_skip_video_filter(data->context);
		}
		return;
	}

	uint32_t width, height;

	if (data->is_source) {
		gs_eparam_t *param = gs_effect_get_param_by_name(effect, "output_image");
		gs_effect_set_texture(param, texture);

		const bool previous = gs_framebuffer_srgb_enabled();
		if (data->fill_type == STROKE_FILL_TYPE_COLOR) {
			const bool linear_srgb = gs_get_linear_srgb() || data->stroke_color.w < 1.0f;
			gs_enable_framebuffer_srgb(linear_srgb);
		}

		set_output_crop(data, &width, &height);
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

		// The input, used to leave untouched pixels undithered. (Effect
		// parameters are cleared after every technique, so set it again.)
		gs_texture_t *input_texture = data->input_texrender ? gs_texrender_get_texture(data->input_texrender)
								    : NULL;
		if (data->param_jump_flood_overlay && input_texture) {
			gs_effect_set_texture(data->param_jump_flood_overlay, input_texture);
		}

		set_output_crop(data, &width, &height);
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

// Seeds the jump flood with the sub-pixel edge points of the source's alpha
// contour (see drawSeedImage), into buffer_a.
void render_jf_seed(stroke_filter_data_t *data)
{
	gs_effect_t *effect = data->effect_jump_flood_sdf;
	gs_texture_t *input_texture = gs_texrender_get_texture(data->input_texrender);
	data->distance_field = NULL;

	if (!effect || !input_texture) {
		return;
	}

	data->buffer_a = stroke_create_or_reset_texrender_high(data->buffer_a);

	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_effect_set_texture(image, input_texture);

	if (data->param_jump_flood_threshold) {
		gs_effect_set_float(data->param_jump_flood_threshold, data->jump_flood_threshold);
	}
	set_uv_size(data);

	stroke_set_blending_parameters();
	if (gs_texrender_begin(data->buffer_a, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, "DrawThreshold"))
			gs_draw_sprite(input_texture, 0, data->width, data->height);
		gs_texrender_end(data->buffer_a);
	}
	gs_blend_state_pop();
}

// Runs the jump flood passes, ping-ponging between buffer_a and buffer_b,
// far enough to find the nearest edge for every pixel within max_extent.
// Leaves data->distance_field pointing at whichever buffer holds the result.
void render_jf_passes(stroke_filter_data_t *data, float max_extent)
{
	gs_effect_t *effect = data->effect_jump_flood_sdf;
	if (!effect || !data->buffer_a) {
		return;
	}

	int extent = (int)max_extent + 1;
	int pass = 1;
	while (pass < extent) {
		pass *= 2;
	}

	set_uv_size(data);
	stroke_set_blending_parameters();

	for (; pass > 0; pass /= 2) {
		gs_texture_t *input_texture = gs_texrender_get_texture(data->buffer_a);
		if (!input_texture) {
			break;
		}

		data->buffer_b = stroke_create_or_reset_texrender_high(data->buffer_b);

		gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
		gs_effect_set_texture(image, input_texture);
		if (data->param_jump_flood_offset) {
			gs_effect_set_float(data->param_jump_flood_offset, (float)pass);
		}
		// Parameters are cleared after every technique.
		set_uv_size(data);

		if (gs_texrender_begin(data->buffer_b, data->width, data->height)) {
			gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
			while (gs_effect_loop(effect, "DrawJumpFloodStep"))
				gs_draw_sprite(input_texture, 0, data->width, data->height);
			gs_texrender_end(data->buffer_b);
		}

		gs_texrender_t *tmp = data->buffer_a;
		data->buffer_a = data->buffer_b;
		data->buffer_b = tmp;
	}
	gs_blend_state_pop();

	data->distance_field = data->buffer_a;
}

void render_jf_distance(stroke_filter_data_t *data)
{
	gs_effect_t *effect = data->effect_jump_flood_sdf;

	gs_texture_t *overlay_texture = gs_texrender_get_texture(data->input_texrender);

	gs_texture_t *field_texture = data->distance_field ? gs_texrender_get_texture(data->distance_field) : NULL;

	bool contours = data->stroke_position == STROKE_POSITION_INNER_CONTOUR ||
			data->stroke_position == STROKE_POSITION_OUTER_CONTOUR;

	if (!effect || !overlay_texture || !field_texture) {
		return;
	}

	// Float output so contour falloff gradients can be dithered on output
	// instead of being quantised (banded) here.
	data->output_texrender = create_or_reset_texrender_format(data->output_texrender, GS_RGBA16F);

	if (data->param_distance_field) {
		gs_effect_set_texture(data->param_distance_field, field_texture);
	}

	if (data->param_jump_flood_overlay) {
		gs_effect_set_texture(data->param_jump_flood_overlay, overlay_texture);
	}

	if (data->param_jump_flood_threshold) {
		gs_effect_set_float(data->param_jump_flood_threshold, data->jump_flood_threshold);
	}

	if (data->param_jump_flood_stroke_offset) {
		float offset = data->stroke_offset;
		gs_effect_set_float(data->param_jump_flood_stroke_offset, offset);
	}

	if (data->param_jump_flood_contour_offset) {
		float offset_pct = fmodf(-data->contour_offset, 100.0f) / 100.01f;
		offset_pct = offset_pct >= 0.0f ? offset_pct : 1.0f + offset_pct;
		float offset = offset_pct * (data->contour_spacing + data->stroke_size);
		gs_effect_set_float(data->param_jump_flood_contour_offset, offset);
	}

	if (data->param_jump_flood_contour_spacing) {
		gs_effect_set_float(data->param_jump_flood_contour_spacing, data->contour_spacing);
	}

	if (data->param_jump_flood_stroke_extent) {
		float stroke_extent = contours ? data->stroke_size : data->stroke_offset + data->stroke_size;
		gs_effect_set_float(data->param_jump_flood_stroke_extent, stroke_extent);
	}
	if (data->param_jump_flood_contour_falloff_start) {
		gs_effect_set_float(data->param_jump_flood_contour_falloff_start, data->contour_falloff_start);
	}
	if (data->param_jump_flood_contour_falloff_end) {
		gs_effect_set_float(data->param_jump_flood_contour_falloff_end, data->contour_falloff_end);
	}
	if (data->param_jump_flood_contour_spacing_power) {
		gs_effect_set_float(data->param_jump_flood_contour_spacing_power, data->contour_spacing_power);
	}
	if (data->param_infill) {
		float fill = data->fill ? 1.0f : 0.0f;
		gs_effect_set_float(data->param_infill, fill);
	}

	// Render the fill source. If it is missing (not found, removed or no
	// size), fall back to a transparent color fill rather than skipping the
	// frame.
	gs_texture_t *fill_texture = NULL;
	if (data->fill_type == STROKE_FILL_TYPE_SOURCE) {
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
	} else if (data->fill_type == STROKE_FILL_TYPE_GRADIENT) {
		// Float, so the gradient is only quantised (and dithered) on
		// output.
		fill_texture = gradient_render_fill(&data->gradient, data->width, data->height);
	}
	const bool source_available = fill_texture != NULL;

	if (source_available) {
		if (data->param_jump_flood_fill_source) {
			gs_effect_set_texture(data->param_jump_flood_fill_source, fill_texture);
		}
	} else if (data->fill_type != STROKE_FILL_TYPE_COLOR) {
		if (data->param_jump_flood_stroke_color) {
			struct vec4 clear_color;
			vec4_zero(&clear_color);
			gs_effect_set_vec4(data->param_jump_flood_stroke_color, &clear_color);
		}
	} else if (data->fill_type == STROKE_FILL_TYPE_COLOR) {
		if (data->param_jump_flood_stroke_color) {
			gs_effect_set_vec4(data->param_jump_flood_stroke_color, &data->stroke_color);
		}
	}

	bool fill_color = (data->fill_type == STROKE_FILL_TYPE_COLOR) || !source_available;

	if (data->param_stroke_uv_size) {
		struct vec2 uv_size;
		uv_size.x = (float)data->width;
		uv_size.y = (float)data->height;
		gs_effect_set_vec2(data->param_stroke_uv_size, &uv_size);
	}

	stroke_set_blending_parameters();

	const char *render = data->stroke_position == STROKE_POSITION_OUTER ||
					     data->stroke_position == STROKE_POSITION_INNER
				     ? "Stroke"
				     : "Contour";
	const char *innieOutie = data->stroke_position == STROKE_POSITION_OUTER ||
						 data->stroke_position == STROKE_POSITION_OUTER_CONTOUR
					 ? ""
					 : "Inner";

	const char *fill_type = fill_color ? "Color" : "Source";

	char shader_id[100] = "";
	snprintf(shader_id, sizeof(shader_id), "Draw%s%s%s", render, innieOutie, fill_type);

	if (gs_texrender_begin(data->output_texrender, data->width, data->height)) {
		gs_ortho(0.0f, (float)data->width, 0.0f, (float)data->height, -100.0f, 100.0f);
		while (gs_effect_loop(effect, shader_id))
			gs_draw_sprite(field_texture, 0, data->width, data->height);
		gs_texrender_end(data->output_texrender);
	}
	gs_blend_state_pop();
}
