#include "source-hooks.h"
#include "audio-reactive.h"
#include "presets.h"

#include <util/darray.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#define MAX_TYPES 128
#define HOTKEY_ENABLE "spectrum.enable"
#define HOTKEY_DISABLE "spectrum.disable"
#define HOTKEY_PRESET_PREFIX "spectrum.preset."
#define HOTKEY_UNDO_PREFIX "spectrum.preset_undo."

struct hook_type {
	struct obs_source_info orig;
	char id[256]; // as OBS reports it: versioned types get "_v<version>"
};

struct hook_state;

// "Apply preset" / "Undo preset" pair (OBS only allows hotkey pairs on
// filters, which are private sources). Bound to one key, it toggles.
struct preset_hotkey {
	struct hook_state *state;
	char *name;
	obs_hotkey_pair_id id;
	// Hotkey thread only: the settings before the preset was applied, and
	// right after (to tell whether it is still applied).
	obs_data_t *previous;
	char *applied;
};

static bool preset_still_applied(struct preset_hotkey *ph, obs_data_t *settings)
{
	return ph->previous && ph->applied && strcmp(obs_data_get_json(settings), ph->applied) == 0;
}

static void forget_previous(struct preset_hotkey *ph)
{
	obs_data_release(ph->previous);
	ph->previous = NULL;
	bfree(ph->applied);
	ph->applied = NULL;
}

struct hook_state {
	const struct hook_type *type;
	obs_source_t *source; // not owned: valid until the destroy wrapper
	void *data;           // the instance's own data
	bool dead;            // destroyed; freed by the next tick

	obs_hotkey_pair_id enable_pair;
	DARRAY(struct preset_hotkey *) presets;

	spectrum_reactive_t *reactive;
	obs_data_t *copy; // settings with defaults, handed to update
	bool force;       // apply on the next tick
};

static struct hook_type types[MAX_TYPES];
static size_t type_count = 0;

// Guards `states` and every hook_state. Recursive: releasing a reference
// inside a tick can destroy a source synchronously.
static pthread_mutex_t mutex;
static DARRAY(struct hook_state *) states;
static bool started = false;

/* ------------------------------------------------------------------------
 * Lookups (mutex held)
 * --------------------------------------------------------------------- */

static const struct hook_type *find_type(const char *id)
{
	for (size_t i = 0; id && i < type_count; i++)
		if (strcmp(types[i].id, id) == 0)
			return &types[i];
	return NULL;
}

static struct hook_state *find_state(void *data)
{
	for (size_t i = 0; i < states.num; i++)
		if (!states.array[i]->dead && states.array[i]->data == data)
			return states.array[i];
	return NULL;
}

/* ------------------------------------------------------------------------
 * Hotkeys
 * --------------------------------------------------------------------- */

static char *describe(const char *text_key, const char *source_name, const char *preset)
{
	struct dstr s = {0};
	dstr_copy(&s, obs_module_text(text_key));
	dstr_replace(&s, "%2", preset ? preset : "");
	dstr_replace(&s, "%1", source_name ? source_name : "");
	return s.array;
}

static bool enable_pressed(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct hook_state *s = data;
	if (!pressed || obs_source_enabled(s->source))
		return false;
	obs_source_set_enabled(s->source, true);
	return true;
}

static bool disable_pressed(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct hook_state *s = data;
	if (!pressed || !obs_source_enabled(s->source))
		return false;
	obs_source_set_enabled(s->source, false);
	return true;
}

static bool preset_apply_pressed(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct preset_hotkey *ph = data;
	if (!pressed)
		return false;
	obs_source_t *source = ph->state->source;
	obs_data_t *settings = obs_source_get_settings(source);
	// Returning false while applied lets one key bound to both toggle.
	bool applied = false;
	if (!preset_still_applied(ph, settings)) {
		obs_data_t *before = obs_data_create_from_json(obs_data_get_json(settings));
		if (spectrum_presets_apply(source, ph->name)) {
			forget_previous(ph);
			ph->previous = before;
			ph->applied = bstrdup(obs_data_get_json(settings));
			applied = true;
		} else {
			obs_data_release(before);
		}
	}
	obs_data_release(settings);
	return applied;
}

