#pragma once

#include <obs-module.h>
#include "base-filter.h"
#include "color-adjustments.h"
#include "mask-source.h"
#include "mask-shape.h"
#include "mask-gradient.h"
#include "mask-bsm.h"
#include "mask-chroma-key.h"
#include "mask-feather.h"
#include "mask-svg.h"

struct mask_filter_data;
typedef struct mask_filter_data mask_filter_data_t;

struct mask_filter_data {
	base_filter_data_t *base;
	color_adjustments_data_t *color_adj_data;
	mask_source_data_t *source_data;
	mask_shape_data_t *shape_data;
	mask_gradient_data_t *gradient_data;
	mask_bsm_data_t *bsm_data;
	mask_chroma_key_data_t *chroma_key_data;
	mask_feather_data_t *feather_data;
	mask_svg_data_t *svg_data;
	void *font_awesome_data;

	bool invert;
	bool multiPassShader;
	bool source_size_set;
};
