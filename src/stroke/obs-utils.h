#pragma once
#include <obs-module.h>

#include <util/base.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>
#include <util/threading.h>

#include <stdio.h>

extern gs_texrender_t *stroke_create_or_reset_texrender(gs_texrender_t *render);
extern gs_texrender_t *stroke_create_or_reset_texrender_high(gs_texrender_t *render);
extern gs_texrender_t *create_or_reset_texrender_format(gs_texrender_t *render, enum gs_color_format format);
extern void stroke_set_blending_parameters();
extern bool stroke_add_source_to_list(void *data, obs_source_t *source);
gs_effect_t *stroke_load_shader_effect(gs_effect_t *effect, const char *effect_file_path);
extern char *stroke_load_shader_from_file(const char *file_name);
extern void stroke_setting_visibility(const char *prop_name, bool visible, obs_properties_t *props);
extern void stroke_texrender_set_texture(gs_texture_t *source, gs_texrender_t *dest);

// A source referenced by name from the settings (a fill source, or the input
// of a stroke/glow/shadow source).
//
// The settings only change the name: source_ref_set_name() may be called
// from any thread. The weak reference is resolved and used only on the
// graphics thread (source_ref_tick() from video_tick, source_ref_get() from
// video_render), so it is never released while the render code is using it.
struct source_ref {
	pthread_mutex_t mutex;
	struct dstr name;  // protected by mutex
	bool name_changed; // protected by mutex

	// Graphics thread only.
	struct dstr resolved_name;
	obs_weak_source_t *weak;
	float retry_timer;

	// The source we hold showing/active (see source_ref_update_activation),
	// and how: 0 = not held, 1 = showing, 2 = active (live, which also
	// counts as showing).
	obs_weak_source_t *held;
	int held_view;
	bool cycle;
	float cycle_timer;
};

extern void source_ref_init(struct source_ref *ref);
extern void source_ref_free(struct source_ref *ref);
extern void source_ref_set_name(struct source_ref *ref, const char *name);
extern void source_ref_tick(struct source_ref *ref, float seconds);
// Returns a new strong reference (release it), or NULL.
extern obs_source_t *source_ref_get(struct source_ref *ref);
extern bool source_ref_has_name(struct source_ref *ref);
// Keeps the referenced source showing/active while `owner` is (and `used` is
// true), as a scene does for its items, so sources that only update when
// shown or live (media, browser sources, ...) work as fills and inputs.
// Refuses references that would make a cycle back to `owner`. Graphics
// thread (video_tick).
extern void source_ref_update_activation(struct source_ref *ref, obs_source_t *owner, bool used, float seconds);

// Renders a source into *texrender (created once and reused between frames)
// and returns its texture, or NULL if the source has no size. Always renders
// in SDR sRGB (OBS tonemaps HDR sources), since the result is used as a
// fill color or shape mask.
extern gs_texture_t *render_source_to_texture(obs_source_t *source, gs_texrender_t **texrender);

// True if rendering `source` from inside `context` (a filter or a source
// that draws another source) would just render `context` again, e.g. when a
// source is picked as its own fill source.
extern bool is_self_reference(obs_source_t *context, obs_source_t *source);

// The color space a stroke/glow/shadow filter works and outputs in.
// GS_CS_SRGB is 8-bit SDR; the others are linear float (GS_CS_709_EXTENDED
// is HDR, with 1.0 = SDR white). Pass the spaces the caller wants, most
// preferred first (when rendering: the current space).
extern enum gs_color_space filter_working_space(obs_source_t *filter, size_t count,
						const enum gs_color_space *preferred_spaces);

// Shader flags for working in a float (non-SDR) color space: the crop/pad
// pass then sRGB-encodes the input itself (extended range, so HDR values
// above 1.0 survive), and the output pass skips the 8-bit dither.
extern void set_encode_input(gs_effect_t *effect, enum gs_color_space space);
extern void set_hdr_output(gs_effect_t *effect, enum gs_color_space space);