static bool preset_undo_pressed(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	struct preset_hotkey *ph = data;
	if (!pressed || !ph->previous)
		return false;
	obs_source_t *source = ph->state->source;
	obs_data_t *settings = obs_source_get_settings(source);
	obs_data_clear(settings);
	obs_data_apply(settings, ph->previous);
	obs_source_update(source, settings);
	obs_data_release(settings);
	forget_previous(ph);
	return true;
}

static void set_preset_descriptions(struct preset_hotkey *ph, const char *source_name)
{
	char *apply = describe("Hotkeys.Preset", source_name, ph->name);
	char *undo = describe("Hotkeys.PresetUndo", source_name, ph->name);
	obs_hotkey_pair_set_descriptions(ph->id, apply, undo);
	bfree(apply);
	bfree(undo);
}

static void set_enable_descriptions(struct hook_state *s, const char *name)
{
	if (s->enable_pair == OBS_INVALID_HOTKEY_PAIR_ID)
		return;
	char *on = describe("Hotkeys.Enable", name, NULL);
	char *off = describe("Hotkeys.Disable", name, NULL);
	obs_hotkey_pair_set_descriptions(s->enable_pair, on, off);
	bfree(on);
	bfree(off);
}

static void register_enable_pair(struct hook_state *s)
{
	s->enable_pair = OBS_INVALID_HOTKEY_PAIR_ID;
	if (obs_source_get_type(s->source) != OBS_SOURCE_TYPE_FILTER)
		return;
	const char *name = obs_source_get_name(s->source);
	char *on = describe("Hotkeys.Enable", name, NULL);
	char *off = describe("Hotkeys.Disable", name, NULL);
	s->enable_pair = obs_hotkey_pair_register_source(s->source, HOTKEY_ENABLE, on, HOTKEY_DISABLE, off,
							 enable_pressed, disable_pressed, s, s);
	bfree(on);
	bfree(off);
}

static void free_preset_hotkey(struct preset_hotkey *ph)
{
	obs_hotkey_pair_unregister(ph->id);
	forget_previous(ph);
	bfree(ph->name);
	bfree(ph);
}

struct name_list {
	DARRAY(char *) names;
};

static void collect_name(void *param, const char *name)
{
	struct name_list *list = param;
	char *copy = bstrdup(name);
	da_push_back(list->names, &copy);
}

// Registers a hotkey for each saved preset of the type, and unregisters
// those of deleted presets.
static void sync_preset_hotkeys(struct hook_state *s)
{
	struct name_list list = {0};
	spectrum_presets_enum(obs_source_get_unversioned_id(s->source), collect_name, &list);

	for (size_t i = s->presets.num; i-- > 0;) {
		bool keep = false;
		for (size_t j = 0; j < list.names.num && !keep; j++)
			keep = strcmp(list.names.array[j], s->presets.array[i]->name) == 0;
		if (!keep) {
			free_preset_hotkey(s->presets.array[i]);
			da_erase(s->presets, i);
		}
	}
	const char *source_name = obs_source_get_name(s->source);
	for (size_t j = 0; j < list.names.num; j++) {
		const char *name = list.names.array[j];
		bool have = false;
		for (size_t i = 0; i < s->presets.num && !have; i++)
			have = strcmp(s->presets.array[i]->name, name) == 0;
		if (!have && *name) {
			struct preset_hotkey *ph = bzalloc(sizeof(*ph));
			ph->state = s;
			ph->name = bstrdup(name);
			struct dstr apply = {0}, undo = {0};
			dstr_printf(&apply, "%s%s", HOTKEY_PRESET_PREFIX, name);
			dstr_printf(&undo, "%s%s", HOTKEY_UNDO_PREFIX, name);
			ph->id = obs_hotkey_pair_register_source(s->source, apply.array, "", undo.array, "",
								 preset_apply_pressed, preset_undo_pressed, ph, ph);
			dstr_free(&apply);
			dstr_free(&undo);
			if (ph->id == OBS_INVALID_HOTKEY_PAIR_ID) {
				bfree(ph->name);
				bfree(ph);
			} else {
				set_preset_descriptions(ph, source_name);
				da_push_back(s->presets, &ph);
			}
		}
		bfree(list.names.array[j]);
	}
	da_free(list.names);
}

