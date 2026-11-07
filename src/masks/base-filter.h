#pragma once

#include <obs-module.h>

#define MASK_EFFECT_ALPHA 1
#define MASK_EFFECT_ALPHA_LABEL "MaskFilter.Effects.Alpha"
#define MASK_EFFECT_ADJUSTMENT 2
#define MASK_EFFECT_ADJUSTMENT_LABEL "MaskFilter.Effects.Adjustment"

#define MASK_TYPE_SHAPE 1
#define MASK_TYPE_SHAPE_LABEL "MaskFilter.Shape"
#define MASK_TYPE_SOURCE 2
#define MASK_TYPE_SOURCE_LABEL "MaskFilter.Source"
#define MASK_TYPE_IMAGE 3
#define MASK_TYPE_IMAGE_LABEL "MaskFilter.Image"
#define MASK_TYPE_GRADIENT 4
#define MASK_TYPE_GRADIENT_LABEL "MaskFilter.Gradient"
#define MASK_TYPE_BSM 5
#define MASK_TYPE_BSM_LABEL "MaskFilter.BSM"
#define MASK_TYPE_CHROMA_KEY 6
#define MASK_TYPE_CHROMA_KEY_LABEL "MaskFilter.ChromaKey"
#define MASK_TYPE_FEATHER 7
#define MASK_TYPE_FEATHER_LABEL "MaskFilter.FeatherMask"
#define MASK_TYPE_SVG 8
#define MASK_TYPE_SVG_LABEL "MaskFilter.SvgMask"
#define MASK_TYPE_FONT_AWESOME 9
#define MASK_TYPE_FONT_AWESOME_LABEL "MaskFilter.FontAwesome"

#ifdef __cplusplus
extern "C" {
#endif
struct base_filter_data;
typedef struct base_filter_data base_filter_data_t;

struct base_filter_data {
	obs_source_t *context;

	bool input_texture_generated;
	gs_texrender_t *input_texrender;
	bool output_rendered;
	gs_texrender_t *output_texrender;
	gs_effect_t *output_effect;
	gs_eparam_t *param_output_image;

	bool rendered;
	bool rendering;

	uint32_t width;
	uint32_t height;

	uint32_t mask_effect;
	uint32_t mask_type;
};
#ifdef __cplusplus
}
#endif
