// Spectrum Animate: drives numeric settings of other filters on the same
// source (or of the source itself). Works with any filter or source, not
// only Spectrum's.
//
// One driver produces a value each frame:
//   - a looping waveform (with optional easing),
//   - a list of keyframes with per-segment easing,
//   - the level of an audio source (attack/release envelope),
//   - the clock (seconds, minutes, hour of the day, ...),
//   - the mouse position on a screen,
//   - the brightness or the motion of another source,
//   - another setting (value = setting x scale + offset).
// Waveforms and keyframes can loop, ping-pong or play once, and can be
// started by a hotkey, when the source is shown or goes live, or always run.
//
// The driver value (0..1) is mapped to each target's From -> To range; up to
// AN_MAX_SLOTS targets share the driver. Target 1 uses the original setting
// keys (an_target, an_setting, an_min, an_max) so older scenes load as they
// were; targets 2.. use the same keys with a "_N" suffix.
//
// The filter itself draws nothing (it passes video through). The values are
// applied with obs_source_update() from video_tick.

#include <obs-module.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>
#include <time.h>
#include "../common/presets.h"
#include "animate-ui.h"
#include "keyframes.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SELF_TARGET "" // the parent source
#define AN_MAX_SLOTS 8
#define PROBE_SIZE 96 // long side of the downsampled copy for brightness/motion

enum an_mode {
	MODE_LOOP = 1,
	MODE_AUDIO = 2,
	MODE_KEYFRAMES = 3,
	MODE_CLOCK = 4,
	MODE_MOUSE = 5,
	MODE_BRIGHTNESS = 6,
	MODE_MOTION = 7,
	MODE_LINK = 8,
};
enum an_wave { WAVE_SINE = 1, WAVE_TRIANGLE, WAVE_SAW_UP, WAVE_SAW_DOWN, WAVE_SQUARE, WAVE_RANDOM };
enum an_playback { PLAY_LOOP = 0, PLAY_PINGPONG = 1, PLAY_ONCE = 2 };
enum an_trigger { TRIGGER_AUTO = 0, TRIGGER_HOTKEY = 1, TRIGGER_SHOW = 2, TRIGGER_ACTIVE = 3 };
enum an_end { END_HOLD = 0, END_RETURN = 1 };
enum an_clock { CLOCK_SECOND = 0, CLOCK_MINUTE, CLOCK_HOUR, CLOCK_HOUR12, CLOCK_WEEKDAY, CLOCK_MONTHDAY, CLOCK_MONTH };

struct an_slot {
	struct dstr target;
	struct dstr setting;
	double min, max;
	double offset; // delay, as a fraction of the cycle
};

// Everything video_tick needs from the settings; copied under the mutex.
struct an_config {
	int slots;
	struct an_slot slot[AN_MAX_SLOTS];
	enum an_mode mode;
	enum an_wave wave;
	int wave_ease;
	double period, phase;
	enum an_playback playback;
	enum an_trigger trigger;
	enum an_end end;
	size_t key_count;
	struct an_keyframe keys[AN_MAX_KEYS];
	double attack, release;
	enum an_clock clock;
	bool clock_step;
	int mouse_axis, mouse_screen;
	double mouse_smooth;
	double luma_lo, luma_hi, motion_full;
	struct dstr probe_name;
	struct dstr link_target, link_setting;
	double link_scale, link_offset;
	struct dstr audio_name;
};

struct animate_filter {
	obs_source_t *context;

	// Settings (written on the UI thread, read in video_tick).
	pthread_mutex_t mutex;
	struct an_config cfg;
	double floor_db, ceiling_db; // read by the audio callback
	bool cmd_play, cmd_stop;     // from hotkeys and buttons
	bool reset_values;

	// Playback state (video_tick).
	double time; // playhead, seconds
	bool playing;
	bool was_enabled;
	bool was_shown, was_active;
	int last_trigger, last_playback, last_mode;
	double envelope;
	bool have_mouse;
	double last_value[AN_MAX_SLOTS];
	bool have_last[AN_MAX_SLOTS];

	// Hotkeys, registered on the parent source so they're listed (and
	// saved) with it.
	obs_hotkey_id hk_play, hk_stop;
	obs_weak_source_t *hk_parent;
	struct dstr hk_name;

	// Audio capture.
	obs_weak_source_t *audio_weak;
	struct dstr audio_bound;
	pthread_mutex_t level_mutex;
	float level; // latest normalised level, 0..1

	// Brightness/motion probe (graphics thread).
	gs_texrender_t *probe_tr;
	gs_stagesurf_t *probe_ss[2];
	bool probe_staged[2];
	int probe_index;
	uint32_t probe_w, probe_h;
	float *probe_prev;
	bool probe_have_prev;
	double probe_luma, probe_motion;
};

static void config_free(struct an_config *c)
{
	for (int i = 0; i < AN_MAX_SLOTS; i++) {
		dstr_free(&c->slot[i].target);
		dstr_free(&c->slot[i].setting);
	}
	dstr_free(&c->probe_name);
	dstr_free(&c->link_target);
	dstr_free(&c->link_setting);
	dstr_free(&c->audio_name);
}

static void config_copy(struct an_config *dst, const struct an_config *src)
{
	config_free(dst);
	*dst = *src; // scalars and keyframes; strings are replaced below
	for (int i = 0; i < AN_MAX_SLOTS; i++) {
		memset(&dst->slot[i].target, 0, sizeof(struct dstr));
		memset(&dst->slot[i].setting, 0, sizeof(struct dstr));
		dstr_copy_dstr(&dst->slot[i].target, &src->slot[i].target);
		dstr_copy_dstr(&dst->slot[i].setting, &src->slot[i].setting);
	}
	struct dstr *d[] = {&dst->probe_name, &dst->link_target, &dst->link_setting, &dst->audio_name};
	const struct dstr *s[] = {&src->probe_name, &src->link_target, &src->link_setting, &src->audio_name};
	for (size_t i = 0; i < sizeof(d) / sizeof(d[0]); i++) {
		memset(d[i], 0, sizeof(struct dstr));
		dstr_copy_dstr(d[i], s[i]);
	}
}

// Setting key of target slot `slot` (0-based): "an_setting", "an_setting_2", ...
static const char *slot_key(char *buf, size_t size, const char *base, int slot)
{
	if (slot == 0)
		snprintf(buf, size, "%s", base);
	else
		snprintf(buf, size, "%s_%d", base, slot + 1);
	return buf;
}

// Slot index of a property named "<base>" or "<base>_N", or -1.
static int key_slot(const char *name, const char *base)
{
	const size_t len = strlen(base);
	if (!name || strncmp(name, base, len) != 0)
		return -1;
	if (name[len] == '\0')
		return 0;
	if (name[len] != '_')
		return -1;
	const int n = atoi(name + len + 1);
	return n >= 2 && n <= AN_MAX_SLOTS ? n - 1 : -1;
}

static bool is_time_mode(enum an_mode mode)
{
	return mode == MODE_LOOP || mode == MODE_KEYFRAMES;
}

/* ------------------------------------------------------------------------
 * Drivers
 * --------------------------------------------------------------------- */