static void source_renamed(void *param, calldata_t *cd)
{
	struct hook_state *s = param;
	const char *name = calldata_string(cd, "new_name");
	pthread_mutex_lock(&mutex);
	if (!s->dead) {
		set_enable_descriptions(s, name);
		for (size_t i = 0; i < s->presets.num; i++)
			set_preset_descriptions(s->presets.array[i], name);
	}
	pthread_mutex_unlock(&mutex);
}

/* ------------------------------------------------------------------------
 * React to Audio
 * --------------------------------------------------------------------- */

// Rebuilds the private settings copy: the type's defaults, then the
// instance's values.
static void rebuild_copy(struct hook_state *s, obs_data_t *settings)
{
	obs_data_release(s->copy);
	s->copy = obs_get_source_defaults(obs_source_get_id(s->source));
	if (!s->copy)
		s->copy = obs_data_create();
	obs_data_apply(s->copy, settings);
}

static void apply_modulated(struct hook_state *s, obs_data_t *settings)
{
	if (!s->copy)
		rebuild_copy(s, settings);
	spectrum_reactive_write(s->reactive, settings, s->copy);
	s->type->orig.update(s->data, s->copy);
}

static void hooks_tick(void *param, float seconds)
{
	UNUSED_PARAMETER(param);
	pthread_mutex_lock(&mutex);
	for (size_t i = states.num; i-- > 0;) {
		struct hook_state *s = states.array[i];
		if (s->dead) {
			spectrum_reactive_destroy(s->reactive);
			obs_data_release(s->copy);
			da_free(s->presets);
			bfree(s);
			da_erase(states, i);
		}
	}
	// By index: a nested destroy only marks states dead.
	for (size_t i = 0; i < states.num; i++) {
		struct hook_state *s = states.array[i];
		if (s->dead || !s->type->orig.update || !spectrum_reactive_active(s->reactive))
			continue;
		const bool changed = spectrum_reactive_tick(s->reactive, seconds);
		if (s->dead || !(changed || s->force))
			continue;
		if (!obs_source_enabled(s->source)) {
			s->force = true; // catch up when enabled again
			continue;
		}
		obs_data_t *settings = obs_source_get_settings(s->source);
		apply_modulated(s, settings);
		obs_data_release(settings);
		s->force = false;
	}
	pthread_mutex_unlock(&mutex);
}

/* ------------------------------------------------------------------------
 * Wrapped obs_source_info callbacks
 * --------------------------------------------------------------------- */

static void *hook_create(obs_data_t *settings, obs_source_t *source)
{
	pthread_mutex_lock(&mutex);
	const struct hook_type *type = find_type(obs_source_get_id(source));
	pthread_mutex_unlock(&mutex);
	if (!type)
		return NULL;

	void *data = type->orig.create(settings, source);
	if (!data)
		return NULL;

	struct hook_state *s = bzalloc(sizeof(*s));
	s->type = type;
	s->source = source;
	s->data = data;
	s->reactive = spectrum_reactive_create();

	pthread_mutex_lock(&mutex);
	da_push_back(states, &s);
	register_enable_pair(s);
	sync_preset_hotkeys(s);
	// The type's create ran its own update with the plain settings.
	s->force = spectrum_reactive_configure(s->reactive, settings);
	pthread_mutex_unlock(&mutex);

	signal_handler_connect(obs_source_get_signal_handler(source), "rename", source_renamed, s);
	return data;
}

static void hook_update(void *data, obs_data_t *settings)
{
	pthread_mutex_lock(&mutex);
	struct hook_state *s = find_state(data);
	if (s && spectrum_reactive_configure(s->reactive, settings)) {
		// Keep the modulation through the user's edit.
		rebuild_copy(s, settings);
		apply_modulated(s, settings);
	} else if (s) {
		obs_data_release(s->copy);
		s->copy = NULL;
		s->type->orig.update(data, settings);
	} else {
		blog(LOG_WARNING, "[Spectrum] update for an unknown instance");
	}
	pthread_mutex_unlock(&mutex);
}

