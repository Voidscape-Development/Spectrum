// Spectrum Text: a text source drawn from a signed distance field, so the
// fill, outline, glow and drop shadow stay crisp at any size, with per
// character animations (typewriter, fades, slides, pops, wave, bounce,
// shake, rainbow).
//
// Layout and the glyph distance field atlas are built on a worker thread
// (text-layout.cpp); every frame, each glyph is drawn as its own quad with
// its own transform, alpha and hue.

#include "text-layout.hpp"

#include <obs-module.h>
#include <graphics/matrix4.h>
#include <util/platform.h>

#include "../common/fx-filter.h"
#include "../common/presets.h"
extern "C" {
#include "../stroke/obs-utils.h"
}

#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using spectrum_text::GlyphEntry;
using spectrum_text::LayoutParams;
using spectrum_text::LayoutResult;

#define TX_FILL_PREFIX "tx_gradient_"
#define TX_OUTLINE_PREFIX "tx_ogradient_"

namespace {

enum { FILL_NONE = 0, FILL_COLOR = 1, FILL_GRADIENT = 2, FILL_SOURCE = 3 };
enum { OUTLINE_INSIDE = 1, OUTLINE_CENTER = 2, OUTLINE_OUTSIDE = 3 };
enum { OUTLINE_COLOR = 1, OUTLINE_GRADIENT = 2 };
enum { IN_NONE = 0, IN_TYPEWRITER = 1, IN_FADE = 2, IN_SLIDE = 3, IN_SCALE = 4, IN_DROP = 5 };
enum { DIR_UP = 1, DIR_DOWN = 2, DIR_LEFT = 3, DIR_RIGHT = 4 };
enum { MOTION_NONE = 0, MOTION_WAVE = 1, MOTION_BOUNCE = 2, MOTION_SHAKE = 3, MOTION_JITTER = 4 };

constexpr float kPi = 3.14159265358979f;
constexpr float kTau = 6.28318530717959f;

struct Style {
	int fill_type;
	bool outline, glow, shadow, rainbow;
	float outline_width;
	int outline_position;
	float glow_size, shadow_angle, shadow_distance, shadow_softness;
	float rainbow_speed, rainbow_spread;

	int anim_in;
	int slide_dir;
	float slide_distance, stagger, duration, delay, hold;
	bool loop, cursor;
	int motion;
	float motion_amount, motion_speed, motion_spread;

	float outline_outer() const
	{
		if (!outline || outline_width <= 0.0f)
			return 0.0f;
		if (outline_position == OUTLINE_OUTSIDE)
			return outline_width;
		if (outline_position == OUTLINE_CENTER)
			return outline_width * 0.5f;
		return 0.0f;
	}
	// Distance field needed outside the glyphs.
	float spread() const
	{
		float s = outline_outer();
		float e = 0.0f;
		if (glow)
			e = std::max(e, glow_size);
		if (shadow)
			e = std::max(e, shadow_softness);
		return s + e + 2.0f;
	}
	// Margin around the text box for the effects.
	float margin() const
	{
		float e = 0.0f;
		if (glow)
			e = std::max(e, glow_size);
		if (shadow)
			e = std::max(e, shadow_distance + shadow_softness);
		float m = outline_outer() + e;
		if (motion != MOTION_NONE)
			m += motion_amount;
		return std::ceil(std::max(m, 0.0f));
	}
};

Style read_style(obs_data_t *s)
{
	Style st;
	st.fill_type = (int)obs_data_get_int(s, "tx_fill_type");
	st.outline = obs_data_get_bool(s, "tx_outline");
	st.outline_width = (float)obs_data_get_double(s, "tx_outline_width");
	st.outline_position = (int)obs_data_get_int(s, "tx_outline_position");
	st.glow = obs_data_get_bool(s, "tx_glow");
	st.glow_size = std::max((float)obs_data_get_double(s, "tx_glow_size"), 0.0f);
	st.shadow = obs_data_get_bool(s, "tx_shadow");
	st.shadow_angle = (float)obs_data_get_double(s, "tx_shadow_angle");
	st.shadow_distance = std::max((float)obs_data_get_double(s, "tx_shadow_distance"), 0.0f);
	st.shadow_softness = std::max((float)obs_data_get_double(s, "tx_shadow_softness"), 0.0f);
	st.rainbow = obs_data_get_bool(s, "tx_rainbow");
	st.rainbow_speed = (float)obs_data_get_double(s, "tx_rainbow_speed");
	st.rainbow_spread = (float)obs_data_get_double(s, "tx_rainbow_spread");
	st.anim_in = (int)obs_data_get_int(s, "tx_anim_in");
	st.slide_dir = (int)obs_data_get_int(s, "tx_slide_direction");
	st.slide_distance = (float)obs_data_get_double(s, "tx_slide_distance");
	st.stagger = std::max((float)obs_data_get_double(s, "tx_stagger"), 0.0f);
	st.duration = std::max((float)obs_data_get_double(s, "tx_duration"), 0.01f);
	st.delay = std::max((float)obs_data_get_double(s, "tx_delay"), 0.0f);
	st.hold = std::max((float)obs_data_get_double(s, "tx_loop_hold"), 0.0f);
	st.loop = obs_data_get_bool(s, "tx_loop");
	st.cursor = obs_data_get_bool(s, "tx_cursor");
	st.motion = (int)obs_data_get_int(s, "tx_motion");
	st.motion_amount = (float)obs_data_get_double(s, "tx_motion_amount");
	st.motion_speed = (float)obs_data_get_double(s, "tx_motion_speed");
	st.motion_spread = (float)obs_data_get_double(s, "tx_motion_spread");
	return st;
}

/* Animation helpers ---------------------------------------------------- */

float saturate(float x)
{
	return std::min(std::max(x, 0.0f), 1.0f);
}

float ease_out_cubic(float p)
{
	const float q = 1.0f - p;
	return 1.0f - q * q * q;
}

float ease_out_back(float p)
{
	const float c1 = 1.70158f, c3 = c1 + 1.0f;
	const float q = p - 1.0f;
	return 1.0f + c3 * q * q * q + c1 * q * q;
}

float ease_out_bounce(float p)
{
	const float n1 = 7.5625f, d1 = 2.75f;
	if (p < 1.0f / d1)
		return n1 * p * p;
	if (p < 2.0f / d1) {
		p -= 1.5f / d1;
		return n1 * p * p + 0.75f;
	}
	if (p < 2.5f / d1) {
		p -= 2.25f / d1;
		return n1 * p * p + 0.9375f;
	}
	p -= 2.625f / d1;
	return n1 * p * p + 0.984375f;
}

float hash(uint32_t a, uint32_t b)
{
	uint32_t h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u;
	h ^= h >> 15;
	h *= 0x2C1B3C6Du;
	h ^= h >> 12;
	h *= 0x297A2D39u;
	h ^= h >> 15;
	return (float)(h & 0xFFFFFF) / (float)0xFFFFFF;
}

// Smooth noise in [-1, 1].
float noise1(uint32_t seed, float t)
{
	const float fl = std::floor(t);
	const float f = t - fl;
	const float u = f * f * (3.0f - 2.0f * f);
	const uint32_t i = (uint32_t)(int64_t)fl;
	return (hash(seed, i) + (hash(seed, i + 1) - hash(seed, i)) * u) * 2.0f - 1.0f;
}

/* Source data ---------------------------------------------------------- */

struct Instance {
	const GlyphEntry *e;
	float px, py; // pen position at rest, source px
	float dx, dy; // animation offset
	float scale, rot, alpha, hue;
};

struct text_source {
	obs_source_t *context = nullptr;
	fx_filter_t *fx = nullptr; // effect, settings snapshot, outline gradient
	gradient_t fill_grad;
	struct source_ref fill_ref;
	gs_texrender_t *fill_texrender = nullptr;
	gs_texrender_t *mask = nullptr;
	gs_texture_t *atlas = nullptr;

