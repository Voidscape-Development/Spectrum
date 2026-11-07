// Named presets for every Spectrum filter and source.
//
// Presets are stored per source type (unversioned id) in
// <module config>/presets.json as {"<id>": [{"name": ..., "settings": {...}}]}.

#include "presets.h"

#include <util/platform.h>

#include <string.h>

#define KEY_LIST "spectrum_preset"
#define KEY_NAME "spectrum_preset_name"

static char *presets_path(void)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	return obs_module_config_path("presets.json");
}

static obs_data_t *load_all(void)
{
	char *path = presets_path();
	obs_data_t *data = path ? obs_data_create_from_json_file_safe(path, "bak") : NULL;
	bfree(path);
	return data ? data : obs_data_create();
}

static void save_all(obs_data_t *data)
{
	char *path = presets_path();
	if (path)
		obs_data_save_json_safe(data, path, "tmp", "bak");
	bfree(path);
}

// Settings that belong to the preset UI itself, never saved in a preset.
static void strip_ui_keys(obs_data_t *settings)
{
	obs_data_erase(settings, KEY_LIST);
	obs_data_erase(settings, KEY_NAME);
}

static void fill_list(obs_property_t *list, const char *id)
{
	obs_property_list_clear(list);
	obs_property_list_add_string(list, obs_module_text("Presets.Choose"), "");
	obs_data_t *all = load_all();
	obs_data_array_t *presets = obs_data_get_array(all, id);
	for (size_t i = 0; presets && i < obs_data_array_count(presets); i++) {
		obs_data_t *item = obs_data_array_item(presets, i);
		const char *name = obs_data_get_string(item, "name");
		obs_property_list_add_string(list, name, name);
		obs_data_release(item);
	}
	obs_data_array_release(presets);
	obs_data_release(all);
}

static bool save_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	obs_source_t *source = data;
	obs_data_t *settings = obs_source_get_settings(source);
	const char *name = obs_data_get_string(settings, KEY_NAME);
	if (!name || !*name) {
		obs_data_release(settings);
		return false;
	}
	const char *id = obs_source_get_unversioned_id(source);

	obs_data_t *copy = obs_data_create_from_json(obs_data_get_json(settings));
	strip_ui_keys(copy);

	obs_data_t *all = load_all();
	obs_data_array_t *presets = obs_data_get_array(all, id);
	if (!presets)
		presets = obs_data_array_create();
	for (size_t i = obs_data_array_count(presets); i-- > 0;) {
		obs_data_t *item = obs_data_array_item(presets, i);
		if (strcmp(obs_data_get_string(item, "name"), name) == 0)
			obs_data_array_erase(presets, i);
		obs_data_release(item);
	}
	obs_data_t *item = obs_data_create();
	obs_data_set_string(item, "name", name);
	obs_data_set_obj(item, "settings", copy);
	obs_data_array_push_back(presets, item);
	obs_data_release(item);
	obs_data_set_array(all, id, presets);
	save_all(all);

	obs_data_set_string(settings, KEY_LIST, name);
	fill_list(obs_properties_get(props, KEY_LIST), id);

	obs_data_array_release(presets);
	obs_data_release(all);
	obs_data_release(copy);
	obs_data_release(settings);
	return true;
}

static bool load_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(p);
	obs_source_t *source = data;
	obs_data_t *settings = obs_source_get_settings(source);
	const char *name = obs_data_get_string(settings, KEY_LIST);
	const char *id = obs_source_get_unversioned_id(source);
	bool loaded = false;

	obs_data_t *all = load_all();
	obs_data_array_t *presets = obs_data_get_array(all, id);
	for (size_t i = 0; presets && name && *name && i < obs_data_array_count(presets); i++) {
		obs_data_t *item = obs_data_array_item(presets, i);
		if (strcmp(obs_data_get_string(item, "name"), name) == 0) {
			obs_data_t *preset = obs_data_get_obj(item, "settings");
			// Replace (not merge) the settings, keeping the preset UI
			// state, so values left at their defaults reset too.
			char *keep_name = bstrdup(name);
			obs_data_clear(settings);
			obs_data_apply(settings, preset);
			obs_data_set_string(settings, KEY_LIST, keep_name);
			obs_data_set_string(settings, KEY_NAME, keep_name);
			bfree(keep_name);
			obs_data_release(preset);
			loaded = true;
		}
		obs_data_release(item);
	}
	obs_data_array_release(presets);
	obs_data_release(all);

	if (loaded)
		obs_source_update(source, settings);
	obs_data_release(settings);
	return loaded;
}

static bool delete_clicked(obs_properties_t *props, obs_property_t *p, void *data)
{
	UNUSED_PARAMETER(p);
	obs_source_t *source = data;
	obs_data_t *settings = obs_source_get_settings(source);
	const char *name = obs_data_get_string(settings, KEY_LIST);
	const char *id = obs_source_get_unversioned_id(source);
	bool deleted = false;

	obs_data_t *all = load_all();
	obs_data_array_t *presets = obs_data_get_array(all, id);
	for (size_t i = presets ? obs_data_array_count(presets) : 0; i-- > 0;) {
		obs_data_t *item = obs_data_array_item(presets, i);
		if (name && *name && strcmp(obs_data_get_string(item, "name"), name) == 0) {
			obs_data_array_erase(presets, i);
			deleted = true;
		}
		obs_data_release(item);
	}
	if (deleted) {
		save_all(all);
		obs_data_set_string(settings, KEY_LIST, "");
		fill_list(obs_properties_get(props, KEY_LIST), id);
	}
	obs_data_array_release(presets);
	obs_data_release(all);
	obs_data_release(settings);
	return deleted;
}

void spectrum_presets_properties(obs_properties_t *props, obs_source_t *source)
{
	if (!source)
		return;
	obs_properties_t *group = obs_properties_create();

	obs_property_t *list = obs_properties_add_list(group, KEY_LIST, obs_module_text("Presets.Preset"),
						       OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	fill_list(list, obs_source_get_unversioned_id(source));
	obs_properties_add_button2(group, "spectrum_preset_load", obs_module_text("Presets.Load"), load_clicked,
				   source);
	obs_properties_add_button2(group, "spectrum_preset_delete", obs_module_text("Presets.Delete"), delete_clicked,
				   source);
	obs_properties_add_text(group, KEY_NAME, obs_module_text("Presets.Name"), OBS_TEXT_DEFAULT);
	obs_properties_add_button2(group, "spectrum_preset_save", obs_module_text("Presets.Save"), save_clicked,
				   source);

	obs_properties_add_group(props, "spectrum_presets_group", obs_module_text("Presets.Group"), OBS_GROUP_NORMAL,
				 group);
}
