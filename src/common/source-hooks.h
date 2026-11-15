#pragma once

// Behaviour shared by every Spectrum filter and source, added without
// touching each filter's code: plugin-main.c routes obs_register_source()
// through spectrum_register_source(), which registers a copy of the
// obs_source_info whose create/update/destroy/defaults callbacks are
// wrapped. The wrappers keep a list of live instances and add:
//
//  * Hotkeys (Settings > Hotkeys, saved with the scene collection like any
//    source hotkey): an Enable/Disable pair on every filter (bind one key to
//    both to toggle) and "Apply preset" for each saved preset of the type.
//  * React to Audio (audio-reactive.h): every tick, the modulated values are
//    handed to the instance's own update callback in a private copy of its
//    settings. The saved settings are never written, so they keep the value
//    the user set; when the user changes a setting, the update wrapper
//    re-applies the modulation in the same call, so there is no flicker.
//
// Transitions are registered unchanged.

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

void spectrum_register_source(const struct obs_source_info *info);
// Removes the tick callback (obs_module_unload).
void spectrum_hooks_shutdown(void);
// Re-syncs the preset hotkeys of every instance of the type (call after
// saving or deleting a preset).
void spectrum_hooks_presets_changed(const char *unversioned_id);

#ifdef __cplusplus
}
#endif
