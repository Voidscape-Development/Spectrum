#include "audio-reactive.h"
#include "audio-level.h"

#include <util/dstr.h>

#include <math.h>
#include <string.h>

#define KEY_ENABLED "spectrum_ar_enabled"
#define KEY_SOURCE "spectrum_ar_source"
#define KEY_MODE "spectrum_ar_mode"
#define KEY_RELATIVE "spectrum_ar_relative"
#define KEY_FLOOR "spectrum_ar_floor_db"
#define KEY_CEILING "spectrum_ar_ceiling_db"
#define KEY_ATTACK "spectrum_ar_attack"
#define KEY_RELEASE "spectrum_ar_release"
#define KEY_SENSITIVITY "spectrum_ar_sensitivity"
#define KEY_RANGE "spectrum_ar_use_range"

#define TARGETS 2
static const char *key_setting[TARGETS] = {"spectrum_ar_setting1", "spectrum_ar_setting2"};
static const char *key_from[TARGETS] = {"spectrum_ar_from1", "spectrum_ar_from2"};
static const char *key_to[TARGETS] = {"spectrum_ar_to1", "spectrum_ar_to2"};

enum reactive_mode { MODE_LEVEL = 0, MODE_BEAT = 1 };

struct spectrum_reactive {
	// Configuration.
	bool enabled;
	struct dstr source;
	enum reactive_mode mode;
	bool relative;
	double attack, release; // seconds
	struct dstr setting[TARGETS];
	double from[TARGETS], to[TARGETS];

	// State.
	spectrum_audio_meter_t *meter;
	double envelope;
	double pulse;
	double written; // envelope at the last write, < 0 when none
};

spectrum_reactive_t *spectrum_reactive_create(void)
{
	spectrum_reactive_t *r = bzalloc(sizeof(*r));
	r->meter = spectrum_audio_meter_create();
	r->written = -1.0;
	return r;
}

void spectrum_reactive_destroy(spectrum_reactive_t *r)
{
	if (!r)
		return;
	spectrum_audio_meter_destroy(r->meter);
	dstr_free(&r->source);
	for (size_t i = 0; i < TARGETS; i++)
		dstr_free(&r->setting[i]);
	bfree(r);
}

void spectrum_reactive_stop(spectrum_reactive_t *r)
{
	spectrum_audio_meter_unbind(r->meter);
}

void spectrum_reactive_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, KEY_ENABLED, false);
	obs_data_set_default_string(settings, KEY_SOURCE, "");
	obs_data_set_default_int(settings, KEY_MODE, MODE_LEVEL);
	obs_data_set_default_bool(settings, KEY_RELATIVE, false);
	obs_data_set_default_double(settings, KEY_FLOOR, -50.0);
	obs_data_set_default_double(settings, KEY_CEILING, -6.0);
	obs_data_set_default_double(settings, KEY_ATTACK, 30.0);
	obs_data_set_default_double(settings, KEY_RELEASE, 250.0);
	obs_data_set_default_double(settings, KEY_SENSITIVITY, 50.0);
	for (size_t i = 0; i < TARGETS; i++) {
		obs_data_set_default_string(settings, key_setting[i], "");
		obs_data_set_default_double(settings, key_from[i], 0.0);
		obs_data_set_default_double(settings, key_to[i], 100.0);
	}
}

/* ------------------------------------------------------------------------
 * Configuration and modulation
 * --------------------------------------------------------------------- */

// Settings may lack our defaults (a source type whose defaults could not
// be hooked), so fall back explicitly.
static bool has_value(obs_data_t *s, const char *key)
{
	return obs_data_has_user_value(s, key) || obs_data_has_default_value(s, key);
}

static double get_double(obs_data_t *s, const char *key, double fallback)
{
	return has_value(s, key) ? obs_data_get_double(s, key) : fallback;
}

bool spectrum_reactive_configure(spectrum_reactive_t *r, obs_data_t *settings)
{
	r->enabled = obs_data_get_bool(settings, KEY_ENABLED);
	dstr_copy(&r->source, obs_data_get_string(settings, KEY_SOURCE));
	r->mode = obs_data_get_int(settings, KEY_MODE) == MODE_BEAT ? MODE_BEAT : MODE_LEVEL;
	r->relative = obs_data_get_bool(settings, KEY_RELATIVE);
	r->attack = fmax(get_double(settings, KEY_ATTACK, 30.0), 0.0) / 1000.0;
	r->release = fmax(get_double(settings, KEY_RELEASE, 250.0), 0.0) / 1000.0;
	for (size_t i = 0; i < TARGETS; i++) {
		dstr_copy(&r->setting[i], obs_data_get_string(settings, key_setting[i]));
		r->from[i] = get_double(settings, key_from[i], 0.0);
		r->to[i] = get_double(settings, key_to[i], 100.0);
	}
	spectrum_audio_meter_configure(r->meter, get_double(settings, KEY_FLOOR, -50.0),
				       get_double(settings, KEY_CEILING, -6.0),
				       get_double(settings, KEY_SENSITIVITY, 50.0) / 100.0);
	r->written = -1.0;
	if (!spectrum_reactive_active(r))
		spectrum_audio_meter_unbind(r->meter);
	return spectrum_reactive_active(r);
}

