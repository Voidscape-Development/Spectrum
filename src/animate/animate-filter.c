// Spectrum Animate: drives one numeric setting of another filter on the same
// source (or of the source itself) from a looping waveform or from the level
// of an audio source. Works with any filter or source, not only Spectrum's.
//
// The filter itself draws nothing (it passes video through). Each frame it
// computes a value in [0, 1], maps it to [min, max] and applies it to the
// target with obs_source_update().

#include <obs-module.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <math.h>
#include <string.h>
#include "../common/presets.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define SELF_TARGET "" // the parent source

enum an_mode { MODE_LOOP = 1, MODE_AUDIO = 2 };
enum an_wave { WAVE_SINE = 1, WAVE_TRIANGLE, WAVE_SAW_UP, WAVE_SAW_DOWN, WAVE_SQUARE, WAVE_RANDOM };

struct animate_filter {
	obs_source_t *context;

	// Settings (written on the UI thread, read in video_tick).
	pthread_mutex_t mutex;
	struct dstr target_name;
	struct dstr setting;
	struct dstr audio_name;
	enum an_mode mode;
	enum an_wave wave;
	double period, phase;
	double min, max;
	double floor_db, ceiling_db, attack, release;

	// State (video_tick).
	double time;
	double envelope;
	double last_value;
	bool have_last;

	// Audio capture.
	obs_weak_source_t *audio_weak;
	struct dstr audio_bound;
	pthread_mutex_t level_mutex;
	float level; // latest normalised level, 0..1
};

