#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

// Adds a "Presets" group (choose, load, delete, name, save) for the
// filter or source `source`. Does nothing when source is NULL (properties
// requested without an instance).
// Also adds the "React to Audio" group (audio-reactive.h) for filters and
// sources.
void spectrum_presets_properties(obs_properties_t *props, obs_source_t *source);

// Loads the preset called `name` of the source's type into the source
// (replacing its settings). Returns false when there is no such preset.
bool spectrum_presets_apply(obs_source_t *source, const char *name);
// Calls `callback` with the name of each saved preset of a source type
// (unversioned id).
void spectrum_presets_enum(const char *id, void (*callback)(void *param, const char *name), void *param);

#ifdef __cplusplus
}
#endif
