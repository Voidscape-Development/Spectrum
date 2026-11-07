#pragma once

// A small framework for Spectrum's single-pass effects (filters and
// generator sources) written against data/shaders/fx/fx-common.effect.
//
// Settings are bound to shader uniforms by name: every uniform whose name
// matches a settings key gets that setting (floats, ints, bools, and colors
// as sRGB encoded straight alpha float4). During a Move Transition between
// two filters of the same kind, float and color uniforms are interpolated.
//
// The framework also sets the shared uniforms of fx-common.effect
// (uv_size, linear_input, dither_step, time and the gradient lookup when the
// effect uses a gradient).

#include <obs-module.h>
#include <util/threading.h>

#include "../gradient/gradient.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fx_filter fx_filter_t;

struct fx_filter_def {
	// Relative to the module data directory.
	const char *effect_file;
	// Settings prefix of a gradient (GRADIENT_PROPS_MAP style), or NULL.
	const char *gradient_prefix;
	// Name of the setting that turns dithering on, or NULL (always on).
	const char *dither_key;
	// Technique to draw with; NULL means "Draw".
	const char *(*technique)(obs_data_t *settings);
	// Extra parameters, after the automatic ones. `to` is the Move
	// Transition target's settings (or NULL) and `t` its progress.
	void (*set_params)(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, obs_data_t *to, float t,
			   uint32_t width, uint32_t height);
	// Extra margin the effect draws outside the source, in px (filters).
	uint32_t (*padding)(obs_data_t *settings);
	// Optional lifetime hooks for the effect's own data (fx->user).
	void (*create)(fx_filter_t *fx, obs_data_t *settings);
	void (*update)(fx_filter_t *fx, obs_data_t *settings);
	void (*tick)(fx_filter_t *fx, float seconds);
	void (*destroy)(fx_filter_t *fx);
};

struct fx_filter {
	obs_source_t *context;
	const struct fx_filter_def *def;
	bool is_filter;

	gs_effect_t *effect;
	bool load_failed;

	pthread_mutex_t mutex;
	obs_data_t *snapshot; // settings with defaults, for the render thread

	bool has_gradient;
	gradient_t gradient;

	float time;
	uint32_t width, height; // generator sources: from "width"/"height"
	void *user;             // for the filter's own data
};

fx_filter_t *fx_filter_create(const struct fx_filter_def *def, obs_data_t *settings, obs_source_t *context);
void fx_filter_destroy(fx_filter_t *fx);
void fx_filter_update(fx_filter_t *fx, obs_data_t *settings);
void fx_filter_tick(fx_filter_t *fx, float seconds);
void fx_filter_render(fx_filter_t *fx);

// For effects with their own render pipeline: loads the effect (graphics
// thread), and sets the shared and settings-bound uniforms.
bool fx_filter_ensure_effect(fx_filter_t *fx);
void fx_filter_set_common(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, obs_data_t *to, float t,
			  uint32_t width, uint32_t height, bool linear, float pad);

// A new reference to the current settings snapshot (release it).
obs_data_t *fx_filter_settings(fx_filter_t *fx);

// Move Transition filter ids that should be offered the interpolation hook
// (called from plugin-main.c).
void fx_filter_register_move_id(const char *id);
size_t fx_filter_move_id_count(void);
const char *fx_filter_move_id(size_t index);

// Generic obs_source_info callbacks (data is the fx_filter_t).
void fx_cb_destroy(void *data);
void fx_cb_update(void *data, obs_data_t *settings);
void fx_cb_tick(void *data, float seconds);
void fx_cb_render(void *data, gs_effect_t *effect);
uint32_t fx_cb_width(void *data);
uint32_t fx_cb_height(void *data);
// "width"/"height" settings of generator sources (defaults: canvas size).
void fx_add_size_properties(obs_properties_t *props);
void fx_size_defaults(obs_data_t *settings);

// A blend mode list matching blend_rgb() in fx-common.effect.
void fx_add_blend_list(obs_properties_t *props, const char *key);

// Move Transition support for filters with their own pipeline: the
// counterpart filter of the same kind and the progress (0 when none).
float fx_move_target(obs_source_t *filter, obs_source_t **target);
// Settings interpolated from `from` to `to` (with defaults): numbers and
// colors (keys containing "color") blend, everything else switches half
// way. Returns a new obs_data_t (release it).
obs_data_t *fx_lerp_settings(obs_data_t *from, obs_data_t *to, float t);

// Helpers for set_params callbacks.
void fx_set_float(gs_effect_t *effect, const char *name, float value);
void fx_set_int(gs_effect_t *effect, const char *name, int value);
void fx_set_vec2(gs_effect_t *effect, const char *name, float x, float y);
void fx_set_vec4(gs_effect_t *effect, const char *name, float x, float y, float z, float w);
void fx_set_texture(gs_effect_t *effect, const char *name, gs_texture_t *texture);
// A float setting, interpolated towards `to` by t.
float fx_lerp_setting(obs_data_t *settings, obs_data_t *to, float t, const char *key);

#ifdef __cplusplus
}
#endif
