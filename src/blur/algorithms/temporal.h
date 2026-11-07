#pragma once

#include <obs-module.h>
#include "../obs-utils.h"
#include "../blur-filter.h"

extern void temporal_setup_callbacks(blur_filter_data_t *data);
extern void render_video_temporal(blur_filter_data_t *data);
extern void load_effect_temporal(blur_filter_data_t *filter);