static double hash1(double n)
{
	double x = sin(n * 127.1 + 311.7) * 43758.5453123;
	return x - floor(x);
}

static double wave_value(enum an_wave wave, double t, double cycle)
{
	switch (wave) {
	case WAVE_TRIANGLE:
		return 1.0 - fabs(2.0 * t - 1.0);
	case WAVE_SAW_UP:
		return t;
	case WAVE_SAW_DOWN:
		return 1.0 - t;
	case WAVE_SQUARE:
		return t < 0.5 ? 0.0 : 1.0;
	case WAVE_RANDOM: {
		// Smoothly interpolated random values, a new one each cycle.
		const double a = hash1(cycle), b = hash1(cycle + 1.0);
		const double u = t * t * (3.0 - 2.0 * t);
		return a + (b - a) * u;
	}
	default:
		return 0.5 - 0.5 * cos(2.0 * M_PI * t);
	}
}

// Length of one cycle of the time-based drivers, in seconds.
static double cycle_length(const struct an_config *c)
{
	if (c->mode == MODE_KEYFRAMES)
		return fmax(an_keys_duration(c->keys, c->key_count), 0.0);
	return c->period;
}

// Position within the cycle, in [0, 1], at playhead time `t` (seconds),
// following the playback mode. *cycle receives the cycle number.
static double play_position(enum an_playback playback, double t, double length, double *cycle)
{
	if (length <= 0.0) {
		*cycle = 0.0;
		return 1.0;
	}
	const double x = t / length;
	switch (playback) {
	case PLAY_PINGPONG: {
		const double c = floor(x / 2.0);
		const double r = x - 2.0 * c; // 0..2
		*cycle = c;
		return r <= 1.0 ? r : 2.0 - r;
	}
	case PLAY_ONCE:
		*cycle = 0.0;
		return x < 0.0 ? 0.0 : (x > 1.0 ? 1.0 : x);
	default:
		*cycle = floor(x);
		return x - *cycle;
	}
}

// Value of a time-based driver (0..1, may overshoot) for one target.
static double time_driver_value(const struct an_config *c, double playhead, double offset)
{
	const double length = cycle_length(c);
	const double t = playhead - offset * length;

	if (c->mode == MODE_KEYFRAMES) {
		if (!c->key_count)
			return 0.0;
		// The phase shifts a repeating animation; a one-shot always
		// starts at its first keyframe.
		double cycle;
		const double shift = c->playback == PLAY_ONCE ? 0.0 : c->phase * length;
		const double pos = play_position(c->playback, t + shift, length, &cycle);
		return an_keys_eval(c->keys, c->key_count, pos * length) / 100.0;
	}

	double v;
	if (c->playback == PLAY_LOOP) {
		// The original loop: identical to earlier versions.
		const double x = t / length + c->phase;
		const double cycle = floor(x);
		v = wave_value(c->wave, x - cycle, cycle);
	} else {
		double cycle;
		const double pos = play_position(c->playback, t, length, &cycle);
		double x = pos + c->phase, k = floor(x), frac = x - k;
		if (c->playback == PLAY_ONCE && pos >= 1.0 && frac == 0.0) {
			// The end of a one-shot is the end of the cycle, not the
			// start of the next one (a ramp holds at its top).
			frac = 1.0;
			k -= 1.0;
		}
		v = wave_value(c->wave, frac, k + cycle);
	}
	return c->wave_ease ? an_ease_apply(c->wave_ease, v) : v;
}

static double clock_value(enum an_clock unit, bool step)
{
	struct timespec ts;
	timespec_get(&ts, TIME_UTC);
	const time_t now = ts.tv_sec;
	struct tm tm;
#ifdef _WIN32
	localtime_s(&tm, &now);
#else
	localtime_r(&now, &tm);
#endif
	const double ms = step ? 0.0 : ts.tv_nsec / 1e9;
	const double sec = tm.tm_sec + ms;
	const double min = tm.tm_min + (step ? 0.0 : sec / 60.0);
	const double hour = tm.tm_hour + (step ? 0.0 : min / 60.0);
	const double day_frac = step ? 0.0 : hour / 24.0;
	static const int month_days[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
	const int year = tm.tm_year + 1900;
	const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
	const int days = month_days[tm.tm_mon] + (tm.tm_mon == 1 && leap ? 1 : 0);

	switch (unit) {
	case CLOCK_MINUTE:
		return min / 60.0;
	case CLOCK_HOUR:
		return hour / 24.0;
	case CLOCK_HOUR12:
		return fmod(hour, 12.0) / 12.0;
	case CLOCK_WEEKDAY: // Monday = 0
		return (((tm.tm_wday + 6) % 7) + day_frac) / 7.0;
	case CLOCK_MONTHDAY:
		return ((tm.tm_mday - 1) + day_frac) / days;
	case CLOCK_MONTH:
		return (tm.tm_mon + (step ? 0.0 : ((tm.tm_mday - 1) + hour / 24.0) / days)) / 12.0;
	default:
		return sec / 60.0;
	}
}

static void audio_callback(void *param, obs_source_t *source, const struct audio_data *audio, bool muted)
{
	UNUSED_PARAMETER(source);
	struct animate_filter *f = param;
	float peak = 0.0f;
	if (!muted) {
		for (size_t ch = 0; ch < MAX_AV_PLANES; ch++) {
			const float *samples = (const float *)audio->data[ch];
			if (!samples)
				continue;
			for (uint32_t i = 0; i < audio->frames; i++) {
				const float v = fabsf(samples[i]);
				if (v > peak)
					peak = v;
			}
		}
	}
	pthread_mutex_lock(&f->mutex);
	const double floor_db = f->floor_db, ceiling_db = f->ceiling_db;
	pthread_mutex_unlock(&f->mutex);

	const double db = peak > 0.0f ? 20.0 * log10(peak) : -200.0;
	double level = (db - floor_db) / fmax(ceiling_db - floor_db, 0.1);
	level = level < 0.0 ? 0.0 : (level > 1.0 ? 1.0 : level);

	pthread_mutex_lock(&f->level_mutex);
	// Keep the highest level until the next tick reads it.
	if (level > f->level)
		f->level = (float)level;
	pthread_mutex_unlock(&f->level_mutex);
}

static void unbind_audio(struct animate_filter *f)
{
	if (f->audio_weak) {
		obs_source_t *source = obs_weak_source_get_source(f->audio_weak);
		if (source) {
			obs_source_remove_audio_capture_callback(source, audio_callback, f);
			obs_source_release(source);
		}
		obs_weak_source_release(f->audio_weak);
		f->audio_weak = NULL;
	}
	dstr_free(&f->audio_bound);
}

// (Re)binds the audio capture callback when the chosen source changes or
// appears. Called from video_tick.
static void bind_audio(struct animate_filter *f, const char *name, bool wanted)
{
	if (!wanted || !name || !*name) {
		unbind_audio(f);
		return;
	}
	if (f->audio_weak && f->audio_bound.array && strcmp(f->audio_bound.array, name) == 0) {
		obs_source_t *s = obs_weak_source_get_source(f->audio_weak);
		const bool alive = s != NULL;
		obs_source_release(s);
		if (alive)
			return;
	}
	unbind_audio(f);
	obs_source_t *source = obs_get_source_by_name(name);
	if (!source)
		return;
	obs_source_add_audio_capture_callback(source, audio_callback, f);
	f->audio_weak = obs_source_get_weak_source(source);
	dstr_copy(&f->audio_bound, name);
	obs_source_release(source);
}

static void probe_free(struct animate_filter *f)
{
	if (f->probe_tr || f->probe_ss[0] || f->probe_ss[1]) {
		obs_enter_graphics();
		gs_texrender_destroy(f->probe_tr);
		gs_stagesurface_destroy(f->probe_ss[0]);
		gs_stagesurface_destroy(f->probe_ss[1]);
		obs_leave_graphics();
	}
	f->probe_tr = NULL;
	f->probe_ss[0] = f->probe_ss[1] = NULL;
	f->probe_staged[0] = f->probe_staged[1] = false;
	bfree(f->probe_prev);
	f->probe_prev = NULL;
	f->probe_have_prev = false;
	f->probe_w = f->probe_h = 0;
}

// Reads back the copy staged on the previous frame (so the GPU never
// stalls), updating the average luma and the frame-to-frame difference.
static void probe_read(struct animate_filter *f, int index)
{
	uint8_t *data;
	uint32_t linesize;
	if (!f->probe_staged[index] || !gs_stagesurface_map(f->probe_ss[index], &data, &linesize))
		return;
	const uint32_t w = f->probe_w, h = f->probe_h;
	double sum = 0.0, diff = 0.0;
	for (uint32_t y = 0; y < h; y++) {
		const uint8_t *row = data + (size_t)y * linesize;
		for (uint32_t x = 0; x < w; x++) {
			const uint8_t *p = row + x * 4;
			// Rec. 709 luma of the (sRGB-encoded) colour, weighted by alpha.
			const float luma =
				(0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2]) * (p[3] / 255.0f) / 255.0f;
			sum += luma;
			float *prev = &f->probe_prev[y * w + x];
			diff += fabsf(luma - *prev);
			*prev = luma;
		}
	}
	gs_stagesurface_unmap(f->probe_ss[index]);
	const double n = (double)w * h;
	f->probe_luma = sum / n;
	f->probe_motion = f->probe_have_prev ? diff / n : 0.0;
	f->probe_have_prev = true;
	f->probe_staged[index] = false;
}

