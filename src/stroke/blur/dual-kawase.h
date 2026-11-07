#pragma once

#include <math.h>
#include <obs-module.h>
#include "../obs-utils.h"
#include "alpha-blur.h"

extern void stroke_load_effect_dual_kawase(alpha_blur_data_t *filter);

extern void dual_kawase_blur(int radius, bool ignore_border, alpha_blur_data_t *data, gs_texrender_t *input);
