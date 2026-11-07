#pragma once

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

// Adds a "Presets" group (choose, load, delete, name, save) for the
// filter or source `source`. Does nothing when source is NULL (properties
// requested without an instance).
void spectrum_presets_properties(obs_properties_t *props, obs_source_t *source);

#ifdef __cplusplus
}
#endif