// Renders a small copy of `source` and reads back last frame's copy.
static void probe_source(struct animate_filter *f, obs_source_t *source)
{
	const uint32_t sw = obs_source_get_width(source), sh = obs_source_get_height(source);
	if (!sw || !sh)
		return;
	uint32_t w = PROBE_SIZE, h = PROBE_SIZE;
	if (sw >= sh)
		h = (uint32_t)fmax(1.0, round((double)PROBE_SIZE * sh / sw));
	else
		w = (uint32_t)fmax(1.0, round((double)PROBE_SIZE * sw / sh));

	obs_enter_graphics();
	if (w != f->probe_w || h != f->probe_h || !f->probe_tr) {
		gs_texrender_destroy(f->probe_tr);
		gs_stagesurface_destroy(f->probe_ss[0]);
		gs_stagesurface_destroy(f->probe_ss[1]);
		f->probe_tr = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
		f->probe_ss[0] = gs_stagesurface_create(w, h, GS_RGBA);
		f->probe_ss[1] = gs_stagesurface_create(w, h, GS_RGBA);
		f->probe_staged[0] = f->probe_staged[1] = false;
		f->probe_w = w;
		f->probe_h = h;
		bfree(f->probe_prev);
		f->probe_prev = bzalloc(sizeof(float) * w * h);
		f->probe_have_prev = false;
	}

	const int index = f->probe_index;
	probe_read(f, index ^ 1);

	gs_texrender_reset(f->probe_tr);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin_with_color_space(f->probe_tr, w, h, GS_CS_SRGB)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)sw, 0.0f, (float)sh, -100.0f, 100.0f);
		obs_source_video_render(source);
		gs_texrender_end(f->probe_tr);
		gs_stage_texture(f->probe_ss[index], gs_texrender_get_texture(f->probe_tr));
		f->probe_staged[index] = true;
	}
	gs_blend_state_pop();
	f->probe_index = index ^ 1;
	obs_leave_graphics();
}

// The source holding the setting to animate (new reference), or NULL.
static obs_source_t *get_target(struct animate_filter *f, const char *name)
{
	obs_source_t *parent = obs_filter_get_parent(f->context);
	if (!parent)
		return NULL;
	if (!name || !*name)
		return obs_source_get_ref(parent);
	obs_source_t *target = obs_source_get_filter_by_name(parent, name);
	if (target == f->context) {
		obs_source_release(target);
		return NULL;
	}
	return target;
}

static bool read_linked_setting(struct animate_filter *f, const struct an_config *c, double *value)
{
	if (!c->link_setting.len)
		return false;
	obs_source_t *source = get_target(f, c->link_target.array);
	if (!source)
		return false;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_item_t *item = obs_data_item_byname(settings, c->link_setting.array);
	bool ok = false;
	if (item && obs_data_item_gettype(item) == OBS_DATA_NUMBER) {
		*value = obs_data_item_get_double(item);
		ok = true;
	}
	obs_data_item_release(&item);
	obs_data_release(settings);
	obs_source_release(source);
	return ok;
}

/* ------------------------------------------------------------------------
 * Playback control and hotkeys
 * --------------------------------------------------------------------- */

static void request_play(struct animate_filter *f)
{
	pthread_mutex_lock(&f->mutex);
	f->cmd_play = true;
	f->cmd_stop = false;
	pthread_mutex_unlock(&f->mutex);
}

static void request_stop(struct animate_filter *f)
{
	pthread_mutex_lock(&f->mutex);
	f->cmd_stop = true;
	f->cmd_play = false;
	pthread_mutex_unlock(&f->mutex);
}

static void hotkey_play(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		request_play(data);
}

static void hotkey_stop(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed)
		request_stop(data);
}

