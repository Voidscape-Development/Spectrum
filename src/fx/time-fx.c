// Spectrum Time FX: echo, feedback trails, frame-delay ghost, slit-scan and
// freeze / strobe.
//
// The filter keeps its own frame history on the GPU:
// - a ring buffer of past frames (echo, ghost, slit-scan), 8-bit sRGB, so
//   each buffered frame costs width x height x 4 bytes of VRAM (about 8 MB
//   at 1080p, 33 MB at 4K). The ring is capped at MAX_RING frames and at
//   RING_BUDGET bytes, whichever is smaller.
// - a float feedback buffer pair (feedback trails), 8 bytes per pixel each.
// - one held frame (freeze / strobe).
//
// History advances once per video frame (the first render after a tick);
// further renders in the same frame (preview, projectors) reuse the result.
// Everything is freed when the filter is disabled or its source stops being
// rendered, and reset when the source changes size.

#include "../common/fx-filter.h"

#include <graphics/vec4.h>
#include <util/threading.h>
#include <math.h>

#define MODE_ECHO 1
#define MODE_FEEDBACK 2
#define MODE_GHOST 3
#define MODE_SLIT 4
#define MODE_FREEZE 5

#define MAX_RING 240
#define RING_BUDGET (1024ull * 1024ull * 1024ull) // bytes
#define MAX_ECHO 16
// Seconds without a render before the history is dropped.
#define IDLE_FREE_SECONDS 0.5f

struct time_data {
	gs_texrender_t *ring[MAX_RING];
	int ring_size; // frames the ring holds
	int head;      // slot of the newest frame
	int filled;    // valid frames

	gs_texrender_t *scratch; // current frame when there's no ring
	gs_texrender_t *hold;    // freeze / strobe
	bool hold_valid;
	bool was_frozen;
	uint64_t strobe_count;

	gs_texrender_t *fb[2]; // feedback ping-pong, float premultiplied
	int fb_cur;
	bool fb_valid;

	gs_texrender_t *out; // float premultiplied result

	// What draw_output() shows.
	gs_texture_t *result;
	bool result_float;

	uint32_t width, height;
	bool pending; // a tick happened since the last processed frame
	float idle;
	int mode;

	volatile bool frozen;
	obs_hotkey_id hotkey;
};

/* ------------------------------------------------------------------------
 * Resources
 * --------------------------------------------------------------------- */

static void free_ring(struct time_data *d)
{
	for (int i = 0; i < MAX_RING; i++) {
		gs_texrender_destroy(d->ring[i]);
		d->ring[i] = NULL;
	}
	d->ring_size = 0;
	d->head = 0;
	d->filled = 0;
}

// Graphics context required.
static void free_all(struct time_data *d)
{
	free_ring(d);
	gs_texrender_destroy(d->scratch);
	gs_texrender_destroy(d->hold);
	gs_texrender_destroy(d->fb[0]);
	gs_texrender_destroy(d->fb[1]);
	gs_texrender_destroy(d->out);
	d->scratch = d->hold = d->fb[0] = d->fb[1] = d->out = NULL;
	d->hold_valid = false;
	d->fb_valid = false;
	d->result = NULL;
	d->strobe_count = 0;
	d->width = d->height = 0;
}

static bool has_resources(struct time_data *d)
{
	return d->width != 0 || d->scratch || d->hold || d->out || d->fb[0] || d->fb[1] || d->ring_size;
}

static gs_texrender_t *ensure_tr(gs_texrender_t **tr, enum gs_color_format format)
{
	if (!*tr)
		*tr = gs_texrender_create(format, GS_ZS_NONE);
	gs_texrender_reset(*tr);
	return *tr;
}

// The most frames the ring may hold at this size.
static int ring_cap(uint32_t w, uint32_t h)
{
	const unsigned long long frame = (unsigned long long)w * h * 4ull;
	unsigned long long cap = frame ? RING_BUDGET / frame : MAX_RING;
	if (cap > MAX_RING)
		cap = MAX_RING;
	return cap < 2 ? 2 : (int)cap;
}

