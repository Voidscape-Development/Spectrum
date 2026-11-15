#pragma once

// "React to Audio": modulates up to two numeric settings of a Spectrum
// filter or source from the level (or the beats) of an audio source.
//
// The configuration lives in the source's own settings (spectrum_ar_*
// keys, added to every Spectrum properties view by spectrum_presets_properties).
// The modulated values are never written into the source's settings: the
// source hooks (source-hooks.c) hand the source's update callback a private
// copy of its settings with the modulated values in place, so the value the
// user set stays the saved value (and the base for "add to value" mode).

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spectrum_reactive spectrum_reactive_t;

spectrum_reactive_t *spectrum_reactive_create(void);
void spectrum_reactive_destroy(spectrum_reactive_t *r);
// Stops capturing audio (the source is going away or reacting is off).
void spectrum_reactive_stop(spectrum_reactive_t *r);

void spectrum_reactive_defaults(obs_data_t *settings);
// Adds the "React to Audio" group. Call after the source's own properties:
// its numeric properties are offered as targets.
void spectrum_reactive_properties(obs_properties_t *props, obs_source_t *source);

// Reads the configuration from the source's settings. Returns true when
// reacting is on and at least one setting is chosen.
bool spectrum_reactive_configure(spectrum_reactive_t *r, obs_data_t *settings);
bool spectrum_reactive_active(const spectrum_reactive_t *r);

// Advances by `seconds` reading the audio. Returns true when the values
// to write changed noticeably since the last write.
bool spectrum_reactive_tick(spectrum_reactive_t *r, float seconds);
// Writes the modulated values into `out`; base values and number types
// come from `base` (the source's settings).
void spectrum_reactive_write(spectrum_reactive_t *r, obs_data_t *base, obs_data_t *out);

#ifdef __cplusplus
}
#endif
