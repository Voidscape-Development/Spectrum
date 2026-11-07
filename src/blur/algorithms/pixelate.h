#pragma once

#include <math.h>
#include <obs-module.h>
#include "../obs-utils.h"
#include "../blur-filter.h"

#define MIN_PIXELATE_BLUR_SIZE 1.01f

extern void set_pixelate_blur_types(obs_properties_t *props);
extern void pixelate_setup_callbacks(blur_filter_data_t *data);
extern void render_video_pixelate(blur_filter_data_t *data);
extern void load_effect_pixelate(blur_filter_data_t *filter);
