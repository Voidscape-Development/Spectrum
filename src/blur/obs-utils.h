#pragma once
#include <obs-module.h>

#include <util/base.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>

#include <stdio.h>

// Binds a named uniform in an effect to a gs_eparam_t* slot.
struct effect_param_binding {
	const char *name;
	gs_eparam_t **param;
};

#define EFFECT_PARAM(name, slot) {name, &(slot)}

// Returns `render` reset for a new frame, (re)creating it if it is NULL or
// was created with a different color format.
extern gs_texrender_t *blur_create_or_reset_texrender(gs_texrender_t *render, enum gs_color_format format);
extern void blur_set_blending_parameters(void);
extern void blur_set_render_parameters(void);
void blur_texrender_set_texture(gs_texture_t *source, gs_texrender_t *dest);
extern bool blur_add_source_to_list(void *data, obs_source_t *source);

// Loads the effect at `effect_file_path` (relative to the module data dir)
// into *effect, destroying any previous effect, and binds every parameter in
// `bindings` (unmatched bindings are set to NULL). If `find` is non-NULL, all
// occurrences of it in the shader source are replaced by `replace` before
// compiling. The swap is done while holding the graphics context, so the
// render thread never sees a destroyed effect or stale parameter handles.
// Returns true on success.
extern bool load_effect_with_params(gs_effect_t **effect, const char *effect_file_path,
				    const struct effect_param_binding *bindings, size_t num_bindings, const char *find,
				    const char *replace);

#define LOAD_EFFECT(effect_ptr, path, bindings)             \
	load_effect_with_params(effect_ptr, path, bindings, \
				OBS_COUNTOF(bindings), NULL, NULL)

extern char *blur_load_shader_from_file(const char *file_name);
