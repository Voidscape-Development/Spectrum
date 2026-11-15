#pragma once

// Qt helpers for Spectrum Animate: the mouse-position driver and the
// keyframe curve editor.

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

// Latest mouse position, normalised to a screen: 0 = the primary screen,
// 1..n = screen n, -1 = the whole desktop. Safe to call from any thread:
// the position is read on the UI thread (QCursor isn't thread-safe), so the
// first calls return false until it arrives. Also false without a Qt GUI.
bool spectrum_cursor_position(int screen, double *x, double *y);

// Calls `cb` for each screen (index 1..n) with a display name. Only lists
// screens when called on the UI thread.
void spectrum_cursor_list_screens(void (*cb)(void *param, int index, const char *name), void *param);

// Opens the keyframe curve editor for an Animate filter (modal). Returns
// true if the keyframes were changed.
bool spectrum_curve_editor_open(obs_source_t *filter);

#ifdef __cplusplus
}
#endif