static int ring_needed(obs_data_t *s, int mode)
{
	switch (mode) {
	case MODE_ECHO: {
		const int count = (int)obs_data_get_int(s, "tf_echo_count");
		const int spacing = (int)obs_data_get_int(s, "tf_echo_spacing");
		return (count < 1 ? 1 : count - 1) * (spacing < 1 ? 1 : spacing) + 1;
	}
	case MODE_GHOST:
		return (int)obs_data_get_int(s, "tf_ghost_delay") + 1;
	case MODE_SLIT:
		return (int)obs_data_get_int(s, "tf_slit_frames");
	default:
		return 0;
	}
}

static gs_texture_t *ring_frame(struct time_data *d, int age)
{
	if (age >= d->filled)
		age = d->filled - 1;
	if (age < 0)
		return NULL;
	const int slot = (d->head - age + d->ring_size) % d->ring_size;
	return gs_texrender_get_texture(d->ring[slot]);
}

/* ------------------------------------------------------------------------
 * Hotkey
 * --------------------------------------------------------------------- */

static void toggle_freeze(struct time_data *d)
{
	os_atomic_set_bool(&d->frozen, !os_atomic_load_bool(&d->frozen));
}

static void freeze_hotkey(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	fx_filter_t *fx = data;
	if (pressed && fx && fx->user)
		toggle_freeze(fx->user);
}

/* ------------------------------------------------------------------------
 * Lifetime hooks
 * --------------------------------------------------------------------- */

static void time_create_hook(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct time_data *d = bzalloc(sizeof(struct time_data));
	d->hotkey = OBS_INVALID_HOTKEY_ID;
	fx->user = d;
	d->hotkey = obs_hotkey_register_source(fx->context, "spectrum_time_fx.freeze",
					       obs_module_text("TimeFx.FreezeHotkey"), freeze_hotkey, fx);
}

static void time_update_hook(fx_filter_t *fx, obs_data_t *settings)
{
	struct time_data *d = fx->user;
	d->mode = (int)obs_data_get_int(settings, "tf_mode");
}

static void time_tick_hook(fx_filter_t *fx, float seconds)
{
	struct time_data *d = fx->user;
	d->pending = true;
	d->idle += seconds;
	const bool enabled = obs_source_enabled(fx->context);
	if ((!enabled || d->idle > IDLE_FREE_SECONDS) && has_resources(d)) {
		obs_enter_graphics();
		free_all(d);
		obs_leave_graphics();
	}
}

static void time_destroy_hook(fx_filter_t *fx)
{
	struct time_data *d = fx->user;
	if (d->hotkey != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(d->hotkey);
	obs_enter_graphics();
	free_all(d);
	obs_leave_graphics();
	bfree(d);
}

static const struct fx_filter_def time_def = {
	.effect_file = "shaders/fx/time-fx.effect",
	.dither_key = "tf_dither",
	.create = time_create_hook,
	.update = time_update_hook,
	.tick = time_tick_hook,
	.destroy = time_destroy_hook,
};

/* ------------------------------------------------------------------------
 * Rendering
 * --------------------------------------------------------------------- */

static void clear_target(void)
{
	struct vec4 clear_color;
	vec4_zero(&clear_color);
	gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
}

// Renders the filter input into tr (8-bit sRGB, straight alpha).
static gs_texture_t *capture_input(fx_filter_t *fx, gs_texrender_t **tr, uint32_t w, uint32_t h)
{
	ensure_tr(tr, GS_RGBA);
	bool ok = false;
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin_with_color_space(*tr, w, h, GS_CS_SRGB)) {
		clear_target();
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		if (obs_source_process_filter_begin(fx->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
			obs_source_process_filter_end(fx->context, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
			ok = true;
		}
		gs_texrender_end(*tr);
	}
	gs_blend_state_pop();
	return ok ? gs_texrender_get_texture(*tr) : NULL;
}

static void set_frame(gs_effect_t *effect, const char *name, gs_texture_t *tex)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, name);
	if (p)
		gs_effect_set_texture_srgb(p, tex);
}

// Begins drawing into a float target (cleared), w x h.
static bool begin_float(gs_texrender_t **tr, uint32_t w, uint32_t h)
{
	ensure_tr(tr, GS_RGBA16F);
	if (!gs_texrender_begin(*tr, w, h))
		return false;
	clear_target();
	gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
	return true;
}