static void unregister_hotkeys(struct animate_filter *f)
{
	if (f->hk_play != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(f->hk_play);
	if (f->hk_stop != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(f->hk_stop);
	f->hk_play = f->hk_stop = OBS_INVALID_HOTKEY_ID;
	obs_weak_source_release(f->hk_parent);
	f->hk_parent = NULL;
	dstr_free(&f->hk_name);
}

static obs_hotkey_id register_hotkey(struct animate_filter *f, obs_source_t *parent, const char *action,
				     const char *text_key, obs_hotkey_func func, obs_data_array_t *bindings)
{
	struct dstr name = {0}, desc = {0};
	dstr_printf(&name, "SpectrumAnimate.%s.%s", action, f->hk_name.array);
	dstr_printf(&desc, obs_module_text(text_key), f->hk_name.array);
	const obs_hotkey_id id = obs_hotkey_register_source(parent, name.array, desc.array, func, f);
	if (bindings && id != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_load(id, bindings);
	dstr_free(&name);
	dstr_free(&desc);
	return id;
}

// Registers "Play" and "Stop" hotkeys on the parent source (where OBS lists
// and saves them), and re-registers them, keeping their bindings, when the
// filter is renamed.
static void update_hotkeys(struct animate_filter *f, obs_source_t *parent)
{
	const char *name = obs_source_get_name(f->context);
	if (!parent || !name)
		return;
	const bool same_parent = f->hk_parent && obs_weak_source_references_source(f->hk_parent, parent);
	if (same_parent && f->hk_name.array && strcmp(f->hk_name.array, name) == 0)
		return;

	obs_data_array_t *play_bindings = NULL, *stop_bindings = NULL;
	if (same_parent) {
		play_bindings = obs_hotkey_save(f->hk_play);
		stop_bindings = obs_hotkey_save(f->hk_stop);
	}
	unregister_hotkeys(f);
	dstr_copy(&f->hk_name, name);
	f->hk_parent = obs_source_get_weak_source(parent);
	f->hk_play = register_hotkey(f, parent, "Play", "Animate.Hotkey.Play", hotkey_play, play_bindings);
	f->hk_stop = register_hotkey(f, parent, "Stop", "Animate.Hotkey.Stop", hotkey_stop, stop_bindings);
	obs_data_array_release(play_bindings);
	obs_data_array_release(stop_bindings);
}

/* ------------------------------------------------------------------------
 * obs_source_info callbacks
 * --------------------------------------------------------------------- */

static const char *animate_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("AnimateFilter");
}

static void animate_update(void *data, obs_data_t *settings)
{
	struct animate_filter *f = data;
	char key[64];
	pthread_mutex_lock(&f->mutex);
	struct an_config *c = &f->cfg;
	c->slots = (int)obs_data_get_int(settings, "an_slots");
	c->slots = c->slots < 1 ? 1 : (c->slots > AN_MAX_SLOTS ? AN_MAX_SLOTS : c->slots);
	for (int i = 0; i < AN_MAX_SLOTS; i++) {
		struct an_slot *s = &c->slot[i];
		dstr_copy(&s->target, obs_data_get_string(settings, slot_key(key, sizeof(key), "an_target", i)));
		dstr_copy(&s->setting, obs_data_get_string(settings, slot_key(key, sizeof(key), "an_setting", i)));
		s->min = obs_data_get_double(settings, slot_key(key, sizeof(key), "an_min", i));
		s->max = obs_data_get_double(settings, slot_key(key, sizeof(key), "an_max", i));
		s->offset = obs_data_get_double(settings, slot_key(key, sizeof(key), "an_offset", i)) / 100.0;
	}
	dstr_copy(&c->audio_name, obs_data_get_string(settings, "an_audio_source"));
	c->mode = (enum an_mode)obs_data_get_int(settings, "an_mode");
	c->wave = (enum an_wave)obs_data_get_int(settings, "an_wave");
	c->wave_ease = (int)obs_data_get_int(settings, "an_wave_ease");
	c->period = fmax(obs_data_get_double(settings, "an_period"), 0.01);
	c->phase = obs_data_get_double(settings, "an_phase") / 100.0;
	c->playback = (enum an_playback)obs_data_get_int(settings, "an_playback");
	c->trigger = (enum an_trigger)obs_data_get_int(settings, "an_trigger");
	c->end = (enum an_end)obs_data_get_int(settings, "an_end");
	c->key_count = an_keys_parse(obs_data_get_string(settings, "an_keys"), c->keys, AN_MAX_KEYS);
	f->floor_db = obs_data_get_double(settings, "an_floor_db");
	f->ceiling_db = obs_data_get_double(settings, "an_ceiling_db");
	c->attack = fmax(obs_data_get_double(settings, "an_attack") / 1000.0, 0.0);
	c->release = fmax(obs_data_get_double(settings, "an_release") / 1000.0, 0.0);
	c->clock = (enum an_clock)obs_data_get_int(settings, "an_clock_unit");
	c->clock_step = obs_data_get_bool(settings, "an_clock_step");
	c->mouse_axis = (int)obs_data_get_int(settings, "an_mouse_axis");
	c->mouse_screen = (int)obs_data_get_int(settings, "an_mouse_screen");
	c->mouse_smooth = fmax(obs_data_get_double(settings, "an_mouse_smooth") / 1000.0, 0.0);
	dstr_copy(&c->probe_name, obs_data_get_string(settings, "an_probe_source"));
	c->luma_lo = obs_data_get_double(settings, "an_luma_lo") / 100.0;
	c->luma_hi = obs_data_get_double(settings, "an_luma_hi") / 100.0;
	c->motion_full = fmax(obs_data_get_double(settings, "an_motion_full") / 100.0, 1e-4);
	dstr_copy(&c->link_target, obs_data_get_string(settings, "an_link_target"));
	dstr_copy(&c->link_setting, obs_data_get_string(settings, "an_link_setting"));
	c->link_scale = obs_data_get_double(settings, "an_link_scale");
	c->link_offset = obs_data_get_double(settings, "an_link_offset");
	f->reset_values = true;
	pthread_mutex_unlock(&f->mutex);
}

static void *animate_create(obs_data_t *settings, obs_source_t *source)
{
	struct animate_filter *f = bzalloc(sizeof(struct animate_filter));
	f->context = source;
	f->hk_play = f->hk_stop = OBS_INVALID_HOTKEY_ID;
	pthread_mutex_init(&f->mutex, NULL);
	pthread_mutex_init(&f->level_mutex, NULL);
	f->last_trigger = f->last_playback = f->last_mode = -1;
	animate_update(f, settings);
	return f;
}

static void animate_destroy(void *data)
{
	struct animate_filter *f = data;
	unregister_hotkeys(f);
	unbind_audio(f);
	probe_free(f);
	config_free(&f->cfg);
	pthread_mutex_destroy(&f->level_mutex);
	pthread_mutex_destroy(&f->mutex);
	bfree(f);
}

static void animate_defaults(obs_data_t *settings)
{
	char key[64];
	obs_data_set_default_int(settings, "an_slots", 1);
	for (int i = 0; i < AN_MAX_SLOTS; i++) {
		obs_data_set_default_string(settings, slot_key(key, sizeof(key), "an_target", i), SELF_TARGET);
		obs_data_set_default_string(settings, slot_key(key, sizeof(key), "an_setting", i), "");
		obs_data_set_default_double(settings, slot_key(key, sizeof(key), "an_min", i), 0.0);
		obs_data_set_default_double(settings, slot_key(key, sizeof(key), "an_max", i), 100.0);
		obs_data_set_default_double(settings, slot_key(key, sizeof(key), "an_offset", i), 0.0);
	}
	obs_data_set_default_int(settings, "an_mode", MODE_LOOP);
	obs_data_set_default_int(settings, "an_wave", WAVE_SINE);
	obs_data_set_default_int(settings, "an_wave_ease", AN_EASE_LINEAR);
	obs_data_set_default_double(settings, "an_period", 2.0);
	obs_data_set_default_double(settings, "an_phase", 0.0);
	obs_data_set_default_int(settings, "an_playback", PLAY_LOOP);
	obs_data_set_default_int(settings, "an_trigger", TRIGGER_AUTO);
	obs_data_set_default_int(settings, "an_end", END_HOLD);
	obs_data_set_default_string(settings, "an_keys", "0 0\n1 100 out\n2 0 bounce");
	obs_data_set_default_double(settings, "an_floor_db", -50.0);
	obs_data_set_default_double(settings, "an_ceiling_db", -6.0);
	obs_data_set_default_double(settings, "an_attack", 30.0);
	obs_data_set_default_double(settings, "an_release", 250.0);
	obs_data_set_default_int(settings, "an_clock_unit", CLOCK_SECOND);
	obs_data_set_default_bool(settings, "an_clock_step", false);
	obs_data_set_default_int(settings, "an_mouse_axis", 0);
	obs_data_set_default_int(settings, "an_mouse_screen", 0);
	obs_data_set_default_double(settings, "an_mouse_smooth", 60.0);
	obs_data_set_default_string(settings, "an_probe_source", "");
	obs_data_set_default_double(settings, "an_luma_lo", 0.0);
	obs_data_set_default_double(settings, "an_luma_hi", 100.0);
	obs_data_set_default_double(settings, "an_motion_full", 10.0);
	obs_data_set_default_string(settings, "an_link_target", SELF_TARGET);
	obs_data_set_default_string(settings, "an_link_setting", "");
	obs_data_set_default_double(settings, "an_link_scale", 1.0);
	obs_data_set_default_double(settings, "an_link_offset", 0.0);
}

// Advances the playhead of the time-based drivers and handles the triggers.
static void advance_playback(struct animate_filter *f, const struct an_config *c, obs_source_t *parent, bool cmd_play,
			     bool cmd_stop, bool enabled_edge, float seconds)
{
	const double length = cycle_length(c);
	bool restart = cmd_play;
	bool stop = cmd_stop;

	// Starting, or the trigger or playback mode changed: automatic
	// animations run, the others wait for their trigger.
	if ((int)c->trigger != f->last_trigger || (int)c->playback != f->last_playback ||
	    (int)c->mode != f->last_mode) {
		f->last_trigger = (int)c->trigger;
		f->last_playback = (int)c->playback;
		f->last_mode = (int)c->mode;
		f->playing = c->trigger == TRIGGER_AUTO;
		f->time = 0.0;
		f->was_shown = f->was_active = false;
	}

	// Show / go-live triggers: start on the rising edge, and stop a
	// repeating animation (ready to start afresh) on the falling one.
	const bool shown = parent && obs_source_showing(parent);
	const bool active = parent && obs_source_active(parent);
	if (c->trigger == TRIGGER_SHOW || c->trigger == TRIGGER_ACTIVE) {
		const bool now = c->trigger == TRIGGER_SHOW ? shown : active;
		const bool before = c->trigger == TRIGGER_SHOW ? f->was_shown : f->was_active;
		if (now && !before)
			restart = true;
		else if (!now && before && c->playback != PLAY_ONCE) {
			f->playing = false;
			f->time = 0.0;
		}
	}
	f->was_shown = shown;
	f->was_active = active;

	// A one-shot that starts automatically replays when the filter is
	// re-enabled.
	if (enabled_edge && c->trigger == TRIGGER_AUTO && c->playback == PLAY_ONCE)
		restart = true;

	if (restart) {
		f->time = 0.0;
		f->playing = true;
	} else if (stop) {
		f->playing = false;
		if (c->end == END_RETURN)
			f->time = 0.0;
	}

	if (!f->playing)
		return;

	if (c->playback == PLAY_ONCE) {
		double max_offset = 0.0;
		for (int i = 0; i < c->slots; i++)
			max_offset = fmax(max_offset, c->slot[i].offset);
		const double end = length * (1.0 + max_offset);
		f->time += seconds;
		if (f->time >= end) {
			f->playing = false;
			f->time = c->end == END_RETURN ? 0.0 : end;
		}
	} else if (c->mode == MODE_LOOP && c->playback == PLAY_LOOP) {
		f->time = fmod(f->time + seconds, length * 1000003.0);
	} else if (length > 0.0) {
		const double wrap = c->playback == PLAY_PINGPONG ? 2.0 * length : length;
		f->time = fmod(f->time + seconds, wrap * 1000003.0);
	}
}

static void apply_value(struct animate_filter *f, int slot, const struct an_slot *s, double value)
{
	if (!s->setting.len || (f->have_last[slot] && fabs(value - f->last_value[slot]) <= 1e-9))
		return;
	obs_source_t *target = get_target(f, s->target.array);
	if (!target)
		return;
	obs_data_t *current = obs_source_get_settings(target);
	obs_data_item_t *item = obs_data_item_byname(current, s->setting.array);
	const bool is_int = item && obs_data_item_numtype(item) == OBS_DATA_NUM_INT;
	obs_data_item_release(&item);
	obs_data_release(current);

	obs_data_t *update = obs_data_create();
	if (is_int)
		obs_data_set_int(update, s->setting.array, (long long)llround(value));
	else
		obs_data_set_double(update, s->setting.array, value);
	obs_source_update(target, update);
	obs_data_release(update);
	obs_source_release(target);
	f->last_value[slot] = value;
	f->have_last[slot] = true;
}

static double smooth(double current, double target, double seconds, double up, double down)
{
	const double tc = target > current ? up : down;
	const double k = tc > 0.0 ? 1.0 - exp(-seconds / tc) : 1.0;
	return current + (target - current) * k;
}

static void animate_tick(void *data, float seconds)
{
	struct animate_filter *f = data;
	const bool enabled = obs_source_enabled(f->context);
	const bool enabled_edge = enabled && !f->was_enabled;
	f->was_enabled = enabled;
	if (!enabled)
		return;

	struct an_config c = {0};
	pthread_mutex_lock(&f->mutex);
	config_copy(&c, &f->cfg);
	const bool cmd_play = f->cmd_play, cmd_stop = f->cmd_stop;
	f->cmd_play = f->cmd_stop = false;
	if (f->reset_values) {
		memset(f->have_last, 0, sizeof(f->have_last));
		f->reset_values = false;
	}
	pthread_mutex_unlock(&f->mutex);

	obs_source_t *parent = obs_filter_get_parent(f->context);
	update_hotkeys(f, parent);
	bind_audio(f, c.audio_name.array, c.mode == MODE_AUDIO);

	double v = 0.0;        // driver value, 0..1
	bool absolute = false; // MODE_LINK: v is the final value
	bool have_value = true;

	switch (c.mode) {
	case MODE_AUDIO: {
		pthread_mutex_lock(&f->level_mutex);
		const double level = f->level;
		f->level = 0.0f;
		pthread_mutex_unlock(&f->level_mutex);
		f->envelope = smooth(f->envelope, level, seconds, c.attack, c.release);
		v = f->envelope;
		break;
	}
	case MODE_CLOCK:
		v = clock_value(c.clock, c.clock_step);
		break;
	case MODE_MOUSE: {
		double x, y;
		if (spectrum_cursor_position(c.mouse_screen, &x, &y)) {
			const double target = c.mouse_axis ? y : x;
			f->envelope = f->have_mouse
					      ? smooth(f->envelope, target, seconds, c.mouse_smooth, c.mouse_smooth)
					      : target;
			f->have_mouse = true;
		}
		// Leave the targets alone until the first position arrives.
		have_value = f->have_mouse;
		v = f->envelope;
		break;
	}
	case MODE_BRIGHTNESS:
	case MODE_MOTION: {
		obs_source_t *probe = c.probe_name.len ? obs_get_source_by_name(c.probe_name.array) : NULL;
		if (probe) {
			probe_source(f, probe);
			obs_source_release(probe);
		}
		double level;
		if (c.mode == MODE_BRIGHTNESS)
			level = (f->probe_luma - c.luma_lo) /
				(fabs(c.luma_hi - c.luma_lo) < 1e-6 ? 1e-6 : c.luma_hi - c.luma_lo);
		else
			level = f->probe_motion / c.motion_full;
		level = level < 0.0 ? 0.0 : (level > 1.0 ? 1.0 : level);
		f->envelope = smooth(f->envelope, level, seconds, c.attack, c.release);
		v = f->envelope;
		break;
	}
	case MODE_LINK: {
		double linked;
		have_value = read_linked_setting(f, &c, &linked);
		v = have_value ? linked * c.link_scale + c.link_offset : 0.0;
		absolute = true;
		break;
	}
	default: // MODE_LOOP, MODE_KEYFRAMES
		advance_playback(f, &c, parent, cmd_play, cmd_stop, enabled_edge, seconds);
		break;
	}
	if (c.mode != MODE_BRIGHTNESS && c.mode != MODE_MOTION && f->probe_tr)
		probe_free(f);

	if (have_value) {
		for (int i = 0; i < c.slots; i++) {
			const struct an_slot *s = &c.slot[i];
			if (absolute) {
				// Never feed a setting back into itself.
				if (strcmp(s->target.array ? s->target.array : "",
					   c.link_target.array ? c.link_target.array : "") == 0 &&
				    s->setting.array && c.link_setting.array &&
				    strcmp(s->setting.array, c.link_setting.array) == 0)
					continue;
				apply_value(f, i, s, v);
				continue;
			}
			const double u = is_time_mode(c.mode) ? time_driver_value(&c, f->time, s->offset) : v;
			apply_value(f, i, s, s->min + (s->max - s->min) * u);
		}
	}
	config_free(&c);
}

static void animate_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct animate_filter *f = data;
	obs_source_skip_video_filter(f->context);
}

/* ------------------------------------------------------------------------
 * Properties
 * --------------------------------------------------------------------- */

struct list_ctx {
	obs_property_t *list;
	obs_source_t *self;
};

static void add_filter_to_list(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct list_ctx *ctx = param;
	if (child == ctx->self)
		return;
	const char *name = obs_source_get_name(child);
	obs_property_list_add_string(ctx->list, name, name);
}

static void fill_target_list(struct animate_filter *f, obs_property_t *p)
{
	obs_property_list_add_string(p, obs_module_text("Animate.ThisSource"), SELF_TARGET);
	obs_source_t *parent = f ? obs_filter_get_parent(f->context) : NULL;
	if (parent) {
		struct list_ctx ctx = {p, f->context};
		obs_source_enum_filters(parent, add_filter_to_list, &ctx);
	}
}

// Adds the numeric properties of `props` (recursing into groups).
static void add_numeric_properties(obs_property_t *list, obs_properties_t *props, const char *prefix)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		const enum obs_property_type type = obs_property_get_type(p);
		const char *desc = obs_property_description(p);
		if (type == OBS_PROPERTY_GROUP) {
			struct dstr sub = {0};
			dstr_printf(&sub, "%s%s / ", prefix, desc ? desc : "");
			add_numeric_properties(list, obs_property_group_content(p), sub.array);
			dstr_free(&sub);
		} else if (type == OBS_PROPERTY_FLOAT || type == OBS_PROPERTY_INT) {
			const char *name = obs_property_name(p);
			struct dstr label = {0};
			dstr_printf(&label, "%s%s", prefix, desc && *desc ? desc : name);
			obs_property_list_add_string(list, label.array, name);
			dstr_free(&label);
		}
	}
}

