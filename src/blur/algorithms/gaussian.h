#pragma once

#include <math.h>
#include <obs-module.h>
#include "../obs-utils.h"
#include "../blur-filter.h"
#include "gaussian-kernel.h"

#define MIN_GAUSSIAN_BLUR_RADIUS 0.01f

extern void set_gaussian_blur_types(obs_properties_t *props);
extern void gaussian_setup_callbacks(blur_filter_data_t *data);
extern void render_video_gaussian(blur_filter_data_t *data);
extern void load_effect_gaussian(blur_filter_data_t *filter);
extern void update_gaussian(blur_filter_data_t *data);

extern bool vector_channel_modified(void *data, obs_properties_t *props, obs_property_t *p, obs_data_t *settings);