static void draw_full(gs_effect_t *effect, const char *tech, uint32_t w, uint32_t h)
{
	while (gs_effect_loop(effect, tech))
		gs_draw_sprite(NULL, 0, w, h);
}

// A rectangle of the target (px), with uvs matching the full target.
static void draw_band(gs_effect_t *effect, const char *tech, float x0, float y0, float x1, float y1, uint32_t w,
		      uint32_t h)
{
	const float u0 = x0 / (float)w, u1 = x1 / (float)w, v0 = y0 / (float)h, v1 = y1 / (float)h;
	while (gs_effect_loop(effect, tech)) {
		gs_render_start(true);
		gs_texcoord(u0, v0, 0);
		gs_vertex2f(x0, y0);
		gs_texcoord(u1, v0, 0);
		gs_vertex2f(x1, y0);
		gs_texcoord(u0, v1, 0);
		gs_vertex2f(x0, y1);
		gs_texcoord(u1, v1, 0);
		gs_vertex2f(x1, y1);
		gs_render_stop(GS_TRISTRIP);
	}
}

static void process_echo(struct time_data *d, gs_effect_t *effect, obs_data_t *s, uint32_t w, uint32_t h)
{
	int count = (int)obs_data_get_int(s, "tf_echo_count");
	count = count < 1 ? 1 : (count > MAX_ECHO ? MAX_ECHO : count);
	int spacing = (int)obs_data_get_int(s, "tf_echo_spacing");
	spacing = spacing < 1 ? 1 : spacing;
	const double decay = obs_data_get_double(s, "tf_echo_decay") / 100.0;

	double weights[MAX_ECHO], total = 0.0;
	int used = 0;
	for (int i = 0; i < count; i++) {
		if (i * spacing >= d->filled)
			break;
		weights[i] = pow(decay, i);
		total += weights[i];
		used++;
	}
	if (total <= 0.0) {
		weights[0] = 1.0;
		total = 1.0;
		used = 1;
	}
	if (!begin_float(&d->out, w, h))
		return;
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	// Oldest first, so float rounding favours the newest frames. (OBS
	// resets effect parameters after each technique: set them per pass.)
	for (int i = used - 1; i >= 0; i--) {
		set_frame(effect, "frame_a", ring_frame(d, i * spacing));
		fx_set_float(effect, "weight", (float)(weights[i] / total));
		draw_full(effect, "Accum", w, h);
	}
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_texrender_end(d->out);
	d->result = gs_texrender_get_texture(d->out);
	d->result_float = true;
}

static void process_feedback(struct time_data *d, gs_effect_t *effect, obs_data_t *s, gs_texture_t *cur, uint32_t w,
			     uint32_t h)
{
	const int next = d->fb_cur ^ 1;
	gs_texture_t *old = d->fb_valid ? gs_texrender_get_texture(d->fb[d->fb_cur]) : NULL;
	const double angle = obs_data_get_double(s, "tf_fb_rotation") * M_PI / 180.0;
	double scale = obs_data_get_double(s, "tf_fb_scale") / 100.0;
	scale = scale < 0.01 ? 0.01 : scale;
	// Old-frame uv from output px: inverse of offset, scale and rotation
	// about the centre.
	const double c = cos(angle), sn = sin(angle);
	fx_set_vec4(effect, "fb_rot", (float)(c / scale), (float)(sn / scale), (float)(-sn / scale),
		    (float)(c / scale));
	fx_set_vec2(effect, "fb_offset", (float)obs_data_get_double(s, "tf_fb_offset_x"),
		    (float)obs_data_get_double(s, "tf_fb_offset_y"));
	fx_set_float(effect, "has_old", old ? 1.0f : 0.0f);
	fx_set_texture(effect, "old_tex", old);
	set_frame(effect, "frame_a", cur);
	if (!begin_float(&d->fb[next], w, h))
		return;
	draw_full(effect, "Feedback", w, h);
	gs_texrender_end(d->fb[next]);
	d->fb_cur = next;
	d->fb_valid = true;
	d->result = gs_texrender_get_texture(d->fb[next]);
	d->result_float = true;
}