static void fill_setting_list(struct animate_filter *f, obs_property_t *list, const char *target_name)
{
	if (!list)
		return;
	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("Animate.ChooseSetting"), "");
	if (!f)
		return;
	obs_source_t *target = get_target(f, target_name);
	if (!target)
		return;
	obs_properties_t *props = obs_source_properties(target);
	if (props) {
		add_numeric_properties(list, props, "");
		obs_properties_destroy(props);
	}
	obs_source_release(target);
}

static bool target_modified(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	char key[64];
	const char *name = obs_property_name(p);
	if (strcmp(name, "an_link_target") == 0) {
		fill_setting_list(priv, obs_properties_get(props, "an_link_setting"),
				  obs_data_get_string(settings, "an_link_target"));
		return true;
	}
	const int slot = key_slot(name, "an_target");
	if (slot < 0)
		return false;
	fill_setting_list(priv, obs_properties_get(props, slot_key(key, sizeof(key), "an_setting", slot)),
			  obs_data_get_string(settings, name));
	return true;
}

static bool use_range_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	struct animate_filter *f = data;
	char key[64];
	const int slot = key_slot(obs_property_name(p), "an_use_range");
	if (!f || slot < 0)
		return false;
	obs_data_t *settings = obs_source_get_settings(f->context);
	obs_source_t *target =
		get_target(f, obs_data_get_string(settings, slot_key(key, sizeof(key), "an_target", slot)));
	bool changed = false;
	if (target) {
		obs_properties_t *tprops = obs_source_properties(target);
		obs_property_t *tp =
			tprops ? obs_properties_get(tprops, obs_data_get_string(settings, slot_key(key, sizeof(key),
												   "an_setting", slot)))
			       : NULL;
		double lo = 0.0, hi = 0.0;
		if (tp && obs_property_get_type(tp) == OBS_PROPERTY_FLOAT) {
			lo = obs_property_float_min(tp);
			hi = obs_property_float_max(tp);
			changed = true;
		} else if (tp && obs_property_get_type(tp) == OBS_PROPERTY_INT) {
			lo = obs_property_int_min(tp);
			hi = obs_property_int_max(tp);
			changed = true;
		}
		if (changed) {
			obs_data_set_double(settings, slot_key(key, sizeof(key), "an_min", slot), lo);
			obs_data_set_double(settings, slot_key(key, sizeof(key), "an_max", slot), hi);
		}
		obs_properties_destroy(tprops);
		obs_source_release(target);
	}
	if (changed)
		obs_source_update(f->context, settings);
	obs_data_release(settings);
	return changed;
}

