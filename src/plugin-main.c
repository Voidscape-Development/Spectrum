/*
Spectrum
Copyright (C) 2026 Voidscape Development

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#include <obs-module.h>
#include <plugin-support.h>

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")
OBS_MODULE_AUTHOR("Voidscape Development")

/* Blur */
extern struct obs_source_info spectrum_blur_filter;

/* Stroke / Glow / Shadow */
extern struct obs_source_info spectrum_stroke_filter;
extern struct obs_source_info spectrum_stroke_source;
extern struct obs_source_info spectrum_glow_filter;
extern struct obs_source_info spectrum_glow_source;
extern struct obs_source_info spectrum_shadow_filter;
extern struct obs_source_info spectrum_shadow_source;

/* Masks */
extern struct obs_source_info spectrum_mask_filter;

/* Gradient */
extern struct obs_source_info spectrum_gradient_source;

/* Effects */
extern struct obs_source_info spectrum_gradient_map_filter;
extern struct obs_source_info spectrum_shape_source;
extern struct obs_source_info spectrum_noise_source;

/* Animation */
extern struct obs_source_info spectrum_animate_filter;

#include "common/fx-filter.h"

// Registers a filter built on the fx framework, which supports Move
// Transition interpolation.
static void register_fx_filter(struct obs_source_info *info)
{
	obs_register_source(info);
	fx_filter_register_move_id(info->id);
}

/* Optional hook provided by the Move Transition plugin, shared by the
 * blur and mask filters so they can interpolate between filter states. */
float (*move_get_transition_filter)(obs_source_t *filter_from, obs_source_t **filter_to) = NULL;

MODULE_EXPORT const char *obs_module_name(void)
{
	return obs_module_text("Spectrum");
}

MODULE_EXPORT const char *obs_module_description(void)
{
	return obs_module_text("Spectrum.Description");
}

bool obs_module_load(void)
{
	obs_register_source(&spectrum_blur_filter);

	obs_register_source(&spectrum_stroke_filter);
	obs_register_source(&spectrum_stroke_source);
	obs_register_source(&spectrum_glow_filter);
	obs_register_source(&spectrum_glow_source);
	obs_register_source(&spectrum_shadow_filter);
	obs_register_source(&spectrum_shadow_source);

	obs_register_source(&spectrum_mask_filter);

	obs_register_source(&spectrum_gradient_source);

	register_fx_filter(&spectrum_gradient_map_filter);
	obs_register_source(&spectrum_shape_source);
	obs_register_source(&spectrum_noise_source);

	obs_register_source(&spectrum_animate_filter);

	obs_log(LOG_INFO, "loaded version %s", PLUGIN_VERSION);
	return true;
}

void obs_module_post_load(void)
{
	if (obs_get_module("move-transition") == NULL)
		return;

	const size_t fx_count = fx_filter_move_id_count();
	const char *filter_ids[2 + 64];
	size_t count = 0;
	filter_ids[count++] = spectrum_blur_filter.id;
	filter_ids[count++] = spectrum_mask_filter.id;
	for (size_t i = 0; i < fx_count && count < OBS_COUNTOF(filter_ids); i++)
		filter_ids[count++] = fx_filter_move_id(i);
	proc_handler_t *ph = obs_get_proc_handler();

	for (size_t i = 0; i < count; i++) {
		struct calldata cd;
		calldata_init(&cd);
		calldata_set_string(&cd, "filter_id", filter_ids[i]);
		if (proc_handler_call(ph, "move_get_transition_filter_function", &cd)) {
			void *callback = calldata_ptr(&cd, "callback");
			if (callback)
				move_get_transition_filter = callback;
		}
		calldata_free(&cd);
	}
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
