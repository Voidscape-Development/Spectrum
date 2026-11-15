#pragma once

// A reusable audio level meter with transient ("beat") detection.
//
// Bind it to an audio source by name; the source's samples arrive on the
// audio thread through an audio capture callback. The video thread reads
// the loudest normalised level and whether an onset happened since its
// last read. Samples can also be fed directly (tests, other producers).

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct spectrum_audio_meter spectrum_audio_meter_t;

spectrum_audio_meter_t *spectrum_audio_meter_create(void);
// Unbinds and frees the meter.
void spectrum_audio_meter_destroy(spectrum_audio_meter_t *m);

// Captures the audio of the source called `name` ("" or NULL unbinds).
// Cheap when already bound; retries when the source does not exist yet or
// was removed, so call it every tick.
void spectrum_audio_meter_bind(spectrum_audio_meter_t *m, const char *name);
void spectrum_audio_meter_unbind(spectrum_audio_meter_t *m);

// Levels at or below floor_db read 0, at or above ceiling_db read 1.
// sensitivity (0..1) sets how far above its recent average a sound must
// jump to count as an onset.
void spectrum_audio_meter_configure(spectrum_audio_meter_t *m, double floor_db, double ceiling_db, double sensitivity);

// Feeds float samples (one pointer per channel).
void spectrum_audio_meter_feed(spectrum_audio_meter_t *m, const float *const *planes, size_t channels, uint32_t frames,
			       uint32_t sample_rate);

// The loudest normalised level (0..1) and whether an onset was detected
// since the previous read; resets both.
void spectrum_audio_meter_read(spectrum_audio_meter_t *m, double *level, bool *onset);

// One step of an attack/release envelope follower (times in seconds).
double spectrum_envelope_step(double env, double target, double seconds, double attack, double release);

#ifdef __cplusplus
}
#endif