static bool play_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	if (data)
		request_play(data);
	return false;
}

static bool stop_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	if (data)
		request_stop(data);
	return false;
}

static bool curve_editor_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct animate_filter *f = data;
	return f && spectrum_curve_editor_open(f->context);
}

static void set_visible(obs_properties_t *props, const char *const *keys, bool visible)
{
	for (size_t i = 0; keys[i]; i++)
		obs_property_set_visible(obs_properties_get(props, keys[i]), visible);
}

static bool visibility_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const enum an_mode mode = (enum an_mode)obs_data_get_int(settings, "an_mode");
	const bool timed = is_time_mode(mode);
	const bool once = obs_data_get_int(settings, "an_playback") == PLAY_ONCE;

	static const char *wave_keys[] = {"an_wave", "an_wave_ease", "an_period", NULL};
	static const char *key_keys[] = {"an_keys", "an_curve_editor", NULL};
	static const char *time_keys[] = {"an_phase", "an_playback", "an_trigger", "an_play", "an_stop", NULL};
	static const char *audio_keys[] = {"an_audio_source", "an_floor_db", "an_ceiling_db", NULL};
	static const char *envelope_keys[] = {"an_attack", "an_release", NULL};
	static const char *clock_keys[] = {"an_clock_unit", "an_clock_step", NULL};
	static const char *mouse_keys[] = {"an_mouse_axis", "an_mouse_screen", "an_mouse_smooth", NULL};
	static const char *luma_keys[] = {"an_luma_lo", "an_luma_hi", NULL};
	static const char *motion_keys[] = {"an_motion_full", NULL};
	static const char *link_keys[] = {"an_link_target", "an_link_setting", "an_link_scale", "an_link_offset", NULL};

	set_visible(props, wave_keys, mode == MODE_LOOP);
	set_visible(props, key_keys, mode == MODE_KEYFRAMES);
	set_visible(props, time_keys, timed);
	obs_property_set_visible(obs_properties_get(props, "an_end"), timed && once);
	set_visible(props, audio_keys, mode == MODE_AUDIO);
	set_visible(props, envelope_keys, mode == MODE_AUDIO || mode == MODE_BRIGHTNESS || mode == MODE_MOTION);
	set_visible(props, clock_keys, mode == MODE_CLOCK);
	set_visible(props, mouse_keys, mode == MODE_MOUSE);
	obs_property_set_visible(obs_properties_get(props, "an_probe_source"),
				 mode == MODE_BRIGHTNESS || mode == MODE_MOTION);
	set_visible(props, luma_keys, mode == MODE_BRIGHTNESS);
	set_visible(props, motion_keys, mode == MODE_MOTION);
	set_visible(props, link_keys, mode == MODE_LINK);

	const int slots = (int)obs_data_get_int(settings, "an_slots");
	char key[64];
	for (int i = 0; i < AN_MAX_SLOTS; i++) {
		obs_property_set_visible(obs_properties_get(props, slot_key(key, sizeof(key), "an_slot_group", i)),
					 i < slots);
		static const char *range_keys[] = {"an_min", "an_max", "an_use_range", NULL};
		for (size_t k = 0; range_keys[k]; k++)
			obs_property_set_visible(obs_properties_get(props,
								    slot_key(key, sizeof(key), range_keys[k], i)),
						 mode != MODE_LINK);
		obs_property_set_visible(obs_properties_get(props, slot_key(key, sizeof(key), "an_offset", i)), timed);
	}
	return true;
}

