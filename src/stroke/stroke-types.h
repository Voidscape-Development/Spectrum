#pragma once

#include <obs-module.h>
#include "defines.h"
#include <util/dstr.h>
#include "obs-utils.h"

#define DEFAULT_COLOR 4294967295

#define STROKE_FILL_TYPE_COLOR_LABEL "StrokeFilter.ColorFill"
#define STROKE_FILL_TYPE_SOURCE_LABEL "StrokeFilter.SourceFill"
#define STROKE_FILL_TYPE_IMAGE_LABEL "StrokeFilter.ImageFill"

enum stroke_fill_type {
	STROKE_FILL_TYPE_COLOR = 1,
	STROKE_FILL_TYPE_SOURCE,
	STROKE_FILL_TYPE_IMAGE,
};

#define STROKE_POSITION_OUTER_LABEL "StrokeFilter.StrokePositionOuter"
#define STROKE_POSITION_INNER_LABEL "StrokeFilter.StrokePositionInner"
#define STROKE_POSITION_OUTER_CONTOUR_LABEL \
	"StrokeFilter.StrokePositionOuterContour"
#define STROKE_POSITION_INNER_CONTOUR_LABEL \
	"StrokeFilter.StrokePositionInnerContour"

enum stroke_position {
	STROKE_POSITION_OUTER = 1,
	STROKE_POSITION_INNER,
	STROKE_POSITION_OUTER_CONTOUR,
	STROKE_POSITION_INNER_CONTOUR
};

struct stroke_filter_data;
typedef struct stroke_filter_data stroke_filter_data_t;

struct stroke_filter_data {
	obs_source_t *context;

	// The source to stroke (source version only), and the fill source.
	struct source_ref input_ref;
	struct source_ref fill_ref;
	// Cached render targets for the two sources above.
	gs_texrender_t *source_texrender;
	gs_texrender_t *fill_texrender;

	bool is_filter;
	bool is_source;

	// Effects
	gs_effect_t *effect_jump_flood_sdf;

	// Render pipeline
	bool input_rendered;
	gs_texrender_t *input_texrender;
	bool output_rendered;
	gs_texrender_t *output_texrender;
	// Frame Buffers
	// Jump flood ping-pong buffers (RG32F nearest edge point per pixel).
	gs_texrender_t *buffer_a;
	gs_texrender_t *buffer_b;
	// The finished distance field: one of the two buffers above, not owned.
	gs_texrender_t *distance_field;

	bool input_texture_generated;
	bool rendering;
	bool reload;
	bool rendered;

	uint32_t width;
	uint32_t height;
	// Color space of the current frame (see filter_working_space).
	enum gs_color_space working_space;

	// Parameters
	float stroke_size;
	float stroke_offset;
	float stroke_distance;
	float contour_spacing;
	float contour_offset;
	float contour_falloff_start;
	float contour_falloff_end;
	float contour_spacing_power;

	uint32_t pad_t;
	uint32_t pad_b;
	uint32_t pad_l;
	uint32_t pad_r;

	uint32_t padding_amount;
	// Draw the padding outside the source's bounds instead of growing the
	// source's reported size.
	bool overflow;

	uint32_t source_width;
	uint32_t source_height;

	uint32_t output_width;
	uint32_t output_height;

	bool ignore_source_border;
	bool fill;

	float jump_flood_threshold;

	struct vec4 stroke_color;

	enum stroke_fill_type fill_type;

	enum stroke_position stroke_position;

	struct vec2 mul_val;
	struct vec2 add_val;

	gs_eparam_t *param_jump_flood_threshold;
	gs_eparam_t *param_stroke_uv_size;
	gs_eparam_t *param_jump_flood_offset;
	gs_eparam_t *param_jump_flood_stroke_offset;
	gs_eparam_t *param_jump_flood_stroke_extent;
	gs_eparam_t *param_jump_flood_overlay;
	gs_eparam_t *param_jump_flood_stroke_color;
	gs_eparam_t *param_jump_flood_fill_source;
	gs_eparam_t *param_jump_flood_contour_offset;
	gs_eparam_t *param_jump_flood_contour_spacing;
	gs_eparam_t *param_jump_flood_contour_falloff_start;
	gs_eparam_t *param_jump_flood_contour_falloff_end;
	gs_eparam_t *param_jump_flood_contour_spacing_power;
	gs_eparam_t *param_distance_field;
	gs_eparam_t *param_infill;

	gs_eparam_t *param_mul_val;
	gs_eparam_t *param_add_val;

	// Callback Functions
	void (*video_render)(stroke_filter_data_t *filter);
	void (*load_effect)(stroke_filter_data_t *filter);
	void (*update)(stroke_filter_data_t *filter);
};
