#pragma once

// Keyframes and easing curves for Spectrum Animate, shared by the filter and
// the Qt curve editor.
//
// Keyframes are written as text, one per line (or separated by ';'):
//     <time in seconds> <value in %> [easing]
// e.g. "0 0", "1.5 100 bounce". The value is a percentage of the target's
// From -> To range (0 = From, 100 = To; values outside 0..100 overshoot).
// The easing names how the value travels INTO that keyframe from the
// previous one; the first keyframe's easing is ignored.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum an_ease {
	AN_EASE_LINEAR = 0,
	AN_EASE_IN,
	AN_EASE_OUT,
	AN_EASE_IN_OUT,
	AN_EASE_SMOOTH,
	AN_EASE_STEP,
	AN_EASE_BACK,
	AN_EASE_ELASTIC,
	AN_EASE_BOUNCE,
	AN_EASE_COUNT,
};

#define AN_MAX_KEYS 64

struct an_keyframe {
	double time;  // seconds
	double value; // % of the From -> To range
	int ease;     // enum an_ease, for the segment ending at this key
};

// Maps s in [0, 1] through an easing curve (back/elastic may leave [0, 1]).
double an_ease_apply(int ease, double s);

// Canonical text name of an easing ("linear", "in", "out", "in-out", ...).
const char *an_ease_name(int ease);
// Locale key of an easing's display name ("Animate.Ease.Linear", ...).
const char *an_ease_text_key(int ease);

// Parses keyframe text into `keys` (sorted by time). Returns the count.
size_t an_keys_parse(const char *text, struct an_keyframe *keys, size_t max);

// Writes keyframes as text into `out` (always NUL terminated). Returns the
// length the full text needs, like snprintf.
size_t an_keys_format(const struct an_keyframe *keys, size_t count, char *out, size_t size);

// Length of the animation: the time of the last keyframe (>= 0).
double an_keys_duration(const struct an_keyframe *keys, size_t count);

// Value (in %, not clamped) at time t.
double an_keys_eval(const struct an_keyframe *keys, size_t count, double t);

#ifdef __cplusplus
}
#endif