static bool add_audio_source(void *param, obs_source_t *source)
{
	if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(source);
		obs_property_list_add_string(param, name, name);
	}
	return true;
}

static bool add_video_source(void *param, obs_source_t *source)
{
	if (obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) {
		const char *name = obs_source_get_name(source);
		obs_property_list_add_string(param, name, name);
	}
	return true;
}

static void add_screen(void *param, int index, const char *name)
{
	obs_property_list_add_int(param, name, index);
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static void add_slot_properties(obs_properties_t *props, struct animate_filter *f, obs_data_t *settings, int slot)
{
	char key[64], text[64];
	obs_properties_t *group = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(group, slot_key(key, sizeof(key), "an_target", slot),
				    obs_module_text("Animate.Target"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_target_list(f, p);
	obs_property_set_modified_callback2(p, target_modified, f);

	p = obs_properties_add_list(group, slot_key(key, sizeof(key), "an_setting", slot),
				    obs_module_text("Animate.Setting"), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_setting_list(f, p,
			  settings ? obs_data_get_string(settings, slot_key(key, sizeof(key), "an_target", slot)) : "");

	slider(group, slot_key(key, sizeof(key), "an_min", slot), "Animate.Min", -10000.0, 10000.0, 0.01, NULL);
	slider(group, slot_key(key, sizeof(key), "an_max", slot), "Animate.Max", -10000.0, 10000.0, 0.01, NULL);
	obs_properties_add_button2(group, slot_key(key, sizeof(key), "an_use_range", slot),
				   obs_module_text("Animate.UseRange"), use_range_clicked, f);
	p = slider(group, slot_key(key, sizeof(key), "an_offset", slot), "Animate.Offset", 0.0, 100.0, 0.1, "%");
	obs_property_set_long_description(p, obs_module_text("Animate.Offset.Description"));

	snprintf(text, sizeof(text), obs_module_text("Animate.TargetN"), slot + 1);
	obs_properties_add_group(props, slot_key(key, sizeof(key), "an_slot_group", slot), text, OBS_GROUP_NORMAL,
				 group);
}

static obs_properties_t *animate_properties(void *data)
{
	struct animate_filter *f = data;
	obs_properties_t *props = obs_properties_create();
	obs_data_t *settings = f ? obs_source_get_settings(f->context) : NULL;
	obs_property_t *p;

	p = obs_properties_add_int(props, "an_slots", obs_module_text("Animate.Slots"), 1, AN_MAX_SLOTS, 1);
	obs_property_set_modified_callback(p, visibility_modified);
	for (int i = 0; i < AN_MAX_SLOTS; i++)
		add_slot_properties(props, f, settings, i);

	p = obs_properties_add_list(props, "an_mode", obs_module_text("Animate.Mode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Animate.Mode.Loop"), MODE_LOOP);
	obs_property_list_add_int(p, obs_module_text("Animate.Mode.Keyframes"), MODE_KEYFRAMES);
	obs_property_list_add_int(p, obs_module_text("Animate.Mode.Audio"), MODE_AUDIO);
	obs_property_list_add_int(p, obs_module_text("Animate.Mode.Clock"), MODE_CLOCK);
	obs_property_list_add_int(p, obs_module_text("Animate.Mode.Mouse"), MODE_MOUSE);
	obs_property_list_add_int(p, obs_module_text("Animate.Mode.Brightness"), MODE_BRIGHTNESS);
	obs_property_list_add_int(p, obs_module_text("Animate.Mode.Motion"), MODE_MOTION);
	obs_property_list_add_int(p, obs_module_text("Animate.Mode.Link"), MODE_LINK);
	obs_property_set_modified_callback(p, visibility_modified);

	// Waveform
	p = obs_properties_add_list(props, "an_wave", obs_module_text("Animate.Wave"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.Sine"), WAVE_SINE);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.Triangle"), WAVE_TRIANGLE);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.SawUp"), WAVE_SAW_UP);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.SawDown"), WAVE_SAW_DOWN);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.Square"), WAVE_SQUARE);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.Random"), WAVE_RANDOM);
	p = obs_properties_add_list(props, "an_wave_ease", obs_module_text("Animate.WaveEase"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	for (int e = 0; e < AN_EASE_COUNT; e++)
		if (e != AN_EASE_STEP)
			obs_property_list_add_int(p, obs_module_text(an_ease_text_key(e)), e);
	obs_property_set_long_description(p, obs_module_text("Animate.WaveEase.Description"));
	slider(props, "an_period", "Animate.Period", 0.05, 120.0, 0.01, "s");

	// Keyframes
	p = obs_properties_add_text(props, "an_keys", obs_module_text("Animate.Keys"), OBS_TEXT_MULTILINE);
	obs_property_set_long_description(p, obs_module_text("Animate.Keys.Description"));
	obs_properties_add_button2(props, "an_curve_editor", obs_module_text("Animate.CurveEditor"),
				   curve_editor_clicked, f);

	// Playback (waveform and keyframes)
	slider(props, "an_phase", "Animate.Phase", 0.0, 100.0, 0.1, "%");
	p = obs_properties_add_list(props, "an_playback", obs_module_text("Animate.Playback"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Animate.Playback.Loop"), PLAY_LOOP);
	obs_property_list_add_int(p, obs_module_text("Animate.Playback.PingPong"), PLAY_PINGPONG);
	obs_property_list_add_int(p, obs_module_text("Animate.Playback.Once"), PLAY_ONCE);
	obs_property_set_modified_callback(p, visibility_modified);
	p = obs_properties_add_list(props, "an_trigger", obs_module_text("Animate.Trigger"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Animate.Trigger.Auto"), TRIGGER_AUTO);
	obs_property_list_add_int(p, obs_module_text("Animate.Trigger.Hotkey"), TRIGGER_HOTKEY);
	obs_property_list_add_int(p, obs_module_text("Animate.Trigger.Show"), TRIGGER_SHOW);
	obs_property_list_add_int(p, obs_module_text("Animate.Trigger.Active"), TRIGGER_ACTIVE);
	obs_property_set_long_description(p, obs_module_text("Animate.Trigger.Description"));
	p = obs_properties_add_list(props, "an_end", obs_module_text("Animate.End"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Animate.End.Hold"), END_HOLD);
	obs_property_list_add_int(p, obs_module_text("Animate.End.Return"), END_RETURN);
	obs_properties_add_button2(props, "an_play", obs_module_text("Animate.Play"), play_clicked, f);
	obs_properties_add_button2(props, "an_stop", obs_module_text("Animate.Stop"), stop_clicked, f);

	// Audio
	p = obs_properties_add_list(props, "an_audio_source", obs_module_text("Animate.AudioSource"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Animate.ChooseAudio"), "");
	obs_enum_sources(add_audio_source, p);
	slider(props, "an_floor_db", "Animate.Floor", -100.0, 0.0, 0.1, "dB");
	slider(props, "an_ceiling_db", "Animate.Ceiling", -100.0, 0.0, 0.1, "dB");

	// Clock
	p = obs_properties_add_list(props, "an_clock_unit", obs_module_text("Animate.Clock"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Animate.Clock.Second"), CLOCK_SECOND);
	obs_property_list_add_int(p, obs_module_text("Animate.Clock.Minute"), CLOCK_MINUTE);
	obs_property_list_add_int(p, obs_module_text("Animate.Clock.Hour"), CLOCK_HOUR);
	obs_property_list_add_int(p, obs_module_text("Animate.Clock.Hour12"), CLOCK_HOUR12);
	obs_property_list_add_int(p, obs_module_text("Animate.Clock.Weekday"), CLOCK_WEEKDAY);
	obs_property_list_add_int(p, obs_module_text("Animate.Clock.MonthDay"), CLOCK_MONTHDAY);
	obs_property_list_add_int(p, obs_module_text("Animate.Clock.Month"), CLOCK_MONTH);
	obs_property_set_long_description(p, obs_module_text("Animate.Clock.Description"));
	obs_properties_add_bool(props, "an_clock_step", obs_module_text("Animate.Clock.Step"));

	// Mouse
	p = obs_properties_add_list(props, "an_mouse_axis", obs_module_text("Animate.Mouse.Axis"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Animate.Mouse.X"), 0);
	obs_property_list_add_int(p, obs_module_text("Animate.Mouse.Y"), 1);
	p = obs_properties_add_list(props, "an_mouse_screen", obs_module_text("Animate.Mouse.Screen"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Animate.Mouse.Primary"), 0);
	obs_property_list_add_int(p, obs_module_text("Animate.Mouse.Desktop"), -1);
	spectrum_cursor_list_screens(add_screen, p);
	obs_property_set_long_description(p, obs_module_text("Animate.Mouse.Description"));
	slider(props, "an_mouse_smooth", "Animate.Mouse.Smooth", 0.0, 2000.0, 1.0, "ms");

	// Brightness / motion of a source
	p = obs_properties_add_list(props, "an_probe_source", obs_module_text("Animate.ProbeSource"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Animate.ChooseSource"), "");
	obs_enum_scenes(add_video_source, p);
	obs_enum_sources(add_video_source, p);
	slider(props, "an_luma_lo", "Animate.LumaLow", 0.0, 100.0, 0.1, "%");
	slider(props, "an_luma_hi", "Animate.LumaHigh", 0.0, 100.0, 0.1, "%");
	p = slider(props, "an_motion_full", "Animate.MotionFull", 0.1, 100.0, 0.1, "%");
	obs_property_set_long_description(p, obs_module_text("Animate.MotionFull.Description"));

	// Attack / release (audio, brightness, motion)
	slider(props, "an_attack", "Animate.Attack", 0.0, 2000.0, 1.0, "ms");
	slider(props, "an_release", "Animate.Release", 0.0, 5000.0, 1.0, "ms");

	// Linked setting
	p = obs_properties_add_list(props, "an_link_target", obs_module_text("Animate.LinkTarget"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	fill_target_list(f, p);
	obs_property_set_modified_callback2(p, target_modified, f);
	p = obs_properties_add_list(props, "an_link_setting", obs_module_text("Animate.LinkSetting"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_setting_list(f, p, settings ? obs_data_get_string(settings, "an_link_target") : "");
	slider(props, "an_link_scale", "Animate.LinkScale", -100.0, 100.0, 0.001, NULL);
	slider(props, "an_link_offset", "Animate.LinkOffset", -10000.0, 10000.0, 0.01, NULL);

	obs_data_release(settings);
	spectrum_presets_properties(props, f ? f->context : NULL);
	return props;
}

struct obs_source_info spectrum_animate_filter = {
	.id = "spectrum_animate_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO,
	.get_name = animate_name,
	.create = animate_create,
	.destroy = animate_destroy,
	.update = animate_update,
	.video_tick = animate_tick,
	.video_render = animate_render,
	.get_properties = animate_properties,
	.get_defaults = animate_defaults,
};