	// Worker.
	std::thread worker;
	std::mutex mutex;
	std::condition_variable cv;
	bool stop = false;
	bool has_request = false;
	LayoutParams request;                  // protected by mutex
	LayoutParams last_params;              // protected by mutex
	bool has_last = false;                 // protected by mutex
	std::shared_ptr<LayoutResult> pending; // protected by mutex

	// Graphics thread.
	std::shared_ptr<const LayoutResult> current;
	bool atlas_dirty = false;
	float time = 0.0f;
	float anim_time = 0.0f;
	float file_timer = 0.0f;
	int64_t file_mtime = 0;
	int64_t file_size = 0;

	std::atomic<bool> reset_anim{true};
	std::atomic<int> fill_type{FILL_COLOR};
	std::atomic<float> margin{0.0f};
	std::atomic<float> box_w{0.0f}, box_h{0.0f};
	std::string anim_key; // update thread
};

const struct fx_filter_def text_def = {
	"shaders/text/text.effect",
	TX_OUTLINE_PREFIX,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
};

bool warned_no_qt = false;

void worker_main(text_source *ts)
{
	spectrum_text::Builder builder;
	std::unique_lock<std::mutex> lock(ts->mutex);
	for (;;) {
		ts->cv.wait(lock, [ts] { return ts->stop || ts->has_request; });
		if (ts->stop)
			break;
		LayoutParams params = ts->request;
		ts->has_request = false;
		lock.unlock();
		if (!spectrum_text::qt_fonts_available() && !warned_no_qt) {
			warned_no_qt = true;
			blog(LOG_WARNING, "[Spectrum] Spectrum Text needs a Qt GUI application; text is not drawn");
		}
		std::shared_ptr<LayoutResult> result = builder.build(params);
		lock.lock();
		ts->pending = std::move(result);
	}
}

void submit(text_source *ts, const LayoutParams &params)
{
	std::lock_guard<std::mutex> lock(ts->mutex);
	ts->request = params;
	ts->has_request = true;
	ts->last_params = params;
	ts->has_last = true;
	ts->cv.notify_one();
}

bool same_params(const LayoutParams &a, const LayoutParams &b)
{
	return a.text == b.text && a.from_file == b.from_file && a.file == b.file && a.face == b.face &&
	       a.style == b.style && a.size == b.size && a.bold == b.bold && a.italic == b.italic &&
	       a.align == b.align && a.line_spacing == b.line_spacing && a.letter_spacing == b.letter_spacing &&
	       a.wrap == b.wrap && a.wrap_width == b.wrap_width && a.spread == b.spread && a.file_stamp == b.file_stamp;
}

/* obs_source_info callbacks -------------------------------------------- */

const char *text_name(void *)
{
	return obs_module_text("TextSource");
}

void text_update(void *data, obs_data_t *settings)
{
	text_source *ts = static_cast<text_source *>(data);
	fx_filter_update(ts->fx, settings);
	gradient_update(&ts->fill_grad, settings);
	source_ref_set_name(&ts->fill_ref, obs_data_get_string(settings, "tx_fill_source"));

	const Style st = read_style(settings);
	ts->fill_type = st.fill_type;
	ts->margin = st.margin();

	LayoutParams p;
	p.text = QString::fromUtf8(obs_data_get_string(settings, "text"));
	p.from_file = obs_data_get_bool(settings, "tx_from_file");
	p.file = obs_data_get_string(settings, "tx_file");
	obs_data_t *font = obs_data_get_obj(settings, "font");
	if (font) {
		p.face = QString::fromUtf8(obs_data_get_string(font, "face"));
		p.style = QString::fromUtf8(obs_data_get_string(font, "style"));
		p.size = (int)obs_data_get_int(font, "size");
		const long long flags = obs_data_get_int(font, "flags");
		p.bold = (flags & OBS_FONT_BOLD) != 0;
		p.italic = (flags & OBS_FONT_ITALIC) != 0;
		obs_data_release(font);
	}
	if (p.size <= 0)
		p.size = 96;
	p.align = (int)obs_data_get_int(settings, "tx_align");
	p.line_spacing = obs_data_get_double(settings, "tx_line_spacing") / 100.0;
	p.letter_spacing = obs_data_get_double(settings, "tx_letter_spacing");
	p.wrap = obs_data_get_bool(settings, "tx_wrap");
	p.wrap_width = (double)obs_data_get_int(settings, "tx_wrap_width");
	p.spread = st.spread();

	bool changed;
	{
		std::lock_guard<std::mutex> lock(ts->mutex);
		p.file_stamp = ts->has_last ? ts->last_params.file_stamp : 0;
		changed = !ts->has_last || !same_params(p, ts->last_params);
	}
	if (changed)
		submit(ts, p);

	// Restart the intro when its settings change.
	char key[256];
	snprintf(key, sizeof key, "%d|%d|%g|%g|%g|%g|%d|%g|%d", st.anim_in, st.slide_dir, st.slide_distance, st.stagger,
		 st.duration, st.delay, (int)st.loop, st.hold, (int)st.cursor);
	if (ts->anim_key != key) {
		ts->anim_key = key;
		ts->reset_anim = true;
	}
}

void *text_create(obs_data_t *settings, obs_source_t *source)
{
	text_source *ts = new text_source();
	ts->context = source;
	source_ref_init(&ts->fill_ref);
	gradient_init(&ts->fill_grad, source, TX_FILL_PREFIX);
	ts->fx = fx_filter_create(&text_def, settings, source);
	ts->worker = std::thread(worker_main, ts);
	text_update(ts, settings);
	return ts;
}

void text_destroy(void *data)
{
	text_source *ts = static_cast<text_source *>(data);
	{
		std::lock_guard<std::mutex> lock(ts->mutex);
		ts->stop = true;
		ts->cv.notify_one();
	}
	if (ts->worker.joinable())
		ts->worker.join();
	obs_enter_graphics();
	if (ts->atlas)
		gs_texture_destroy(ts->atlas);
	if (ts->mask)
		gs_texrender_destroy(ts->mask);
	if (ts->fill_texrender)
		gs_texrender_destroy(ts->fill_texrender);
	obs_leave_graphics();
	fx_filter_destroy(ts->fx);
	gradient_free(&ts->fill_grad);
	source_ref_free(&ts->fill_ref);
	delete ts;
}

uint32_t text_width(void *data)
{
	text_source *ts = static_cast<text_source *>(data);
	const float w = ts->box_w;
	return w > 0.0f ? (uint32_t)(w + 2.0f * ts->margin) : 0;
}

uint32_t text_height(void *data)
{
	text_source *ts = static_cast<text_source *>(data);
	const float h = ts->box_h;
	return h > 0.0f ? (uint32_t)(h + 2.0f * ts->margin) : 0;
}

void check_file(text_source *ts, float seconds)
{
	ts->file_timer += seconds;
	if (ts->file_timer < 1.0f)
		return;
	ts->file_timer = 0.0f;

	LayoutParams params;
	{
		std::lock_guard<std::mutex> lock(ts->mutex);
		if (!ts->has_last || !ts->last_params.from_file || ts->last_params.file.empty())
			return;
		params = ts->last_params;
	}
	struct stat st;
	if (os_stat(params.file.c_str(), &st) != 0)
		return;
	const int64_t mtime = (int64_t)st.st_mtime, size = (int64_t)st.st_size;
	if (mtime == ts->file_mtime && size == ts->file_size)
		return;
	const bool first = ts->file_mtime == 0 && ts->file_size == 0;
	ts->file_mtime = mtime;
	ts->file_size = size;
	if (first)
		return; // already read by the last request
	params.file_stamp++;
	submit(ts, params);
}

void text_tick(void *data, float seconds)
{
	text_source *ts = static_cast<text_source *>(data);
	fx_filter_tick(ts->fx, seconds);
	gradient_tick(&ts->fill_grad, seconds);
	source_ref_tick(&ts->fill_ref, seconds);
	source_ref_update_activation(&ts->fill_ref, ts->context, ts->fill_type == FILL_SOURCE, seconds);
	check_file(ts, seconds);

	std::shared_ptr<LayoutResult> result;
	{
		std::lock_guard<std::mutex> lock(ts->mutex);
		result = std::move(ts->pending);
		ts->pending.reset();
	}
	if (result) {
		if (!ts->current || ts->current->text != result->text)
			ts->reset_anim = true;
		ts->current = result;
		ts->atlas_dirty = true;
		ts->box_w = result->glyphs.empty() ? 0.0f : result->width;
		ts->box_h = result->glyphs.empty() ? 0.0f : result->height;
	}

	ts->time = std::fmod(ts->time + seconds, 3600.0f);
	if (ts->reset_anim.exchange(false))
		ts->anim_time = 0.0f;
	else
		ts->anim_time = std::min(ts->anim_time + seconds, 1e6f);
}

void text_activate(void *data)
{
	text_source *ts = static_cast<text_source *>(data);
	obs_data_t *s = obs_source_get_settings(ts->context);
	if (obs_data_get_bool(s, "tx_replay_on_activate"))
		ts->reset_anim = true;
	obs_data_release(s);
}

/* Per frame glyph animation -------------------------------------------- */

void build_instances(text_source *ts, const LayoutResult &r, const Style &st, float margin, std::vector<Instance> &out)
{
	out.clear();
	const int n = std::max(r.ordinal_count, 1);
	float at = ts->anim_time - st.delay;
	const float per_char = st.anim_in == IN_TYPEWRITER ? st.stagger : st.duration;
	const float total = (float)(n - 1) * st.stagger + per_char;
	if (st.anim_in != IN_NONE && st.loop) {
		const float cycle = st.delay + total + st.hold;
		if (cycle > 0.01f)
			at = std::fmod(ts->anim_time, cycle) - st.delay;
	}
	const float t = ts->time;

	auto animate = [&](Instance &in, int k) {
		in.dx = in.dy = 0.0f;
		in.scale = 1.0f;
		in.rot = 0.0f;
		in.alpha = 1.0f;
		const float lt = at - (float)k * st.stagger;
		const float p = saturate(lt / st.duration);
		switch (st.anim_in) {
		case IN_TYPEWRITER:
			in.alpha = (st.stagger <= 0.0f ? at >= 0.0f : lt >= 0.0f) ? 1.0f : 0.0f;
			break;
		case IN_FADE:
			in.alpha = p * p * (3.0f - 2.0f * p);
			break;
		case IN_SLIDE: {
			const float off = st.slide_distance * (1.0f - ease_out_cubic(p));
			if (st.slide_dir == DIR_UP)
				in.dy = off;
			else if (st.slide_dir == DIR_DOWN)
				in.dy = -off;
			else if (st.slide_dir == DIR_LEFT)
				in.dx = off;
			else
				in.dx = -off;
			in.alpha = saturate(p * 2.0f);
			break;
		}
		case IN_SCALE:
			in.scale = std::max(ease_out_back(p), 0.0f);
			in.alpha = saturate(p * 3.0f);
			break;
		case IN_DROP:
			in.dy = -st.slide_distance * (1.0f - ease_out_bounce(p));
			in.alpha = saturate(p * 4.0f);
			break;
		default:
			break;
		}

		const float amt = st.motion_amount;
		const float phase = st.motion_speed * t - st.motion_spread * (float)k;
		switch (st.motion) {
		case MOTION_WAVE:
			in.dy -= amt * std::sin(kTau * phase);
			break;
		case MOTION_BOUNCE:
			in.dy -= amt * std::fabs(std::sin(kPi * phase));
			break;
		case MOTION_SHAKE: {
			const float s = st.motion_speed * t * 6.0f;
			in.dx += amt * noise1((uint32_t)k * 3u, s);
			in.dy += amt * noise1((uint32_t)k * 3u + 1u, s);
			in.rot += amt * 0.6f * kPi / 180.0f * noise1((uint32_t)k * 3u + 2u, s);
			break;
		}
		case MOTION_JITTER: {
			const uint32_t step = (uint32_t)(int64_t)std::floor(st.motion_speed * t * 12.0f);
			in.dx += amt * (hash((uint32_t)k * 3u, step) * 2.0f - 1.0f);
			in.dy += amt * (hash((uint32_t)k * 3u + 1u, step) * 2.0f - 1.0f);
			break;
		}
		default:
			break;
		}

		in.hue = (st.rainbow_speed * t + st.rainbow_spread * (float)k) * kPi / 180.0f;
	};

	for (const spectrum_text::GlyphInstance &g : r.glyphs) {
		if (g.entry < 0 || g.entry >= (int)r.entries.size() || r.entries[g.entry].empty)
			continue;
		Instance in;
		in.e = &r.entries[g.entry];
		in.px = g.x + margin;
		in.py = g.y + margin;
		animate(in, g.ordinal);
		if (in.alpha > 0.0f && in.scale > 1e-3f)
			out.push_back(in);
	}

	// Typewriter cursor, after the last typed character.
	if (st.anim_in == IN_TYPEWRITER && st.cursor && r.cursor_entry >= 0 && !r.entries[r.cursor_entry].empty &&
	    !r.caret_x.empty()) {
		int typed = r.ordinal_count;
		if (st.stagger > 0.0f)
			typed = at < 0.0f ? 0 : std::min((int)std::floor(at / st.stagger) + 1, r.ordinal_count);
		else if (at < 0.0f)
			typed = 0;
		const bool done = typed >= r.ordinal_count;
		const bool on = !done || std::fmod(t, 1.0f) < 0.5f;
		if (on) {
			Instance in;
			in.e = &r.entries[r.cursor_entry];
			const size_t i = std::min((size_t)typed, r.caret_x.size() - 1);
			in.px = r.caret_x[i] + margin;
			in.py = r.caret_y[i] + margin;
			animate(in, typed);
			in.alpha = 1.0f;
			in.scale = 1.0f;
			out.push_back(in);
		}
	}
}

/* Rendering ------------------------------------------------------------- */

void upload_atlas(text_source *ts)
{
	if (!ts->atlas_dirty)
		return;
	ts->atlas_dirty = false;
	const LayoutResult *r = ts->current.get();
	const uint32_t w = r ? (uint32_t)r->atlas_w : 0, h = r ? (uint32_t)r->atlas_h : 0;
	if (ts->atlas && (!r || gs_texture_get_width(ts->atlas) != w || gs_texture_get_height(ts->atlas) != h)) {
		gs_texture_destroy(ts->atlas);
		ts->atlas = nullptr;
	}
	if (!r || !w || !h)
		return;
	if (!ts->atlas) {
		const uint8_t *data = reinterpret_cast<const uint8_t *>(r->atlas.data());
		ts->atlas = gs_texture_create(w, h, GS_R16F, 1, &data, GS_DYNAMIC);
	} else {
		gs_texture_set_image(ts->atlas, reinterpret_cast<const uint8_t *>(r->atlas.data()),
				     w * sizeof(uint16_t), false);
	}
}

// Draws each glyph with the technique, with its own transform and params.
void draw_glyphs(gs_effect_t *e, const char *technique, const std::vector<Instance> &glyphs, const Style &st,
		 bool mask_quads)
{
	gs_technique_t *tech = gs_effect_get_technique(e, technique);
	if (!tech)
		return;
	const float sa = st.shadow_angle * kPi / 180.0f;
	const float sx = std::cos(sa) * st.shadow_distance, sy = std::sin(sa) * st.shadow_distance;

	const size_t passes = gs_technique_begin(tech);
	for (size_t pass = 0; pass < passes; pass++) {
		if (!gs_technique_begin_pass(tech, pass))
			continue;
		for (const Instance &in : glyphs) {
			const GlyphEntry &g = *in.e;
			const float rw = (float)g.aw / g.scale, rh = (float)g.ah / g.scale;
			// Shadow offset in glyph space.
			const float c = std::cos(-in.rot), s = std::sin(-in.rot);
			const float ox = (c * sx - s * sy) / in.scale, oy = (s * sx + c * sy) / in.scale;
			float qx = g.ox, qy = g.oy, qx1 = g.ox + rw, qy1 = g.oy + rh;
			if (mask_quads && st.shadow) {
				qx = std::min(qx, g.ox + ox);
				qy = std::min(qy, g.oy + oy);
				qx1 = std::max(qx1, g.ox + rw + ox);
				qy1 = std::max(qy1, g.oy + rh + oy);
			}
			fx_set_vec4(e, "g_quad", qx, qy, qx1 - qx, qy1 - qy);
			fx_set_vec4(e, "g_region", g.ox, g.oy, (float)g.aw, (float)g.ah);
			fx_set_vec2(e, "g_atlas_pos", (float)g.ax, (float)g.ay);
			fx_set_float(e, "g_scale", g.scale);
			fx_set_vec2(e, "g_rest", in.px, in.py);
			fx_set_float(e, "g_alpha", in.alpha);
			fx_set_float(e, "g_hue", in.hue);
			fx_set_vec2(e, "g_shadow_off", ox, oy);

			gs_matrix_push();
			gs_matrix_translate3f(in.px + in.dx + g.cx, in.py + in.dy + g.cy, 0.0f);
			if (in.rot != 0.0f)
				gs_matrix_rotaa4f(0.0f, 0.0f, 1.0f, in.rot);
			if (in.scale != 1.0f)
				gs_matrix_scale3f(in.scale, in.scale, 1.0f);
			gs_matrix_translate3f(qx - g.cx, qy - g.cy, 0.0f);
			gs_matrix_scale3f(qx1 - qx, qy1 - qy, 1.0f);
			gs_draw_sprite(nullptr, 0, 1, 1);
			gs_matrix_pop();
		}
		gs_technique_end_pass(tech);
	}
	gs_technique_end(tech);
}

void draw_composite(gs_effect_t *e, const char *technique, uint32_t w, uint32_t h)
{
	while (gs_effect_loop(e, technique))
		gs_draw_sprite(nullptr, 0, w, h);
}

void text_render(void *data, gs_effect_t *)
{
	text_source *ts = static_cast<text_source *>(data);
	upload_atlas(ts);
	std::shared_ptr<const LayoutResult> r = ts->current;
	if (!r || r->glyphs.empty() || !ts->atlas || !fx_filter_ensure_effect(ts->fx))
		return;
	const uint32_t W = text_width(ts), H = text_height(ts);
	if (!W || !H)
		return;

	obs_data_t *settings = fx_filter_settings(ts->fx);
	const Style st = read_style(settings);
	const float margin = ts->margin;
	gs_effect_t *e = ts->fx->effect;

	std::vector<Instance> glyphs;
	build_instances(ts, *r, st, margin, glyphs);
	if (glyphs.empty()) {
		obs_data_release(settings);
		return;
	}

	// Fill texture (before any other render target is bound).
	gs_texture_t *fill = nullptr;
	int fill_type = st.fill_type;
	if (fill_type == FILL_GRADIENT) {
		fill = gradient_render_fill(&ts->fill_grad, W, H);
	} else if (fill_type == FILL_SOURCE) {
		obs_source_t *source = source_ref_get(&ts->fill_ref);
		if (source && !is_self_reference(ts->context, source))
			fill = render_source_to_texture(source, &ts->fill_texrender);
		obs_source_release(source);
	}
	if ((fill_type == FILL_GRADIENT || fill_type == FILL_SOURCE) && !fill)
		fill_type = FILL_NONE;

	gs_texture_t *mask = nullptr;
	// Ending a technique clears the effect's parameters: set them before
	// each one.
	auto set_params = [&]() {
		fx_filter_set_common(ts->fx, e, settings, nullptr, 0.0f, W, H, true, 0.0f);
		fx_set_int(e, "tx_fill_type", fill_type);
		fx_set_texture(e, "fill_tex", fill);
		fx_set_texture(e, "sdf_tex", ts->atlas);
		fx_set_texture(e, "mask_tex", mask);
		fx_set_vec2(e, "atlas_size", (float)r->atlas_w, (float)r->atlas_h);
	};

	const bool outline = st.outline && st.outline_width > 0.0f;
	const bool under = st.glow || st.shadow;
	if (outline || under) {
		// The mask is drawn at the size the source is shown at, so the
		// outline stays crisp when the source is scaled up.
		struct matrix4 m;
		gs_matrix_get(&m);
		const float sx = std::sqrt(m.x.x * m.x.x + m.x.y * m.x.y);
		const float sy = std::sqrt(m.y.x * m.y.x + m.y.y * m.y.y);
		float scale = std::clamp(std::max(sx, sy), 0.25f, 4.0f);
		scale = std::min(scale, 4096.0f / (float)std::max(W, H));
		const uint32_t mw = std::max((uint32_t)std::ceil((float)W * scale), 1u);
		const uint32_t mh = std::max((uint32_t)std::ceil((float)H * scale), 1u);

		if (!ts->mask)
			ts->mask = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
		gs_texrender_reset(ts->mask);
		const bool previous = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(false);
		gs_blend_state_push();
		gs_enable_blending(true);
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_ONE);
		gs_blend_op(GS_BLEND_OP_MAX);
		if (gs_texrender_begin(ts->mask, mw, mh)) {
			struct vec4 zero;
			vec4_zero(&zero);
			gs_clear(GS_CLEAR_COLOR, &zero, 0.0f, 0);
			gs_ortho(0.0f, (float)W, 0.0f, (float)H, -100.0f, 100.0f);
			set_params();
			draw_glyphs(e, "Mask", glyphs, st, true);
			gs_texrender_end(ts->mask);
			mask = gs_texrender_get_texture(ts->mask);
		}
		gs_blend_op(GS_BLEND_OP_ADD);
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous);
	}

	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(true);
	gs_blend_state_push();
	gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	if (mask && under) {
		set_params();
		draw_composite(e, "Under", W, H);
	}
	if (fill_type != FILL_NONE) {
		set_params();
		draw_glyphs(e, "Fill", glyphs, st, false);
	}
	if (mask && outline) {
		set_params();
		draw_composite(e, "Over", W, H);
	}
	gs_blend_state_pop();
	gs_enable_framebuffer_srgb(previous);

	obs_data_release(settings);
}

