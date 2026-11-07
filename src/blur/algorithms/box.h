#pragma once

#include <math.h>
#include <obs-module.h>
#include "../obs-utils.h"
#include "../blur-filter.h"

#define MIN_BOX_BLUR_RADIUS 0.01f

extern void set_box_blur_types(obs_properties_t *props);
extern void box_setup_callbacks(blur_filter_data_t *data);
extern void render_video_box(blur_filter_data_t *data);
extern void load_effect_box(blur_filter_data_t *filter);