bool spectrum_reactive_active(const spectrum_reactive_t *r)
{
	if (!r->enabled)
		return false;
	for (size_t i = 0; i < TARGETS; i++)
		if (r->setting[i].len)
			return true;
	return false;
}

bool spectrum_reactive_tick(spectrum_reactive_t *r, float seconds)
{
	if (!spectrum_reactive_active(r))
		return false;
	spectrum_audio_meter_bind(r->meter, r->source.array);

	double level;
	bool onset;
	spectrum_audio_meter_read(r->meter, &level, &onset);

	if (r->mode == MODE_BEAT) {
		// Jump on each onset, then decay over the release time.
		if (onset)
			r->pulse = 1.0;
		else
			r->pulse *= r->release > 0.0 ? exp(-(double)seconds / r->release) : 0.0;
		// Rise over the attack time, follow the decay directly.
		r->envelope = r->pulse > r->envelope
				      ? spectrum_envelope_step(r->envelope, r->pulse, seconds, r->attack, 0.0)
				      : r->pulse;
	} else {
		r->envelope = spectrum_envelope_step(r->envelope, level, seconds, r->attack, r->release);
	}
	if (r->envelope < 1e-6)
		r->envelope = 0.0;

	// Re-apply on changes of 0.1% of the range (always reach the ends).
	if (r->written < 0.0)
		return true;
	const double d = fabs(r->envelope - r->written);
	return d > 1e-3 || (d > 0.0 && (r->envelope == 0.0 || r->envelope >= 1.0));
}

void spectrum_reactive_write(spectrum_reactive_t *r, obs_data_t *base, obs_data_t *out)
{
	for (size_t i = 0; i < TARGETS; i++) {
		const char *key = r->setting[i].array;
		if (!key || !*key)
			continue;
		obs_data_item_t *item = obs_data_item_byname(base, key);
		if (!item || obs_data_item_gettype(item) != OBS_DATA_NUMBER) {
			obs_data_item_release(&item);
			continue;
		}
		const bool is_int = obs_data_item_numtype(item) == OBS_DATA_NUM_INT;
		obs_data_item_release(&item);

		const double offset = r->relative ? obs_data_get_double(base, key) : 0.0;
		const double value = offset + r->from[i] + (r->to[i] - r->from[i]) * r->envelope;
		if (is_int)
			obs_data_set_int(out, key, (long long)llround(value));
		else
			obs_data_set_double(out, key, value);
	}
	r->written = r->envelope;
}

/* ------------------------------------------------------------------------
 * Properties
 * --------------------------------------------------------------------- */

static void add_numeric_properties(obs_property_t *list, obs_properties_t *props, const char *prefix)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		const char *name = obs_property_name(p);
		if (name && strncmp(name, "spectrum_", 9) == 0)
			continue; // our own UI (this group)
		const enum obs_property_type type = obs_property_get_type(p);
		const char *desc = obs_property_description(p);
		if (type == OBS_PROPERTY_GROUP) {
			struct dstr sub = {0};
			dstr_printf(&sub, "%s%s / ", prefix, desc ? desc : "");
			add_numeric_properties(list, obs_property_group_content(p), sub.array);
			dstr_free(&sub);
		} else if (type == OBS_PROPERTY_FLOAT || type == OBS_PROPERTY_INT) {
			struct dstr label = {0};
			dstr_printf(&label, "%s%s", prefix, desc && *desc ? desc : name);
			obs_property_list_add_string(list, label.array, name);
			dstr_free(&label);
		}
	}
}

static bool add_audio_source(void *param, obs_source_t *source)
{
	if (obs_source_get_output_flags(source) & OBS_SOURCE_AUDIO) {
		const char *name = obs_source_get_name(source);
		if (name && *name)
			obs_property_list_add_string(param, name, name);
	}
	return true;
}

static obs_properties_t *group_content(obs_properties_t *props)
{
	obs_property_t *group = obs_properties_get(props, KEY_ENABLED);
	return group ? obs_property_group_content(group) : NULL;
}

static bool refresh_visibility(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	obs_properties_t *group = group_content(props);
	if (!group)
		return false;
	// Unchecked, the group folds down to its title.
	const bool on = obs_data_get_bool(settings, KEY_ENABLED);
	const bool beat = obs_data_get_int(settings, KEY_MODE) == MODE_BEAT;
	for (obs_property_t *q = obs_properties_first(group); q; obs_property_next(&q))
		obs_property_set_visible(q, on);
	obs_property_set_visible(obs_properties_get(group, KEY_SENSITIVITY), on && beat);
	obs_property_set_description(obs_properties_get(group, KEY_RELEASE),
				     obs_module_text(beat ? "Reactive.Decay" : "Reactive.Release"));
	return true;
}

