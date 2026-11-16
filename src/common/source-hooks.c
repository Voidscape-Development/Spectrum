#include "source-hooks.h"
#include "audio-reactive.h"

#include <util/darray.h>
#include <util/dstr.h>
#include <util/threading.h>

#include <stdio.h>
#include <string.h>

#define MAX_TYPES 128
#define HOTKEY_ENABLE "spectrum.enable"
#define HOTKEY_DISABLE "spectrum.disable"

struct hook_type {
	struct obs_source_info orig;
	char id[256]; // as OBS reports it: versioned types get "_v<version>"
};

struct hook_state {
	const struct hook_type *type;
	obs_source_t *source; // not owned: valid until the destroy wrapper
	void *data;           // the instance's own data
	bool dead;            // destroyed; freed by the next tick

	obs_hotkey_pair_id enable_pair;

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

static char *describe(const char *text_key, const char *source_name)
{
	struct dstr s = {0};
	dstr_copy(&s, obs_module_text(text_key));
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

static void set_enable_descriptions(struct hook_state *s, const char *name)
{
	if (s->enable_pair == OBS_INVALID_HOTKEY_PAIR_ID)
		return;
	char *on = describe("Hotkeys.Enable", name);
	char *off = describe("Hotkeys.Disable", name);
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
	char *on = describe("Hotkeys.Enable", name);
	char *off = describe("Hotkeys.Disable", name);
	s->enable_pair = obs_hotkey_pair_register_source(s->source, HOTKEY_ENABLE, on, HOTKEY_DISABLE, off,
							 enable_pressed, disable_pressed, s, s);
	bfree(on);
	bfree(off);
}

static void source_renamed(void *param, calldata_t *cd)
{
	struct hook_state *s = param;
	const char *name = calldata_string(cd, "new_name");
	pthread_mutex_lock(&mutex);
	if (!s->dead)
		set_enable_descriptions(s, name);
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
		bfree(s);
		da_erase(states, i--);
	}
	pthread_mutex_unlock(&mutex);
}