static void process_ghost(struct time_data *d, gs_effect_t *effect, obs_data_t *s, gs_texture_t *cur, uint32_t w,
			  uint32_t h)
{
	const int delay = (int)obs_data_get_int(s, "tf_ghost_delay");
	set_frame(effect, "frame_a", cur);
	set_frame(effect, "frame_b", ring_frame(d, delay));
	if (!begin_float(&d->out, w, h))
		return;
	draw_full(effect, "Ghost", w, h);
	gs_texrender_end(d->out);
	d->result = gs_texrender_get_texture(d->out);
	d->result_float = true;
}

static void process_slit(fx_filter_t *fx, struct time_data *d, gs_effect_t *effect, obs_data_t *s, uint32_t w,
			 uint32_t h)
{
	const int map = (int)obs_data_get_int(s, "tf_slit_map");
	const bool invert = obs_data_get_bool(s, "tf_slit_invert");
	const int frames = d->ring_size;
	const int avail = d->filled;
	if (!begin_float(&d->out, w, h))
		return;
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
	for (int k = 0; k < avail; k++) {
		// OBS resets effect parameters at the end of every technique, so
		// each pass sets them all again.
		fx_filter_set_common(fx, effect, s, NULL, 0.0f, w, h, true, 0.0f);
		fx_set_float(effect, "slit_scale", (float)(frames - 1));
		fx_set_float(effect, "slit_max", (float)(avail - 1));
		set_frame(effect, "frame_a", ring_frame(d, k));
		fx_set_float(effect, "slit_index", (float)k);
		// Rows and columns: only the band of pixels that frame k reaches.
		if ((map == 1 || map == 2) && frames > 1) {
			const bool rows = map == 1;
			const float len = rows ? (float)h : (float)w;
			// Frame index = pos / len * (frames - 1), mirrored if inverted.
			float a = (float)(k - 1) / (float)(frames - 1) * len;
			float b = (float)(k + 1) / (float)(frames - 1) * len;
			if (k == avail - 1)
				b = len; // the oldest frame fills everything past it
			if (invert) {
				const float ia = len - b, ib = len - a;
				a = ia;
				b = ib;
			}
			a = floorf(fmaxf(a, 0.0f));
			b = ceilf(fminf(b, len));
			if (b <= a)
				continue;
			if (rows)
				draw_band(effect, "SlitAccum", 0.0f, a, (float)w, b, w, h);
			else
				draw_band(effect, "SlitAccum", a, 0.0f, b, (float)h, w, h);
		} else {
			draw_full(effect, "SlitAccum", w, h);
		}
	}
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_texrender_end(d->out);
	d->result = gs_texrender_get_texture(d->out);
	d->result_float = true;
}

static void process_freeze(struct time_data *d, obs_data_t *s, gs_texture_t *cur)
{
	const int strobe = (int)obs_data_get_int(s, "tf_strobe");
	const bool frozen = os_atomic_load_bool(&d->frozen);
	bool take = false;
	if (frozen) {
		take = !d->was_frozen || !d->hold_valid;
	} else if (strobe > 1) {
		take = !d->hold_valid || (d->strobe_count % (uint64_t)strobe) == 0;
		d->strobe_count++;
	} else {
		d->hold_valid = false;
	}
	d->was_frozen = frozen;

	if (take) {
		// The frame just captured becomes the held one.
		gs_texrender_t *tmp = d->hold;
		d->hold = d->scratch;
		d->scratch = tmp;
		d->hold_valid = true;
	}
	d->result = (frozen || strobe > 1) && d->hold_valid ? gs_texrender_get_texture(d->hold) : cur;
	d->result_float = false;
}