/* Settings -------------------------------------------------------------- */

void text_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "text", "Spectrum");
	obs_data_set_default_bool(settings, "tx_from_file", false);
	obs_data_t *font = obs_data_create();
	obs_data_set_default_string(font, "face", "Arial");
	obs_data_set_default_string(font, "style", "Bold");
	obs_data_set_default_int(font, "size", 96);
	obs_data_set_default_int(font, "flags", OBS_FONT_BOLD);
	obs_data_set_default_obj(settings, "font", font);
	obs_data_release(font);
	obs_data_set_default_int(settings, "tx_align", 0);
	obs_data_set_default_double(settings, "tx_line_spacing", 100.0);
	obs_data_set_default_double(settings, "tx_letter_spacing", 0.0);
	obs_data_set_default_bool(settings, "tx_wrap", false);
	obs_data_set_default_int(settings, "tx_wrap_width", 800);

	obs_data_set_default_int(settings, "tx_fill_type", FILL_COLOR);
	obs_data_set_default_int(settings, "tx_fill_color", 0xFFFFFFFF);
	gradient_defaults(settings, TX_FILL_PREFIX);

	obs_data_set_default_bool(settings, "tx_outline", false);
	obs_data_set_default_double(settings, "tx_outline_width", 4.0);
	obs_data_set_default_int(settings, "tx_outline_position", OUTLINE_OUTSIDE);
	obs_data_set_default_int(settings, "tx_outline_type", OUTLINE_COLOR);
	obs_data_set_default_int(settings, "tx_outline_color", 0xFF000000);
	gradient_defaults(settings, TX_OUTLINE_PREFIX);

	obs_data_set_default_bool(settings, "tx_glow", false);
	obs_data_set_default_double(settings, "tx_glow_size", 16.0);
	obs_data_set_default_double(settings, "tx_glow_intensity", 100.0);
	obs_data_set_default_int(settings, "tx_glow_color", 0xFFFFC030);

	obs_data_set_default_bool(settings, "tx_shadow", false);
	obs_data_set_default_double(settings, "tx_shadow_angle", 45.0);
	obs_data_set_default_double(settings, "tx_shadow_distance", 6.0);
	obs_data_set_default_double(settings, "tx_shadow_softness", 4.0);
	obs_data_set_default_int(settings, "tx_shadow_color", 0x99000000);

	obs_data_set_default_double(settings, "tx_opacity", 100.0);

	obs_data_set_default_int(settings, "tx_anim_in", IN_NONE);
	obs_data_set_default_int(settings, "tx_slide_direction", DIR_UP);
	obs_data_set_default_double(settings, "tx_slide_distance", 40.0);
	obs_data_set_default_double(settings, "tx_stagger", 0.05);
	obs_data_set_default_double(settings, "tx_duration", 0.4);
	obs_data_set_default_double(settings, "tx_delay", 0.0);
	obs_data_set_default_bool(settings, "tx_cursor", true);
	obs_data_set_default_bool(settings, "tx_loop", false);
	obs_data_set_default_double(settings, "tx_loop_hold", 2.0);
	obs_data_set_default_bool(settings, "tx_replay_on_activate", true);

	obs_data_set_default_int(settings, "tx_motion", MOTION_NONE);
	obs_data_set_default_double(settings, "tx_motion_amount", 8.0);
	obs_data_set_default_double(settings, "tx_motion_speed", 1.0);
	obs_data_set_default_double(settings, "tx_motion_spread", 0.1);

	obs_data_set_default_bool(settings, "tx_rainbow", false);
	obs_data_set_default_double(settings, "tx_rainbow_speed", 90.0);
	obs_data_set_default_double(settings, "tx_rainbow_spread", 20.0);
	obs_data_set_default_double(settings, "tx_rainbow_amount", 100.0);
}

