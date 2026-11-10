#pragma once

#include <obs-module.h>
#include <util/base.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>

#include "obs-utils.h"

#define ALGO_NONE 0
#define ALGO_NONE_LABEL "None"
#define ALGO_GAUSSIAN 1
#define ALGO_GAUSSIAN_LABEL "BlurFilter.Algorithm.Gaussian"
#define ALGO_BOX 2
#define ALGO_BOX_LABEL "BlurFilter.Algorithm.Box"
#define ALGO_DUAL_KAWASE 3
#define ALGO_DUAL_KAWASE_LABEL "BlurFilter.Algorithm.DualKawase"
#define ALGO_PIXELATE 4
#define ALGO_PIXELATE_LABEL "BlurFilter.Algorithm.Pixelate"
#define ALGO_TEMPORAL 5
#define ALGO_TEMPORAL_LABEL "BlurFilter.Algorithm.Temporal"

#define TYPE_NONE 0
#define TYPE_NONE_LABEL "None"
#define TYPE_AREA 1
#define TYPE_AREA_LABEL "BlurFilter.Type.Area"
#define TYPE_DIRECTIONAL 2
#define TYPE_DIRECTIONAL_LABEL "BlurFilter.Type.Directional"
#define TYPE_ZOOM 3
#define TYPE_ZOOM_LABEL "BlurFilter.Type.Zoom"
#define TYPE_MOTION 4
#define TYPE_MOTION_LABEL "BlurFilter.Type.Motion"
#define TYPE_TILTSHIFT 5
#define TYPE_TILTSHIFT_LABEL "BlurFilter.Type.TiltShift"
#define TYPE_VECTOR 6
#define TYPE_VECTOR_LABEL "BlurFilter.Type.Vector"

#define PIXELATE_TYPE_SQUARE 0
#define PIXELATE_TYPE_SQUARE_LABEL "BlurFilter.Pixelate.Square"
#define PIXELATE_TYPE_HEXAGONAL 1
#define PIXELATE_TYPE_HEXAGONAL_LABEL "BlurFilter.Pixelate.Hexagonal"
#define PIXELATE_TYPE_TRIAKIS 2
#define PIXELATE_TYPE_TRIAKIS_LABEL "BlurFilter.Pixelate.Triakis"
#define PIXELATE_TYPE_CIRCLE 3
#define PIXELATE_TYPE_CIRCLE_LABEL "BlurFilter.Pixelate.Circle"
#define PIXELATE_TYPE_TRIANGLE 4
#define PIXELATE_TYPE_TRIANGLE_LABEL "BlurFilter.Pixelate.Triangle"
#define PIXELATE_TYPE_VORONOI 5
#define PIXELATE_TYPE_VORONOI_LABEL "BlurFilter.Pixelate.Voronoi"
#define PIXELATE_TYPE_RHOMBOID 6
#define PIXELATE_TYPE_RHOMBOID_LABEL "BlurFilter.Pixelate.Rhomboid"

#define EFFECT_MASK_TYPE_NONE 0
#define EFFECT_MASK_TYPE_NONE_LABEL "BlurFilter.EffectMask.None"
#define EFFECT_MASK_TYPE_CROP 1
#define EFFECT_MASK_TYPE_CROP_LABEL "BlurFilter.EffectMask.Crop"
#define EFFECT_MASK_TYPE_RECT 2
#define EFFECT_MASK_TYPE_RECT_LABEL "BlurFilter.EffectMask.Rectangle"
#define EFFECT_MASK_TYPE_CIRCLE 3
#define EFFECT_MASK_TYPE_CIRCLE_LABEL "BlurFilter.EffectMask.Circle"
#define EFFECT_MASK_TYPE_SOURCE 4
#define EFFECT_MASK_TYPE_SOURCE_LABEL "BlurFilter.EffectMask.Source"
#define EFFECT_MASK_TYPE_IMAGE 5
#define EFFECT_MASK_TYPE_IMAGE_LABEL "BlurFilter.EffectMask.Image"

#define EFFECT_MASK_SOURCE_FILTER_ALPHA 0
#define EFFECT_MASK_SOURCE_FILTER_ALPHA_LABEL \
	"BlurFilter.EffectMask.Source.Alpha"
#define EFFECT_MASK_SOURCE_FILTER_GRAYSCALE 1
#define EFFECT_MASK_SOURCE_FILTER_GRAYSCALE_LABEL \
	"BlurFilter.EffectMask.Source.Grayscale"
#define EFFECT_MASK_SOURCE_FILTER_LUMINOSITY 2
#define EFFECT_MASK_SOURCE_FILTER_LUMINOSITY_LABEL \
	"BlurFilter.EffectMask.Source.Luminosity"
#define EFFECT_MASK_SOURCE_FILTER_SLIDERS 3
#define EFFECT_MASK_SOURCE_FILTER_SLIDERS_LABEL \
	"BlurFilter.EffectMask.Source.Sliders"

#define GRADIENT_TYPE_SOBEL 0
#define GRADIENT_TYPE_SOBEL_LABEL \
	"BlurFilter.VectorBlur.GradientType.Sobel"
