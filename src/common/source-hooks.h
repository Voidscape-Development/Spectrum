#pragma once

// Enable/Disable hotkeys for every Spectrum filter, added without touching
// each filter's code: plugin-main.c routes obs_register_source() through
// spectrum_register_source(), which registers filters with wrapped
// create/destroy callbacks. Each filter instance gets an Enable/Disable
// hotkey pair (Settings > Hotkeys, saved with the scene collection; bind
// one key to both to toggle).
//
// Sources and transitions are registered unchanged.

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

void spectrum_register_source(const struct obs_source_info *info);
// Removes the tick callback (obs_module_unload).
void spectrum_hooks_shutdown(void);

#ifdef __cplusplus
}
#endif