static bool use_range_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	obs_source_t *source = data;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_properties_t *own = obs_source_properties(source);
	bool changed = false;
	for (size_t i = 0; own && i < TARGETS; i++) {
		const char *key = obs_data_get_string(settings, key_setting[i]);
		obs_property_t *tp = *key ? obs_properties_get(own, key) : NULL;
		if (!tp)
			continue;
		double lo, hi;
		if (obs_property_get_type(tp) == OBS_PROPERTY_FLOAT) {
			lo = obs_property_float_min(tp);
			hi = obs_property_float_max(tp);
		} else if (obs_property_get_type(tp) == OBS_PROPERTY_INT) {
			lo = obs_property_int_min(tp);
			hi = obs_property_int_max(tp);
		} else {
			continue;
		}
		const bool relative = obs_data_get_bool(settings, KEY_RELATIVE);
		const double value = obs_data_get_double(settings, key);
		// "Add to value" mode: from 0 up to the setting's maximum.
		obs_data_set_double(settings, key_from[i], relative ? 0.0 : lo);
		obs_data_set_double(settings, key_to[i], relative ? hi - value : hi);
		changed = true;
	}
	obs_properties_destroy(own);
	if (changed)
		obs_source_update(source, NULL);
	obs_data_release(settings);
	return changed;
}

static obs_property_t *add_float(obs_properties_t *props, const char *key, const char *text, double min, double max,
				 double step, const char *suffix, bool slider)
{
	obs_property_t *p = slider ? obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step)
				   : obs_properties_add_float(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

void spectrum_reactive_properties(obs_properties_t *props, obs_source_t *source)
{
	if (!source || obs_source_get_type(source) == OBS_SOURCE_TYPE_TRANSITION)
		return;
	obs_properties_t *group = obs_properties_create();
	obs_property_t *p;

	p = obs_properties_add_list(group, KEY_SOURCE, obs_module_text("Reactive.AudioSource"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, obs_module_text("Reactive.ChooseAudio"), "");
	obs_enum_sources(add_audio_source, p);
	obs_data_t *settings = obs_source_get_settings(source);
	// Keep a chosen source that does not exist (yet) selectable.
	const char *current = obs_data_get_string(settings, KEY_SOURCE);
	bool found = !current || !*current;
	for (size_t i = 0; !found && i < obs_property_list_item_count(p); i++)
		found = strcmp(obs_property_list_item_string(p, i), current) == 0;
	if (!found)
		obs_property_list_add_string(p, current, current);

	p = obs_properties_add_list(group, KEY_MODE, obs_module_text("Reactive.Mode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Reactive.Mode.Level"), MODE_LEVEL);
	obs_property_list_add_int(p, obs_module_text("Reactive.Mode.Beat"), MODE_BEAT);
	obs_property_set_modified_callback(p, refresh_visibility);
	obs_property_set_long_description(p, obs_module_text("Reactive.Mode.Description"));

	static const char *setting_text[TARGETS] = {"Reactive.Setting1", "Reactive.Setting2"};
	static const char *from_text[TARGETS] = {"Reactive.From1", "Reactive.From2"};
	static const char *to_text[TARGETS] = {"Reactive.To1", "Reactive.To2"};
	for (size_t i = 0; i < TARGETS; i++) {
		p = obs_properties_add_list(group, key_setting[i], obs_module_text(setting_text[i]),
					    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
		obs_property_list_add_string(p, obs_module_text("Reactive.ChooseSetting"), "");
		add_numeric_properties(p, props, "");
		add_float(group, key_from[i], from_text[i], -100000.0, 100000.0, 0.01, NULL, false);
		add_float(group, key_to[i], to_text[i], -100000.0, 100000.0, 0.01, NULL, false);
	}
	p = obs_properties_add_bool(group, KEY_RELATIVE, obs_module_text("Reactive.Relative"));
	obs_property_set_long_description(p, obs_module_text("Reactive.Relative.Description"));
	obs_properties_add_button2(group, KEY_RANGE, obs_module_text("Reactive.UseRange"), use_range_clicked, source);

	add_float(group, KEY_FLOOR, "Reactive.Floor", -100.0, 0.0, 0.1, "dB", true);
	add_float(group, KEY_CEILING, "Reactive.Ceiling", -100.0, 0.0, 0.1, "dB", true);
	add_float(group, KEY_SENSITIVITY, "Reactive.Sensitivity", 0.0, 100.0, 1.0, "%", true);
	add_float(group, KEY_ATTACK, "Reactive.Attack", 0.0, 2000.0, 1.0, "ms", true);
	add_float(group, KEY_RELEASE, "Reactive.Release", 0.0, 5000.0, 1.0, "ms", true);

	p = obs_properties_add_group(props, KEY_ENABLED, obs_module_text("Reactive.Group"), OBS_GROUP_CHECKABLE, group);
	obs_property_set_modified_callback(p, refresh_visibility);
	refresh_visibility(props, p, settings);
	obs_data_release(settings);
}