static void hook_destroy(void *data)
{
	pthread_mutex_lock(&mutex);
	struct hook_state *s = find_state(data);
	pthread_mutex_unlock(&mutex);
	// Outside our lock: the rename handler takes it while the signal
	// handler holds its own. `s` stays valid: only dead states are freed,
	// and only this function marks it dead.
	if (s)
		signal_handler_disconnect(obs_source_get_signal_handler(s->source), "rename", source_renamed, s);

	pthread_mutex_lock(&mutex);
	const struct hook_type *type = s ? s->type : NULL;
	if (s) {
		s->dead = true;
		if (s->enable_pair != OBS_INVALID_HOTKEY_PAIR_ID)
			obs_hotkey_pair_unregister(s->enable_pair);
		s->enable_pair = OBS_INVALID_HOTKEY_PAIR_ID;
		for (size_t i = 0; i < s->presets.num; i++)
			free_preset_hotkey(s->presets.array[i]);
		da_resize(s->presets, 0);
		spectrum_reactive_stop(s->reactive);
	}
	pthread_mutex_unlock(&mutex);

	if (type && type->orig.destroy)
		type->orig.destroy(data);
	else if (!type)
		blog(LOG_WARNING, "[Spectrum] destroy for an unknown instance (leaked)");
}

static void hook_defaults2(void *type_data, obs_data_t *settings)
{
	const struct hook_type *type = type_data;
	if (type->orig.get_defaults)
		type->orig.get_defaults(settings);
	spectrum_reactive_defaults(settings);
}

/* ------------------------------------------------------------------------
 * Registration
 * --------------------------------------------------------------------- */

void spectrum_register_source(const struct obs_source_info *info)
{
	if (info->type == OBS_SOURCE_TYPE_TRANSITION || !info->create || type_count >= MAX_TYPES) {
		obs_register_source_s(info, sizeof(struct obs_source_info));
		return;
	}
	if (!started) {
		pthread_mutex_init_recursive(&mutex);
		da_init(states);
		obs_add_tick_callback(hooks_tick, NULL);
		started = true;
	}

	struct hook_type *type = &types[type_count++];
	type->orig = *info;
	if (info->version)
		snprintf(type->id, sizeof(type->id), "%s_v%u", info->id, info->version);
	else
		snprintf(type->id, sizeof(type->id), "%s", info->id);

	struct obs_source_info wrapped = *info;
	wrapped.create = hook_create;
	if (info->update)
		wrapped.update = hook_update;
	wrapped.destroy = hook_destroy;
	// Some OBS paths (obs_get_source_defaults) call only get_defaults2 when
	// both are set, so it calls the type's get_defaults itself. Types
	// that use type_data keep theirs (React to Audio reads with fallbacks).
	if (!info->get_defaults2 && !info->type_data) {
		wrapped.type_data = type;
		wrapped.get_defaults = NULL;
		wrapped.get_defaults2 = hook_defaults2;
	}
	obs_register_source_s(&wrapped, sizeof(struct obs_source_info));
}

void spectrum_hooks_shutdown(void)
{
	if (!started)
		return;
	obs_remove_tick_callback(hooks_tick, NULL);
	pthread_mutex_lock(&mutex);
	for (size_t i = 0; i < states.num; i++) {
		struct hook_state *s = states.array[i];
		if (!s->dead)
			continue; // still alive: leave it to its destroy wrapper
		spectrum_reactive_destroy(s->reactive);
		obs_data_release(s->copy);
		da_free(s->presets);
		bfree(s);
		da_erase(states, i--);
	}
	pthread_mutex_unlock(&mutex);
}

void spectrum_hooks_presets_changed(const char *unversioned_id)
{
	if (!started || !unversioned_id)
		return;
	pthread_mutex_lock(&mutex);
	for (size_t i = 0; i < states.num; i++) {
		struct hook_state *s = states.array[i];
		if (!s->dead && strcmp(obs_source_get_unversioned_id(s->source), unversioned_id) == 0)
			sync_preset_hotkeys(s);
	}
	pthread_mutex_unlock(&mutex);
}
