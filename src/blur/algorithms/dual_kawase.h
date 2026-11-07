#pragma once

#include <math.h>
#include <obs-module.h>
#include "../obs-utils.h"
#include "../blur-filter.h"

extern void set_dual_kawase_blur_types(obs_properties_t *props);
extern void dual_kawase_setup_callbacks(blur_filter_data_t *data);
extern void render_video_dual_kawase(blur_filter_data_t *data);
extern void blur_load_effect_dual_kawase(blur_filter_data_t *filter);

// Blurs `texture` with the given number of dual kawase passes and writes the
// result to data->output_texrender. Uses data->render, data->render2 and
// data->kawase_base_texrender as scratch; never touches
// data->input_texrender. Does nothing if the effects are not loaded.
extern void dual_kawase_blur_texture(blur_filter_data_t *data, gs_texture_t *texture, float passes);
