#pragma once

#include "stroke-types.h"
#include "obs-utils.h"

void load_jump_flood_sdf_effect(stroke_filter_data_t *filter);
void render_padded_input(stroke_filter_data_t *data);
void render_cropped_output(stroke_filter_data_t *data);
void render_jf_field(stroke_filter_data_t *data, float extent);
void render_jf_arc(stroke_filter_data_t *data);
void render_jf_distance(stroke_filter_data_t *data);
