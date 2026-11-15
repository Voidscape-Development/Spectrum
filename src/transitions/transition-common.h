#pragma once

// Shared code for Spectrum's scene transitions.
//
// A transition is an fx_filter_t (see src/common/fx-filter.h), so settings
// bind to shader uniforms by name and the gradient engine is available as
// an edge glow fill. The shaders include
// data/shaders/transitions/transition-common.effect and receive:
//   tex_a, tex_b   the outgoing and incoming scenes, sampled in linear
//                  light (premultiplied alpha)
//   progress       eased progress, 0 = all A, 1 = all B
//   raw_progress   linear progress
// and output linear, premultiplied color (the framebuffer encodes it),
// dithered to the target's bit depth.

#include "../common/fx-filter.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TR_EDGE_PREFIX "tr_edge_gradient_"

struct tr_def {
	struct fx_filter_def fx; // must be first
	// Custom render pipeline; NULL draws the effect's "Draw" technique in
	// one pass. `progress` is eased. Called with the sRGB framebuffer on.
	void (*render)(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, gs_texture_t *a, gs_texture_t *b,
		       float progress, uint32_t cx, uint32_t cy);
};

// Easing of the transition progress (setting "tr_easing").
enum tr_easing {
	TR_EASE_LINEAR = 1,
	TR_EASE_SMOOTH,
	TR_EASE_IN_OUT,
	TR_EASE_IN,
	TR_EASE_OUT,
};

float tr_ease(int easing, float t);

// Sets tex_a / tex_b (linear light), progress and raw_progress, plus the
// common and settings-bound uniforms.
void tr_set_common(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, gs_texture_t *a, gs_texture_t *b,
		   float progress, float raw, uint32_t cx, uint32_t cy);

// obs_source_info callbacks shared by every transition (data is the
// fx_filter_t, created with tr_create()).
void *tr_create(const struct tr_def *def, obs_data_t *settings, obs_source_t *source);
void tr_video_render(void *data, gs_effect_t *effect);
bool tr_audio_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers, size_t channels,
		     size_t sample_rate);
enum gs_color_space tr_video_get_color_space(void *data, size_t count, const enum gs_color_space *preferred_spaces);

// Common settings: easing first, the edge glow group, presets last.
void tr_defaults_common(obs_data_t *settings);
void tr_add_easing_property(obs_properties_t *props);
// Edge glow/border settings (tr_edge_*), filled with a color or the
// gradient engine. `width_suffix` is the unit of tr_edge_width ("px" or "%").
void tr_edge_defaults(obs_data_t *settings, int mode, double width, double intensity);
void tr_add_edge_properties(obs_properties_t *props, fx_filter_t *fx, const char *width_suffix, double max_width);
void tr_finish_properties(obs_properties_t *props, void *data);

obs_property_t *tr_slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			  double step, const char *suffix);

#ifdef __cplusplus
}
#endif