#define GRADIENT_TYPE_FORWARD_DIFF 1
#define GRADIENT_TYPE_FORWARD_DIFF_LABEL \
	"BlurFilter.VectorBlur.GradientType.ForwardDifference"
#define GRADIENT_TYPE_CENTRAL_LIMIT_DIFF 2
#define GRADIENT_TYPE_CENTRAL_LIMIT_DIFF_LABEL \
	"BlurFilter.VectorBlur.GradientType.CentralLimitDifference"

#define GRADIENT_CHANNEL_RED 0
#define GRADIENT_CHANNEL_RED_LABEL "BlurFilter.VectorBlur.Channel.Red"
#define GRADIENT_CHANNEL_GREEN 1
#define GRADIENT_CHANNEL_GREEN_LABEL \
	"BlurFilter.VectorBlur.Channel.Green"
#define GRADIENT_CHANNEL_BLUE 2
#define GRADIENT_CHANNEL_BLUE_LABEL \
	"BlurFilter.VectorBlur.Channel.Blue"
#define GRADIENT_CHANNEL_ALPHA 3
#define GRADIENT_CHANNEL_ALPHA_LABEL \
	"BlurFilter.VectorBlur.Channel.Alpha"
#define GRADIENT_CHANNEL_LUMINANCE 4
#define GRADIENT_CHANNEL_LUMINANCE_LABEL \
	"BlurFilter.VectorBlur.Channel.Luminance"
#define GRADIENT_CHANNEL_SATURATION 5
#define GRADIENT_CHANNEL_SATURATION_LABEL \
	"BlurFilter.VectorBlur.Channel.Saturation"

typedef DARRAY(float) fDarray;

struct blur_filter_data;
typedef struct blur_filter_data blur_filter_data_t;

// A source referenced by name from the filter settings (background, mask,
// vector map). `name` and `weak` are written from the UI thread (update) and
// read from the graphics thread (tick/render); every access must hold the
// filter's sources_mutex. Use the external_source_* helpers.
typedef struct external_source {
	struct dstr name;
	obs_weak_source_t *weak;
} external_source_t;

struct blur_filter_data {
	obs_source_t *context;
	struct dstr filter_name;

	// Effects
	gs_effect_t *effect;
	gs_effect_t *effect_2;
	gs_effect_t *composite_effect;
	gs_effect_t *pixelate_effect;
	gs_effect_t *mix_effect;
	gs_effect_t *effect_mask_effect;
	gs_effect_t *output_effect;
	gs_effect_t *gradient_effect;
	gs_effect_t *gv_effect;

	// Color space/format of the render pipeline for the current frame.
	// Determined from the filter target in get_input_source.
	enum gs_color_space color_space;
	enum gs_color_format color_format;

	// Render pipeline
	gs_texrender_t *input_texrender;
	gs_texrender_t *output_texrender;
	// Frame Buffers
	gs_texrender_t *render;
	gs_texrender_t *render2;
	gs_texrender_t *background_texrender;
	// Renderer for composite render step
	gs_texrender_t *composite_render;
	// Renderer for an external mask source
	gs_texrender_t *mask_source_texrender;
	// Extra buffer used by dual kawase for the residual pass
	gs_texrender_t *kawase_base_texrender;
	// Temporal blur history (previous output)
	gs_texrender_t *temporal_history;

	// Renderers for vector blur
	gs_texrender_t *vb_gradient;
	gs_texrender_t *vb_smoothed_gradient;
	gs_texrender_t *vector_source_texrender;

	obs_hotkey_pair_id hotkey;

	bool rendering;
	bool reload;
	bool rendered;

	float time;

	// Blur Filter Common
	int blur_algorithm;
	int blur_algorithm_last;
	int blur_type;
	int blur_type_last;

	gs_eparam_t *param_uv_size;
	struct vec2 uv_size;
	gs_eparam_t *param_radius;
	float radius;
	float inactive_radius;
	gs_eparam_t *param_texel_step;
	struct vec2 texel_step;

	// Gaussian Blur
	gs_eparam_t *param_kernel_size;
	size_t kernel_size;
	gs_eparam_t *param_offset;
	fDarray offset;
	gs_eparam_t *param_weight;
	fDarray kernel;
	gs_eparam_t *param_kernel_texture;
	gs_texture_t *kernel_texture;
	// Radius the current gaussian kernel was sampled for (-1 = none).
	float kernel_radius_last;
	gs_eparam_t *param_gradient_image;
	gs_eparam_t *param_gradient_channel;
	gs_eparam_t *param_gradient_uv_size;
	gs_eparam_t *param_gradient_map;
	int vector_blur_channel;
	float vector_blur_amount;
	float vector_blur_smoothing;
	int vector_blur_type;
	external_source_t vector_source;
	gs_eparam_t *param_inactive_radius;

	// Box Blur
	int passes;

	// Kawase Blur
	float kawase_passes;