/* ------------------------------------------------------------------------
 * Helpers
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
	pthread_mutex_lock(&f->mutex);
	dstr_copy(&f->target_name, obs_data_get_string(settings, "an_target"));
	dstr_copy(&f->setting, obs_data_get_string(settings, "an_setting"));
	dstr_copy(&f->audio_name, obs_data_get_string(settings, "an_audio_source"));
	f->mode = (enum an_mode)obs_data_get_int(settings, "an_mode");
	f->wave = (enum an_wave)obs_data_get_int(settings, "an_wave");
	f->period = fmax(obs_data_get_double(settings, "an_period"), 0.01);
	f->phase = obs_data_get_double(settings, "an_phase") / 100.0;
	f->min = obs_data_get_double(settings, "an_min");
	f->max = obs_data_get_double(settings, "an_max");
	f->floor_db = obs_data_get_double(settings, "an_floor_db");
	f->ceiling_db = obs_data_get_double(settings, "an_ceiling_db");
	f->attack = fmax(obs_data_get_double(settings, "an_attack") / 1000.0, 0.0);
	f->release = fmax(obs_data_get_double(settings, "an_release") / 1000.0, 0.0);
	f->have_last = false;
	pthread_mutex_unlock(&f->mutex);
}

static void *animate_create(obs_data_t *settings, obs_source_t *source)
{
	struct animate_filter *f = bzalloc(sizeof(struct animate_filter));
	f->context = source;
	pthread_mutex_init(&f->mutex, NULL);
	pthread_mutex_init(&f->level_mutex, NULL);
	animate_update(f, settings);
	return f;
}

static void animate_destroy(void *data)
{
	struct animate_filter *f = data;
	unbind_audio(f);
	dstr_free(&f->target_name);
	dstr_free(&f->setting);
	dstr_free(&f->audio_name);
	pthread_mutex_destroy(&f->level_mutex);
	pthread_mutex_destroy(&f->mutex);
	bfree(f);
}

static void animate_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "an_target", SELF_TARGET);
	obs_data_set_default_string(settings, "an_setting", "");
	obs_data_set_default_int(settings, "an_mode", MODE_LOOP);
	obs_data_set_default_int(settings, "an_wave", WAVE_SINE);
	obs_data_set_default_double(settings, "an_period", 2.0);
	obs_data_set_default_double(settings, "an_phase", 0.0);
	obs_data_set_default_double(settings, "an_min", 0.0);
	obs_data_set_default_double(settings, "an_max", 100.0);
	obs_data_set_default_double(settings, "an_floor_db", -50.0);
	obs_data_set_default_double(settings, "an_ceiling_db", -6.0);
	obs_data_set_default_double(settings, "an_attack", 30.0);
	obs_data_set_default_double(settings, "an_release", 250.0);
}

static void animate_tick(void *data, float seconds)
{
	struct animate_filter *f = data;
	if (!obs_source_enabled(f->context))
		return;

	pthread_mutex_lock(&f->mutex);
	struct dstr target_name = {0}, setting = {0}, audio_name = {0};
	dstr_copy_dstr(&target_name, &f->target_name);
	dstr_copy_dstr(&setting, &f->setting);
	dstr_copy_dstr(&audio_name, &f->audio_name);
	const enum an_mode mode = f->mode;
	const enum an_wave wave = f->wave;
	const double period = f->period, phase = f->phase, vmin = f->min, vmax = f->max;
	const double attack = f->attack, release = f->release;
	pthread_mutex_unlock(&f->mutex);

	bind_audio(f, audio_name.array, mode == MODE_AUDIO);

	double v;
	if (mode == MODE_AUDIO) {
		pthread_mutex_lock(&f->level_mutex);
		const double level = f->level;
		f->level = 0.0f;
		pthread_mutex_unlock(&f->level_mutex);
		const double tc = level > f->envelope ? attack : release;
		const double k = tc > 0.0 ? 1.0 - exp(-(double)seconds / tc) : 1.0;
		f->envelope += (level - f->envelope) * k;
		v = f->envelope;
	} else {
		f->time = fmod(f->time + seconds, period * 1000003.0);
		const double x = f->time / period + phase;
		const double cycle = floor(x);
		v = wave_value(wave, x - cycle, cycle);
	}

	const double value = vmin + (vmax - vmin) * v;
	if (setting.len && (!f->have_last || fabs(value - f->last_value) > 1e-9)) {
		obs_source_t *target = get_target(f, target_name.array);
		if (target) {
			obs_data_t *current = obs_source_get_settings(target);
			obs_data_item_t *item = obs_data_item_byname(current, setting.array);
			const bool is_int = item && obs_data_item_numtype(item) == OBS_DATA_NUM_INT;
			obs_data_item_release(&item);
			obs_data_release(current);

			obs_data_t *update = obs_data_create();
			if (is_int)
				obs_data_set_int(update, setting.array, (long long)llround(value));
			else
				obs_data_set_double(update, setting.array, value);
			obs_source_update(target, update);
			obs_data_release(update);
			obs_source_release(target);
			f->last_value = value;
			f->have_last = true;
		}
	}

	dstr_free(&target_name);
	dstr_free(&setting);
	dstr_free(&audio_name);
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
	UNUSED_PARAMETER(p);
	fill_setting_list(priv, obs_properties_get(props, "an_setting"), obs_data_get_string(settings, "an_target"));
	return true;
}

static bool use_range_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	struct animate_filter *f = data;
	obs_data_t *settings = obs_source_get_settings(f->context);
	obs_source_t *target = get_target(f, obs_data_get_string(settings, "an_target"));
	bool changed = false;
	if (target) {
		obs_properties_t *tprops = obs_source_properties(target);
		obs_property_t *tp = tprops ? obs_properties_get(tprops, obs_data_get_string(settings, "an_setting"))
					    : NULL;
		if (tp && obs_property_get_type(tp) == OBS_PROPERTY_FLOAT) {
			obs_data_set_double(settings, "an_min", obs_property_float_min(tp));
			obs_data_set_double(settings, "an_max", obs_property_float_max(tp));
			changed = true;
		} else if (tp && obs_property_get_type(tp) == OBS_PROPERTY_INT) {
			obs_data_set_double(settings, "an_min", obs_property_int_min(tp));
			obs_data_set_double(settings, "an_max", obs_property_int_max(tp));
			changed = true;
		}
		obs_properties_destroy(tprops);
		obs_source_release(target);
	}
	if (changed)
		obs_source_update(f->context, settings);
	obs_data_release(settings);
	return changed;
}

static bool mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const bool audio = obs_data_get_int(settings, "an_mode") == MODE_AUDIO;
	static const char *loop_keys[] = {"an_wave", "an_period", "an_phase", NULL};
	static const char *audio_keys[] = {"an_audio_source", "an_floor_db", "an_ceiling_db",
					   "an_attack",       "an_release",  NULL};
	for (size_t i = 0; loop_keys[i]; i++)
		obs_property_set_visible(obs_properties_get(props, loop_keys[i]), !audio);
	for (size_t i = 0; audio_keys[i]; i++)
		obs_property_set_visible(obs_properties_get(props, audio_keys[i]), audio);
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

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static obs_properties_t *animate_properties(void *data)
{
	struct animate_filter *f = data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(props, "an_target", obs_module_text("Animate.Target"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Animate.ThisSource"), SELF_TARGET);
	obs_source_t *parent = f ? obs_filter_get_parent(f->context) : NULL;
	if (parent) {
		struct list_ctx ctx = {p, f->context};
		obs_source_enum_filters(parent, add_filter_to_list, &ctx);
	}
	obs_property_set_modified_callback2(p, target_modified, f);

	p = obs_properties_add_list(props, "an_setting", obs_module_text("Animate.Setting"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	if (f) {
		obs_data_t *settings = obs_source_get_settings(f->context);
		fill_setting_list(f, p, obs_data_get_string(settings, "an_target"));
		obs_data_release(settings);
	}

	slider(props, "an_min", "Animate.Min", -10000.0, 10000.0, 0.01, NULL);
	slider(props, "an_max", "Animate.Max", -10000.0, 10000.0, 0.01, NULL);
	obs_properties_add_button2(props, "an_use_range", obs_module_text("Animate.UseRange"), use_range_clicked, f);

	p = obs_properties_add_list(props, "an_mode", obs_module_text("Animate.Mode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Animate.Mode.Loop"), MODE_LOOP);
	obs_property_list_add_int(p, obs_module_text("Animate.Mode.Audio"), MODE_AUDIO);
	obs_property_set_modified_callback(p, mode_modified);

	p = obs_properties_add_list(props, "an_wave", obs_module_text("Animate.Wave"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.Sine"), WAVE_SINE);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.Triangle"), WAVE_TRIANGLE);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.SawUp"), WAVE_SAW_UP);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.SawDown"), WAVE_SAW_DOWN);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.Square"), WAVE_SQUARE);
	obs_property_list_add_int(p, obs_module_text("Animate.Wave.Random"), WAVE_RANDOM);
	slider(props, "an_period", "Animate.Period", 0.05, 120.0, 0.01, "s");
	slider(props, "an_phase", "Animate.Phase", 0.0, 100.0, 0.1, "%");

	p = obs_properties_add_list(props, "an_audio_source", obs_module_text("Animate.AudioSource"),
				    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Animate.ChooseAudio"), "");
	obs_enum_sources(add_audio_source, p);
	slider(props, "an_floor_db", "Animate.Floor", -100.0, 0.0, 0.1, "dB");
	slider(props, "an_ceiling_db", "Animate.Ceiling", -100.0, 0.0, 0.1, "dB");
	slider(props, "an_attack", "Animate.Attack", 0.0, 2000.0, 1.0, "ms");
	slider(props, "an_release", "Animate.Release", 0.0, 5000.0, 1.0, "ms");
	spectrum_presets_properties(props, data ? ((struct animate_filter *)data)->context : NULL);
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
