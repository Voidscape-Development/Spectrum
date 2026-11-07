#pragma once

// Shared gradient engine, used by the Spectrum Gradient source and by the
// gradient fill of the stroke, glow and shadow filters/sources.
//
// The color stops are baked on the CPU into a high resolution 32-bit float
// lookup table (interpolated in the chosen color space, with easing,
// midpoints and gamut mapping), which the shader samples with manual linear
// interpolation in float. The result is only quantised by the final render
// target, where it is dithered (triangular noise of +/-1 step, sized to the
// target's bit depth), so gradients never band.
//
// Every setting is stored under a key prefix, so several gradients can live
// in one settings object (e.g. "gradient_" for the source,
// "stroke_gradient_" for the stroke fill).

#include <obs-module.h>
#include <graphics/vec4.h>
#include <util/threading.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GRADIENT_MAX_STOPS 32
#define GRADIENT_LUT_SIZE 2048

enum gradient_shape {
	GRADIENT_SHAPE_LINEAR = 1,
	GRADIENT_SHAPE_REFLECTED,
	GRADIENT_SHAPE_RADIAL,
	GRADIENT_SHAPE_ELLIPTICAL,
	GRADIENT_SHAPE_CONIC,
	GRADIENT_SHAPE_SPIRAL,
	GRADIENT_SHAPE_DIAMOND,
	GRADIENT_SHAPE_CORNERS,
};

enum gradient_interp {
	GRADIENT_INTERP_OKLAB = 1,
	GRADIENT_INTERP_OKLCH_SHORTER,
	GRADIENT_INTERP_OKLCH_LONGER,
	GRADIENT_INTERP_OKLCH_INCREASING,
	GRADIENT_INTERP_OKLCH_DECREASING,
	GRADIENT_INTERP_LINEAR_RGB,
	GRADIENT_INTERP_SRGB,
};

enum gradient_repeat {
	GRADIENT_REPEAT_PAD = 1,
	GRADIENT_REPEAT_REPEAT,
	GRADIENT_REPEAT_MIRROR,
};

// Easing of the segment from a stop to the next one.
enum gradient_ease {
	GRADIENT_EASE_LINEAR = 1,
	GRADIENT_EASE_SMOOTH,
	GRADIENT_EASE_IN,
	GRADIENT_EASE_OUT,
	GRADIENT_EASE_IN_OUT,
	GRADIENT_EASE_STEP,
};

struct gradient_stop {
	float position; // 0..1
	uint32_t color; // OBS color, 0xAABBGGRR (sRGB, straight alpha)
	float midpoint; // 0..1, where the segment to the next stop is half way
	enum gradient_ease ease;
};

typedef struct gradient {
	obs_source_t *owner;
	const char *prefix; // static storage

	// Settings (written by update, read when rendering).
	enum gradient_shape shape;
	enum gradient_interp interp;
	enum gradient_repeat repeat;
	float center_x, center_y; // fraction of the size
	float angle;              // radians
	float scale;              // fraction
	float aspect;             // x/y stretch ratio
	float focal_x, focal_y;   // fraction of the radius
	float twist;              // spiral turns from the center to the edge
	int repetitions;          // conic / spiral sweeps
	float offset;             // phase, in gradient lengths
	float hue_shift;          // degrees
	bool corner_smooth;
	uint32_t corners[4]; // top left, top right, bottom left, bottom right
	float anim_rotate;   // degrees / second
	float anim_scroll;   // gradient lengths / second
	float anim_hue;      // degrees / second
	float opacity;
	float grain_amount; // 0..1
	float grain_size;   // px
	bool grain_animate;
	bool dither;

	// Animation state (graphics thread).
	float time_rotate; // radians
	float time_scroll;
	float time_hue; // degrees
	float grain_seed;

	// Baked lookup table: written by update under the mutex, uploaded on
	// the graphics thread.
	pthread_mutex_t mutex;
	float *lut_pending;
	bool lut_dirty;
	bool lut_baked;
	uint64_t lut_hash;

	// Graphics resources.
	gs_texture_t *lut_texture;
	gs_effect_t *effect;
	gs_texrender_t *fill_texrender;
} gradient_t;

void gradient_init(gradient_t *g, obs_source_t *owner, const char *prefix);
// Frees graphics resources; must not be called from inside the graphics
// context (it enters it itself).
void gradient_free(gradient_t *g);

void gradient_defaults(obs_data_t *settings, const char *prefix);
void gradient_update(gradient_t *g, obs_data_t *settings);
// Adds the gradient settings as groups. `g` may be NULL (no instance), in
// which case the stop editor button does nothing.
void gradient_properties(obs_properties_t *props, const char *prefix, gradient_t *g);

// Only the stops, blending, extend mode, offset and hue animation: for
// effects that map a value through the gradient (gradient map, noise).
#define GRADIENT_PROPS_MAP (1u << 0)
void gradient_properties_ex(obs_properties_t *props, const char *prefix, gradient_t *g, uint32_t flags);
// Shows or hides all of the gradient's groups (for fill type lists).
void gradient_set_visible(obs_properties_t *props, const char *prefix, bool visible);
// Hides the settings that place the gradient on screen (shape, center,
// angle...) and the finish group, for uses that only map a value through
// the gradient.
void gradient_hide_geometry(obs_properties_t *props, const char *prefix);
// Applies the shape dependent visibility of the gradient settings.
void gradient_refresh_visibility(obs_properties_t *props, const char *prefix, obs_data_t *settings);

void gradient_tick(gradient_t *g, float seconds);
// True when the gradient changes over time.
bool gradient_is_animated(const gradient_t *g);

// Draws the gradient to the current render target as a source output
// (straight alpha, linear with sRGB framebuffer encoding), dithered to the
// bit depth of the target.
void gradient_draw(gradient_t *g, uint32_t width, uint32_t height);
// Renders the gradient into a float texture to use as a stroke/glow/shadow
// fill (sRGB encoded, premultiplied in linear space, like a rendered fill
// source). Returns NULL on failure.
gs_texture_t *gradient_render_fill(gradient_t *g, uint32_t width, uint32_t height);

// Binds the lookup table to another effect, for shaders that sample the
// gradient themselves (see data/shaders/fx/fx-common.effect): sets
// gradient_lut, gradient_lut_size, gradient_offset, gradient_repeat and
// gradient_hue where the effect has them. Graphics thread.
bool gradient_bind_lut(gradient_t *g, gs_effect_t *effect);

// The size of one quantisation step of the current render target in
// (sRGB encoded) output units, or 0 for float targets. Graphics thread.
float spectrum_target_dither_step(void);

// Stops stored in settings.
size_t gradient_read_stops(obs_data_t *settings, const char *prefix, struct gradient_stop *stops, size_t max_stops);
void gradient_write_stops(obs_data_t *settings, const char *prefix, const struct gradient_stop *stops, size_t count);
void gradient_default_stops(struct gradient_stop *stops, size_t *count);

// Bakes `count` samples (t = i / (count - 1)) of the gradient into `out`
// (linear sRGB, straight alpha, 4 floats per sample). Stops need not be
// sorted.
void gradient_bake(const struct gradient_stop *stops, size_t stop_count, enum gradient_interp interp, float *out,
		   size_t count);

// Small color helpers, shared with the editor.
float gradient_srgb_to_linear(float v);
float gradient_linear_to_srgb(float v);

#ifdef __cplusplus
}
#endif
