#pragma once

#include <math.h>
#include <obs-module.h>
#include "../obs-utils.h"
#include "../blur-filter.h"

extern void set_frosted_blur_types(obs_properties_t *props);
extern void frosted_setup_callbacks(blur_filter_data_t *data);
extern void render_video_frosted(blur_filter_data_t *data);
extern void load_effect_frosted(blur_filter_data_t *filter);