static void process_frame(fx_filter_t *fx, struct time_data *d, gs_effect_t *effect, obs_data_t *s, uint32_t w,
			  uint32_t h)
{
	const int mode = (int)obs_data_get_int(s, "tf_mode");
	int needed = ring_needed(s, mode);
	if (needed > 0) {
		const int cap = ring_cap(w, h);
		needed = needed < 1 ? 1 : (needed > cap ? cap : needed);
	}
	if (needed != d->ring_size) {
		free_ring(d);
		d->ring_size = needed;
	}
	if (mode != MODE_FEEDBACK && d->fb_valid) {
		gs_texrender_destroy(d->fb[0]);
		gs_texrender_destroy(d->fb[1]);
		d->fb[0] = d->fb[1] = NULL;
		d->fb_valid = false;
	}
	if (mode != MODE_FREEZE && d->hold) {
		gs_texrender_destroy(d->hold);
		d->hold = NULL;
		d->hold_valid = false;
	}

	gs_texture_t *cur;
	if (d->ring_size > 0) {
		const int slot = (d->head + 1) % d->ring_size;
		cur = capture_input(fx, &d->ring[slot], w, h);
		if (cur) {
			d->head = slot;
			if (d->filled < d->ring_size)
				d->filled++;
		}
	} else {
		cur = capture_input(fx, &d->scratch, w, h);
	}
	if (!cur) {
		d->result = NULL;
		return;
	}

	fx_filter_set_common(fx, effect, s, NULL, 0.0f, w, h, true, 0.0f);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);

	switch (mode) {
	case MODE_ECHO:
		process_echo(d, effect, s, w, h);
		break;
	case MODE_FEEDBACK:
		process_feedback(d, effect, s, cur, w, h);
		break;
	case MODE_GHOST:
		process_ghost(d, effect, s, cur, w, h);
		break;
	case MODE_SLIT:
		process_slit(fx, d, effect, s, w, h);
		break;
	case MODE_FREEZE:
		process_freeze(d, s, cur);
		break;
	default:
		d->result = cur;
		d->result_float = false;
		break;
	}

	gs_enable_framebuffer_srgb(previous);
	gs_blend_state_pop();
}

static void time_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	fx_filter_t *fx = data;
	struct time_data *d = fx->user;

	obs_source_t *target = obs_filter_get_target(fx->context);
	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	if (!w || !h || !fx_filter_ensure_effect(fx)) {
		obs_source_skip_video_filter(fx->context);
		return;
	}
	gs_effect_t *effect = fx->effect;
	obs_data_t *settings = fx_filter_settings(fx);
	d->idle = 0.0f;

	if (w != d->width || h != d->height) {
		// New size: the history no longer fits.
		free_all(d);
		d->width = w;
		d->height = h;
		d->pending = true;
	}
	if (d->pending || !d->result) {
		d->pending = false;
		process_frame(fx, d, effect, settings, w, h);
	}
	if (!d->result) {
		obs_data_release(settings);
		obs_source_skip_video_filter(fx->context);
		return;
	}

	// Output: linear light through the sRGB framebuffer, straight alpha,
	// dithered.
	fx_filter_set_common(fx, effect, settings, NULL, 0.0f, w, h, true, 0.0f);
	if (d->result_float) {
		fx_set_texture(effect, "result_tex", d->result);
	} else {
		// An untouched 8-bit frame: already quantised, nothing to dither.
		set_frame(effect, "frame_a", d->result);
		fx_set_float(effect, "dither_step", 0.0f);
	}
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	draw_full(effect, d->result_float ? "OutputFloat" : "OutputFrame", w, h);
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);

	obs_data_release(settings);
}

/* ------------------------------------------------------------------------
 * obs_source_info
 * --------------------------------------------------------------------- */

static const char *time_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("TimeFxFilter");
}

static void *time_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&time_def, settings, source);
}

static void time_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "tf_mode", MODE_ECHO);
	obs_data_set_default_int(settings, "tf_echo_count", 6);
	obs_data_set_default_int(settings, "tf_echo_spacing", 2);
	obs_data_set_default_double(settings, "tf_echo_decay", 70.0);
	obs_data_set_default_double(settings, "tf_fb_decay", 85.0);
	obs_data_set_default_int(settings, "tf_fb_blend", 1);
	obs_data_set_default_double(settings, "tf_fb_offset_x", 0.0);
	obs_data_set_default_double(settings, "tf_fb_offset_y", 0.0);
	obs_data_set_default_double(settings, "tf_fb_scale", 100.0);
	obs_data_set_default_double(settings, "tf_fb_rotation", 0.0);
	obs_data_set_default_int(settings, "tf_ghost_delay", 10);
	obs_data_set_default_double(settings, "tf_ghost_opacity", 50.0);
	obs_data_set_default_int(settings, "tf_ghost_color", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "tf_ghost_blend", 1);
	obs_data_set_default_int(settings, "tf_slit_frames", 30);
	obs_data_set_default_int(settings, "tf_slit_map", 1);
	obs_data_set_default_bool(settings, "tf_slit_invert", false);
	obs_data_set_default_double(settings, "tf_slit_angle", 0.0);
	obs_data_set_default_double(settings, "tf_slit_center_x", 50.0);
	obs_data_set_default_double(settings, "tf_slit_center_y", 50.0);
	obs_data_set_default_double(settings, "tf_slit_radius", 100.0);
	obs_data_set_default_int(settings, "tf_strobe", 1);
	obs_data_set_default_bool(settings, "tf_dither", true);
}