	// Pixelate Blur
	int pixelate_type;
	int pixelate_type_last;
	struct vec2 pixelate_tessel_center;
	float pixelate_smoothing_pct;
	float pixelate_cos_theta;
	float pixelate_sin_theta;
	float pixelate_cos_rtheta;
	float pixelate_sin_rtheta;
	bool pixelate_animate;
	float pixelate_animation_speed;
	float pixelate_animation_time;
	gs_eparam_t *param_pixel_size;
	gs_eparam_t *param_pixel_center;
	gs_eparam_t *param_pixel_rot;
	gs_eparam_t *param_pixel_cos_theta;
	gs_eparam_t *param_pixel_sin_theta;
	gs_eparam_t *param_pixel_cos_rtheta;
	gs_eparam_t *param_pixel_sin_rtheta;
	gs_eparam_t *param_pixel_time;
	gs_texrender_t *pixelate_texrender;

	// Radial Blur
	gs_eparam_t *param_radial_center;
	float center_x;
	float center_y;
	// True when the stored center/origin settings still hold the
	// "uninitialized" sentinel and should follow the source center.
	bool center_uninitialized;
	bool center_init_queued;

	// Motion/Directional Blur
	float angle;

	// Tilt-Shift
	gs_eparam_t *param_focus_width;
	float tilt_shift_width;
	gs_eparam_t *param_focus_center;
	float tilt_shift_center;
	gs_eparam_t *param_focus_angle;
	float tilt_shift_angle;

	// Temporal
	gs_eparam_t *param_temporal_image;
	gs_eparam_t *param_temporal_prior_image;
	gs_eparam_t *param_temporal_current_weight;
	gs_eparam_t *param_temporal_clear_threshold;
	float temporal_current_weight;
	float temporal_clear_threshold;
	bool temporal_prior_stored;

	// Guards every external_source_t in this struct.
	pthread_mutex_t sources_mutex;
	// Seconds until we next retry resolving a named source that was not
	// found (e.g. not yet loaded or since deleted).
	float source_retry_timer;

	// Compositing
	gs_eparam_t *param_background;
	gs_eparam_t *param_composite_image;
	external_source_t background;

	// Mask
	int mask_type;
	int mask_type_last;
	gs_eparam_t *param_mask_image;
	gs_eparam_t *param_filtered_image;
	float mask_crop_left;
	float mask_crop_right;
	float mask_crop_top;
	float mask_crop_bot;
	gs_eparam_t *param_mask_crop_scale;
	gs_eparam_t *param_mask_crop_offset;
	gs_eparam_t *param_mask_crop_box_aspect_ratio;
	gs_eparam_t *param_mask_crop_corner_radius;
	float mask_crop_corner_radius;
	gs_eparam_t *param_mask_crop_feathering;
	float mask_crop_feathering;
	gs_eparam_t *param_mask_crop_invert;
	bool mask_crop_invert;
	external_source_t mask_source;
	int mask_source_filter_type;
	struct vec4 mask_source_weights;
	gs_eparam_t *param_mask_source_alpha_source;
	gs_eparam_t *param_mask_source_rgba_weights;
	gs_eparam_t *param_mask_source_multiplier;
	float mask_source_multiplier;
	gs_eparam_t *param_mask_source_invert;
	bool mask_source_invert;
	gs_eparam_t *param_mask_circle_center;
	float mask_circle_center_x;
	float mask_circle_center_y;
	gs_eparam_t *param_mask_circle_radius;
	float mask_circle_radius;
	gs_eparam_t *param_mask_circle_feathering;
	float mask_circle_feathering;
	gs_eparam_t *param_mask_circle_inv;
	bool mask_circle_inv;
	gs_eparam_t *param_mask_circle_uv_scale;
	float mask_rect_center_x;
	float mask_rect_center_y;
	float mask_rect_width;
	float mask_rect_height;
	float mask_rect_corner_radius;
	float mask_rect_feathering;
	bool mask_rect_inv;

	// Image mask. Decoded off the graphics thread, uploaded under the
	// graphics lock. Loaded with gs_create_texture_file_data() rather than
	// the gs_image_file_* API, which was removed in OBS Studio 33.
	gs_texture_t *mask_image;
	struct dstr mask_image_path;

	// Output Effect Parameters
	gs_eparam_t *param_output_image;

	uint32_t width;
	uint32_t height;

	uint32_t device_type;

	// Callback Functions
	void (*video_render)(blur_filter_data_t *filter);
	void (*load_effect)(blur_filter_data_t *filter);
	void (*update)(blur_filter_data_t *filter);
};

// Shared helpers used by the individual blur algorithms.
extern gs_texture_t *blend_composite(gs_texture_t *texture, blur_filter_data_t *data);

// Returns a strong reference to the external source (release with
// obs_source_release), or NULL if it is not set or no longer exists.
extern obs_source_t *external_source_get(blur_filter_data_t *filter, external_source_t *src);

// Renders `source` into *texrender (created/reset as needed) in the given
// color space and returns the resulting texture, or NULL.
extern gs_texture_t *render_source_to_texrender(obs_source_t *source, gs_texrender_t **texrender,
						enum gs_color_space space);

extern float (*move_get_transition_filter)(obs_source_t *filter_from, obs_source_t **filter_to);
