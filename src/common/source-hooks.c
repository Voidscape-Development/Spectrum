#include "source-hooks.h"

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
	obs_hotkey_pair_id enable_pair;
};

static struct hook_type types[MAX_TYPES];
static size_t type_count = 0;

// Guards `states`.
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

static size_t find_state(void *data)
{
	for (size_t i = 0; i < states.num; i++)
		if (states.array[i]->data == data)
			return i;
	return DARRAY_INVALID;
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
	set_enable_descriptions(param, calldata_string(cd, "new_name"));
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
	register_enable_pair(s);

	pthread_mutex_lock(&mutex);
	da_push_back(states, &s);
	pthread_mutex_unlock(&mutex);

	signal_handler_connect(obs_source_get_signal_handler(source), "rename", source_renamed, s);
	return data;
}

static void hook_destroy(void *data)
{
	pthread_mutex_lock(&mutex);
	const size_t idx = find_state(data);
	struct hook_state *s = idx != DARRAY_INVALID ? states.array[idx] : NULL;
	if (s)
		da_erase(states, idx);
	pthread_mutex_unlock(&mutex);

	if (!s) {
		blog(LOG_WARNING, "[Spectrum] destroy for an unknown instance (leaked)");
		return;
	}

	signal_handler_disconnect(obs_source_get_signal_handler(s->source), "rename", source_renamed, s);
	if (s->enable_pair != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(s->enable_pair);
	if (s->type->orig.destroy)
		s->type->orig.destroy(data);
	bfree(s);
}

/* ------------------------------------------------------------------------
 * Registration
 * --------------------------------------------------------------------- */

void spectrum_register_source(const struct obs_source_info *info)
{
	// Only filters get hotkeys; everything else is registered unchanged.
	if (info->type != OBS_SOURCE_TYPE_FILTER || !info->create || type_count >= MAX_TYPES) {
		obs_register_source_s(info, sizeof(struct obs_source_info));
		return;
	}
	if (!started) {
		pthread_mutex_init(&mutex, NULL);
		da_init(states);
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
	wrapped.destroy = hook_destroy;
	obs_register_source_s(&wrapped, sizeof(struct obs_source_info));
}

void spectrum_hooks_shutdown(void)
{
	if (!started)
		return;
	pthread_mutex_lock(&mutex);
	// OBS destroys every source before unloading modules; anything still
	// listed is left to its destroy wrapper.
	if (states.num == 0)
		da_free(states);
	pthread_mutex_unlock(&mutex);
}