void set_vis(obs_properties_t *props, const char *name, bool visible)
{
	obs_property_t *p = obs_properties_get(props, name);
	if (p)
		obs_property_set_visible(p, visible);
}

bool text_modified(obs_properties_t *props, obs_property_t *, obs_data_t *settings)
{
	const bool from_file = obs_data_get_bool(settings, "tx_from_file");
	set_vis(props, "text", !from_file);
	set_vis(props, "tx_file", from_file);
	set_vis(props, "tx_wrap_width", obs_data_get_bool(settings, "tx_wrap"));

	const int fill = (int)obs_data_get_int(settings, "tx_fill_type");
	set_vis(props, "tx_fill_color", fill == FILL_COLOR);
	set_vis(props, "tx_fill_source", fill == FILL_SOURCE);
	gradient_set_visible(props, TX_FILL_PREFIX, fill == FILL_GRADIENT);
	if (fill == FILL_GRADIENT)
		gradient_refresh_visibility(props, TX_FILL_PREFIX, settings);

	const bool outline = obs_data_get_bool(settings, "tx_outline");
	const int outline_type = (int)obs_data_get_int(settings, "tx_outline_type");
	set_vis(props, "tx_outline_color", outline_type == OUTLINE_COLOR);
	gradient_set_visible(props, TX_OUTLINE_PREFIX, outline && outline_type == OUTLINE_GRADIENT);

	const int in = (int)obs_data_get_int(settings, "tx_anim_in");
	set_vis(props, "tx_slide_direction", in == IN_SLIDE);
	set_vis(props, "tx_slide_distance", in == IN_SLIDE || in == IN_DROP);
	set_vis(props, "tx_duration", in != IN_NONE && in != IN_TYPEWRITER);
	set_vis(props, "tx_stagger", in != IN_NONE);
	set_vis(props, "tx_delay", in != IN_NONE);
	set_vis(props, "tx_cursor", in == IN_TYPEWRITER);
	set_vis(props, "tx_loop", in != IN_NONE);
	set_vis(props, "tx_loop_hold", in != IN_NONE && obs_data_get_bool(settings, "tx_loop"));
	set_vis(props, "tx_replay_on_activate", in != IN_NONE);
	set_vis(props, "tx_replay", in != IN_NONE);

	const int motion = (int)obs_data_get_int(settings, "tx_motion");
	set_vis(props, "tx_motion_amount", motion != MOTION_NONE);
	set_vis(props, "tx_motion_speed", motion != MOTION_NONE);
	set_vis(props, "tx_motion_spread", motion == MOTION_WAVE || motion == MOTION_BOUNCE);
	return true;
}

obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max, double step,
		       const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

bool replay_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	text_source *ts = static_cast<text_source *>(data);
	if (ts)
		ts->reset_anim = true;
	return false;
}

obs_properties_t *text_properties(void *data)
{
	text_source *ts = static_cast<text_source *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	/* Text */
	p = obs_properties_add_bool(props, "tx_from_file", obs_module_text("Text.FromFile"));
	obs_property_set_modified_callback(p, text_modified);
	obs_properties_add_text(props, "text", obs_module_text("Text.Text"), OBS_TEXT_MULTILINE);
	obs_properties_add_path(props, "tx_file", obs_module_text("Text.File"), OBS_PATH_FILE,
				"Text files (*.txt);;All files (*.*)", nullptr);
	obs_properties_add_font(props, "font", obs_module_text("Text.Font"));
	p = obs_properties_add_list(props, "tx_align", obs_module_text("Text.Align"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Text.Align.Left"), 0);
	obs_property_list_add_int(p, obs_module_text("Text.Align.Center"), 1);
	obs_property_list_add_int(p, obs_module_text("Text.Align.Right"), 2);
	slider(props, "tx_line_spacing", "Text.LineSpacing", 25.0, 400.0, 1.0, "%");
	slider(props, "tx_letter_spacing", "Text.LetterSpacing", -50.0, 200.0, 0.5, "px");
	p = obs_properties_add_bool(props, "tx_wrap", obs_module_text("Text.Wrap"));
	obs_property_set_modified_callback(p, text_modified);
	p = obs_properties_add_int(props, "tx_wrap_width", obs_module_text("Text.WrapWidth"), 16, 16384, 1);
	obs_property_int_set_suffix(p, "px");

	/* Fill */
	obs_properties_t *fill = obs_properties_create();
	p = obs_properties_add_list(fill, "tx_fill_type", obs_module_text("StrokeFilter.FillType"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Shape.Fill.None"), FILL_NONE);
	obs_property_list_add_int(p, obs_module_text("StrokeFilter.ColorFill"), FILL_COLOR);
	obs_property_list_add_int(p, obs_module_text("StrokeFilter.GradientFill"), FILL_GRADIENT);
	obs_property_list_add_int(p, obs_module_text("StrokeFilter.SourceFill"), FILL_SOURCE);
	obs_property_set_modified_callback(p, text_modified);
	obs_properties_add_color_alpha(fill, "tx_fill_color", obs_module_text("StrokeFilter.ColorFill"));
	p = obs_properties_add_list(fill, "tx_fill_source", obs_module_text("StrokeFilter.SourceFill"),
				    OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(stroke_add_source_to_list, p);
	obs_enum_scenes(stroke_add_source_to_list, p);
	obs_property_list_insert_string(p, 0, "None", "");
	slider(fill, "tx_opacity", "Gradient.Opacity", 0.0, 100.0, 0.1, "%");
	obs_properties_add_group(props, "tx_fill_group", obs_module_text("Text.Fill"), OBS_GROUP_NORMAL, fill);
	gradient_properties(props, TX_FILL_PREFIX, ts ? &ts->fill_grad : nullptr);

	/* Outline */
	obs_properties_t *outline = obs_properties_create();
	slider(outline, "tx_outline_width", "Text.OutlineWidth", 0.0, 100.0, 0.1, "px");
	p = obs_properties_add_list(outline, "tx_outline_position", obs_module_text("StrokeFilter.StrokePosition"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Shape.Stroke.Inside"), OUTLINE_INSIDE);
	obs_property_list_add_int(p, obs_module_text("Shape.Stroke.Center"), OUTLINE_CENTER);
	obs_property_list_add_int(p, obs_module_text("Shape.Stroke.Outside"), OUTLINE_OUTSIDE);
	p = obs_properties_add_list(outline, "tx_outline_type", obs_module_text("StrokeFilter.FillType"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("StrokeFilter.ColorFill"), OUTLINE_COLOR);
	obs_property_list_add_int(p, obs_module_text("Text.OutlineGradient"), OUTLINE_GRADIENT);
	obs_property_set_modified_callback(p, text_modified);
	obs_properties_add_color_alpha(outline, "tx_outline_color", obs_module_text("Shape.StrokeColor"));
	p = obs_properties_add_group(props, "tx_outline", obs_module_text("Text.Outline"), OBS_GROUP_CHECKABLE,
				     outline);
	obs_property_set_modified_callback(p, text_modified);
	gradient_properties_ex(props, TX_OUTLINE_PREFIX, ts ? &ts->fx->gradient : nullptr, GRADIENT_PROPS_MAP);

	/* Glow */
	obs_properties_t *glow = obs_properties_create();
	slider(glow, "tx_glow_size", "Text.GlowSize", 0.0, 200.0, 0.5, "px");
	slider(glow, "tx_glow_intensity", "Text.GlowIntensity", 0.0, 400.0, 1.0, "%");
	obs_properties_add_color_alpha(glow, "tx_glow_color", obs_module_text("Text.GlowColor"));
	obs_properties_add_group(props, "tx_glow", obs_module_text("Text.Glow"), OBS_GROUP_CHECKABLE, glow);

	/* Shadow */
	obs_properties_t *shadow = obs_properties_create();
	slider(shadow, "tx_shadow_angle", "Text.ShadowAngle", -180.0, 180.0, 0.1, "°");
	slider(shadow, "tx_shadow_distance", "Text.ShadowDistance", 0.0, 200.0, 0.5, "px");
	slider(shadow, "tx_shadow_softness", "Shape.ShadowBlur", 0.0, 100.0, 0.5, "px");
	obs_properties_add_color_alpha(shadow, "tx_shadow_color", obs_module_text("Shape.ShadowColor"));
	obs_properties_add_group(props, "tx_shadow", obs_module_text("Shape.Shadow"), OBS_GROUP_CHECKABLE, shadow);

	/* Animation */
	obs_properties_t *anim = obs_properties_create();
	p = obs_properties_add_list(anim, "tx_anim_in", obs_module_text("Text.AnimIn"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Shape.Fill.None"), IN_NONE);
	obs_property_list_add_int(p, obs_module_text("Text.AnimIn.Typewriter"), IN_TYPEWRITER);
	obs_property_list_add_int(p, obs_module_text("Text.AnimIn.Fade"), IN_FADE);
	obs_property_list_add_int(p, obs_module_text("Text.AnimIn.Slide"), IN_SLIDE);
	obs_property_list_add_int(p, obs_module_text("Text.AnimIn.Scale"), IN_SCALE);
	obs_property_list_add_int(p, obs_module_text("Text.AnimIn.Drop"), IN_DROP);
	obs_property_set_modified_callback(p, text_modified);
	p = obs_properties_add_list(anim, "tx_slide_direction", obs_module_text("Text.SlideDirection"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Text.Dir.Up"), DIR_UP);
	obs_property_list_add_int(p, obs_module_text("Text.Dir.Down"), DIR_DOWN);
	obs_property_list_add_int(p, obs_module_text("Text.Dir.Left"), DIR_LEFT);
	obs_property_list_add_int(p, obs_module_text("Text.Dir.Right"), DIR_RIGHT);
	slider(anim, "tx_slide_distance", "Text.SlideDistance", 0.0, 1000.0, 1.0, "px");
	slider(anim, "tx_stagger", "Text.Stagger", 0.0, 2.0, 0.005, "s");
	slider(anim, "tx_duration", "Text.Duration", 0.01, 5.0, 0.01, "s");
	slider(anim, "tx_delay", "Text.Delay", 0.0, 30.0, 0.05, "s");
	obs_properties_add_bool(anim, "tx_cursor", obs_module_text("Text.Cursor"));
	p = obs_properties_add_bool(anim, "tx_loop", obs_module_text("Text.Loop"));
	obs_property_set_modified_callback(p, text_modified);
	slider(anim, "tx_loop_hold", "Text.LoopHold", 0.0, 60.0, 0.1, "s");
	obs_properties_add_bool(anim, "tx_replay_on_activate", obs_module_text("Text.ReplayOnActivate"));
	obs_properties_add_button2(anim, "tx_replay", obs_module_text("Text.Replay"), replay_clicked, ts);

	p = obs_properties_add_list(anim, "tx_motion", obs_module_text("Text.Motion"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Shape.Fill.None"), MOTION_NONE);
	obs_property_list_add_int(p, obs_module_text("Text.Motion.Wave"), MOTION_WAVE);
	obs_property_list_add_int(p, obs_module_text("Text.Motion.Bounce"), MOTION_BOUNCE);
	obs_property_list_add_int(p, obs_module_text("Text.Motion.Shake"), MOTION_SHAKE);
	obs_property_list_add_int(p, obs_module_text("Text.Motion.Jitter"), MOTION_JITTER);
	obs_property_set_modified_callback(p, text_modified);
	slider(anim, "tx_motion_amount", "Text.MotionAmount", 0.0, 200.0, 0.5, "px");
	slider(anim, "tx_motion_speed", "Text.MotionSpeed", 0.0, 10.0, 0.01, "Hz");
	slider(anim, "tx_motion_spread", "Text.MotionSpread", 0.0, 1.0, 0.005, nullptr);
	obs_properties_add_group(props, "tx_anim_group", obs_module_text("Text.Animation"), OBS_GROUP_NORMAL, anim);

	/* Rainbow */
	obs_properties_t *rainbow = obs_properties_create();
	slider(rainbow, "tx_rainbow_speed", "Text.RainbowSpeed", -720.0, 720.0, 1.0, "°/s");
	slider(rainbow, "tx_rainbow_spread", "Text.RainbowSpread", -180.0, 180.0, 0.5, "°");
	slider(rainbow, "tx_rainbow_amount", "Text.RainbowAmount", 0.0, 100.0, 0.1, "%");
	obs_properties_add_group(props, "tx_rainbow", obs_module_text("Text.Rainbow"), OBS_GROUP_CHECKABLE, rainbow);

	spectrum_presets_properties(props, ts ? ts->context : nullptr);
	return props;
}

} // namespace

extern "C" struct obs_source_info spectrum_text_source;

struct obs_source_info spectrum_text_source = [] {
	struct obs_source_info info = {};
	info.id = "spectrum_text_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB;
	info.get_name = text_name;
	info.create = text_create;
	info.destroy = text_destroy;
	info.update = text_update;
	info.activate = text_activate;
	info.video_tick = text_tick;
	info.video_render = text_render;
	info.get_width = text_width;
	info.get_height = text_height;
	info.get_properties = text_properties;
	info.get_defaults = text_defaults;
	info.icon_type = OBS_ICON_TYPE_TEXT;
	return info;
}();