static void vis(obs_properties_t *props, const char *key, bool visible)
{
	obs_property_set_visible(obs_properties_get(props, key), visible);
}

static bool mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int m = (int)obs_data_get_int(settings, "tf_mode");
	const int map = (int)obs_data_get_int(settings, "tf_slit_map");
	vis(props, "tf_echo_count", m == MODE_ECHO);
	vis(props, "tf_echo_spacing", m == MODE_ECHO);
	vis(props, "tf_echo_decay", m == MODE_ECHO);
	vis(props, "tf_fb_decay", m == MODE_FEEDBACK);
	vis(props, "tf_fb_blend", m == MODE_FEEDBACK);
	vis(props, "tf_fb_offset_x", m == MODE_FEEDBACK);
	vis(props, "tf_fb_offset_y", m == MODE_FEEDBACK);
	vis(props, "tf_fb_scale", m == MODE_FEEDBACK);
	vis(props, "tf_fb_rotation", m == MODE_FEEDBACK);
	vis(props, "tf_ghost_delay", m == MODE_GHOST);
	vis(props, "tf_ghost_opacity", m == MODE_GHOST);
	vis(props, "tf_ghost_color", m == MODE_GHOST);
	vis(props, "tf_ghost_blend", m == MODE_GHOST);
	vis(props, "tf_slit_frames", m == MODE_SLIT);
	vis(props, "tf_slit_map", m == MODE_SLIT);
	vis(props, "tf_slit_invert", m == MODE_SLIT);
	vis(props, "tf_slit_angle", m == MODE_SLIT && map == 3);
	vis(props, "tf_slit_center_x", m == MODE_SLIT && map == 4);
	vis(props, "tf_slit_center_y", m == MODE_SLIT && map == 4);
	vis(props, "tf_slit_radius", m == MODE_SLIT && map == 4);
	vis(props, "tf_strobe", m == MODE_FREEZE);
	vis(props, "tf_freeze", m == MODE_FREEZE);
	vis(props, "tf_memory", m == MODE_ECHO || m == MODE_GHOST || m == MODE_SLIT);
	return true;
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static bool freeze_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	fx_filter_t *fx = data;
	if (fx && fx->user)
		toggle_freeze(fx->user);
	return false;
}

