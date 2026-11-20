#pragma once

#include <obs-module.h>

// Corner brackets: the "Corners" line style of Spectrum Stroke, and the
// corner options of Spectrum Glow and Shadow. See corners-common.effect.

enum corner_mode {
	CORNER_MODE_BOUNDS = 0,
	CORNER_MODE_CONTOUR,
};

enum corner_rect {
	CORNER_RECT_FRAME = 0,
	CORNER_RECT_CONTENT,
};

enum corner_unit {
	CORNER_UNIT_PX = 0,
	CORNER_UNIT_PCT,
};

enum corner_cap {
	CORNER_CAP_SQUARE = 0,
	CORNER_CAP_ROUND,
	CORNER_CAP_FADE,
};

// Glow and Shadow only.
enum corner_glow {
	CORNER_GLOW_OFF = 0,
	CORNER_GLOW_MASK,     // the glow, limited to the corners
	CORNER_GLOW_BRACKETS, // the glow cast by the corner brackets of the shape
};

struct corners {
	// Settings
	bool enabled;
	enum corner_mode mode;
	enum corner_rect rect;
	enum corner_cap cap;
	enum corner_glow glow;
	bool pct;
	bool sharp;
	float arm_h, arm_v; // px, or fractions of the edge/outline
	float angle;        // degrees
	float radius;       // px
	bool on[4];         // TL, TR, BR, BL
	float pulse_speed, pulse_depth, slide_speed;
	double time;

	// Content box (1x1), corners and links (contour mode), ping-pong.
	gs_texrender_t *bounds, *mark, *next, *link, *a, *b;
	// Glow/Shadow only: their own effect, distance and arc fields, and the
	// bracket coverage.
	gs_effect_t *effect;
	gs_texrender_t *jf_a, *jf_b, *arc_a, *arc_b, *mask;
};

void corners_defaults(obs_data_t *settings);
void corners_properties(obs_properties_t *props, bool glow);
// Shows the corner properties for the current settings (on: style enabled).
void corners_refresh(obs_properties_t *props, obs_data_t *settings, bool on);
void corners_update(struct corners *c, obs_data_t *settings, bool on);
// Advances the animation; true when it moves (the result must be redrawn).
bool corners_tick(struct corners *c, float seconds);
// Graphics thread.
void corners_free(struct corners *c);

// The outline pipeline shared with the stroke (see jump-flood-common.effect),
// for an input of w x h px with straight alpha. Both return the texrender
// (one of *a / *b) holding the result.
gs_texrender_t *jf_field(gs_effect_t *effect, gs_texture_t *input, uint32_t w, uint32_t h, float threshold,
			 float extent, gs_texrender_t **a, gs_texrender_t **b);
gs_texrender_t *jf_arc(gs_effect_t *effect, gs_texture_t *input, uint32_t w, uint32_t h, float threshold,
		       gs_texrender_t **a, gs_texrender_t **b);

// Finds the content box and (contour mode, from the arc field) the corners.
void corners_render(struct corners *c, gs_effect_t *effect, gs_texture_t *input, gs_texture_t *arc, uint32_t w,
		    uint32_t h, float threshold);
// Sets the corner uniforms of a drawing pass. frame: the source's area in
// the (padded) input, (min x, min y, max x, max y) px.
void corners_bind(struct corners *c, gs_effect_t *effect, gs_texture_t *arc, uint32_t w, uint32_t h,
		  const struct vec4 *frame);
// Glow/Shadow: the bracket coverage of the input (R channel), or NULL.
gs_texture_t *corners_render_mask(struct corners *c, gs_texture_t *input, uint32_t w, uint32_t h, float threshold,
				  const struct vec4 *frame);
