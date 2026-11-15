#pragma once

#include <math.h>
#include <obs-module.h>
#include "../obs-utils.h"
#include "../blur-filter.h"

#define MIN_LENS_BLUR_RADIUS 0.5f

extern void set_lens_blur_types(obs_properties_t *props);
extern void lens_setup_callbacks(blur_filter_data_t *data);
extern void render_video_lens(blur_filter_data_t *data);
extern void load_effect_lens(blur_filter_data_t *filter);

/* Helpers shared by the lens and frosted glass algorithms. */

// Move Transition state for this filter. Returns the filter being
// transitioned to when it uses the same algorithm and blur type (lerp
// settings towards it with *t), else NULL; *fade then scales the blur
// strength the same way the other algorithms fade between unrelated
// states. With no transition: NULL, *t = 0, *fade = 1.
extern blur_filter_data_t *blur_move_peer(blur_filter_data_t *data, float *t, float *fade);

static inline float blur_lerpf(float a, float b, float t)
{
	return a + (b - a) * t;
}

extern void blur_fx_float(gs_effect_t *effect, const char *name, float value);
extern void blur_fx_vec2(gs_effect_t *effect, const char *name, float x, float y);
extern void blur_fx_vec4(gs_effect_t *effect, const char *name, const struct vec4 *value);
extern void blur_fx_texture(gs_effect_t *effect, const char *name, gs_texture_t *texture);

// Binds the uniforms of linear_common.effect for the current pipeline.
extern void blur_fx_set_linear_common(blur_filter_data_t *data, gs_effect_t *effect, uint32_t width, uint32_t height);

// Draws `technique` of `effect` as a full-target sprite into *texrender
// (created/reset in `format`). Returns the resulting texture or NULL.
extern gs_texture_t *blur_fx_draw(gs_effect_t *effect, const char *technique, gs_texrender_t **texrender,
				  enum gs_color_format format, uint32_t width, uint32_t height);

// Copies the input straight to the output (blur strength zero).
extern void blur_passthrough(blur_filter_data_t *data, gs_texture_t *texture);