static obs_properties_t *time_properties(void *data)
{
	UNUSED_PARAMETER(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "tf_mode", obs_module_text("TimeFx.Mode"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("TimeFx.Mode.Echo"), MODE_ECHO);
	obs_property_list_add_int(p, obs_module_text("TimeFx.Mode.Feedback"), MODE_FEEDBACK);
	obs_property_list_add_int(p, obs_module_text("TimeFx.Mode.Ghost"), MODE_GHOST);
	obs_property_list_add_int(p, obs_module_text("TimeFx.Mode.Slit"), MODE_SLIT);
	obs_property_list_add_int(p, obs_module_text("TimeFx.Mode.Freeze"), MODE_FREEZE);
	obs_property_set_modified_callback(p, mode_modified);

	p = obs_properties_add_int_slider(props, "tf_echo_count", obs_module_text("TimeFx.EchoCount"), 1, MAX_ECHO, 1);
	p = obs_properties_add_int_slider(props, "tf_echo_spacing", obs_module_text("TimeFx.EchoSpacing"), 1, 30, 1);
	obs_property_int_set_suffix(p, obs_module_text("TimeFx.FramesSuffix"));
	slider(props, "tf_echo_decay", "Distort.Decay", 0.0, 100.0, 0.1, "%");

	slider(props, "tf_fb_decay", "TimeFx.Persistence", 0.0, 99.5, 0.1, "%");
	p = obs_properties_add_list(props, "tf_fb_blend", obs_module_text("Fx.BlendMode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Normal"), 1);
	obs_property_list_add_int(p, obs_module_text("TimeFx.Blend.Lighten"), 2);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Add"), 3);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Screen"), 4);
	slider(props, "tf_fb_offset_x", "TimeFx.OffsetX", -100.0, 100.0, 0.1, "px");
	slider(props, "tf_fb_offset_y", "TimeFx.OffsetY", -100.0, 100.0, 0.1, "px");
	slider(props, "tf_fb_scale", "TimeFx.Scale", 50.0, 150.0, 0.1, "%");
	slider(props, "tf_fb_rotation", "TimeFx.Rotation", -45.0, 45.0, 0.1, "°");

	p = obs_properties_add_int_slider(props, "tf_ghost_delay", obs_module_text("TimeFx.Delay"), 1, MAX_RING - 1, 1);
	obs_property_int_set_suffix(p, obs_module_text("TimeFx.FramesSuffix"));
	slider(props, "tf_ghost_opacity", "TimeFx.Opacity", 0.0, 100.0, 0.1, "%");
	obs_properties_add_color(props, "tf_ghost_color", obs_module_text("Light.Tint"));
	p = obs_properties_add_list(props, "tf_ghost_blend", obs_module_text("Fx.BlendMode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Normal"), 1);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Add"), 2);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Screen"), 3);
	obs_property_list_add_int(p, obs_module_text("TimeFx.Blend.Difference"), 4);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Multiply"), 5);
	obs_property_list_add_int(p, obs_module_text("TimeFx.Blend.Lighten"), 6);

	p = obs_properties_add_int_slider(props, "tf_slit_frames", obs_module_text("TimeFx.SlitFrames"), 2, MAX_RING,
					  1);
	obs_property_int_set_suffix(p, obs_module_text("TimeFx.FramesSuffix"));
	p = obs_properties_add_list(props, "tf_slit_map", obs_module_text("TimeFx.SlitMap"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("TimeFx.SlitMap.Rows"), 1);
	obs_property_list_add_int(p, obs_module_text("TimeFx.SlitMap.Columns"), 2);
	obs_property_list_add_int(p, obs_module_text("TimeFx.SlitMap.Linear"), 3);
	obs_property_list_add_int(p, obs_module_text("TimeFx.SlitMap.Radial"), 4);
	obs_property_set_modified_callback(p, mode_modified);
	obs_properties_add_bool(props, "tf_slit_invert", obs_module_text("TimeFx.SlitInvert"));
	slider(props, "tf_slit_angle", "LongShadow.Angle", -180.0, 180.0, 0.1, "°");
	slider(props, "tf_slit_center_x", "Gradient.CenterX", -50.0, 150.0, 0.1, "%");
	slider(props, "tf_slit_center_y", "Gradient.CenterY", -50.0, 150.0, 0.1, "%");
	slider(props, "tf_slit_radius", "Distort.Radius", 1.0, 400.0, 0.1, "%");

	p = obs_properties_add_int_slider(props, "tf_strobe", obs_module_text("TimeFx.Strobe"), 1, 120, 1);
	obs_property_int_set_suffix(p, obs_module_text("TimeFx.FramesSuffix"));
	obs_property_set_long_description(p, obs_module_text("TimeFx.Strobe.Description"));
	p = obs_properties_add_button2(props, "tf_freeze", obs_module_text("TimeFx.FreezeButton"), freeze_clicked,
				       data);
	obs_property_set_long_description(p, obs_module_text("TimeFx.FreezeButton.Description"));

	obs_properties_add_text(props, "tf_memory", obs_module_text("TimeFx.Memory"), OBS_TEXT_INFO);

	obs_properties_add_bool(props, "tf_dither", obs_module_text("Gradient.Dither"));
	return props;
}

struct obs_source_info spectrum_time_fx_filter = {
	.id = "spectrum_time_fx_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = time_name,
	.create = time_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = time_render,
	.get_properties = time_properties,
	.get_defaults = time_defaults,
};
