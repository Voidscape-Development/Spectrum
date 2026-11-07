#pragma once
#include <obs-module.h>

#include <util/base.h>
#include <util/dstr.h>
#include <util/darray.h>
#include <util/platform.h>

#include <stdio.h>

extern gs_texrender_t *mask_create_or_reset_texrender(gs_texrender_t *render);
gs_texrender_t *mask_create_or_reset_texrender_high(gs_texrender_t *render);
extern void mask_set_blending_parameters();
extern void label_indent(char *label, const char *label_text);
extern void mask_set_render_parameters();
extern bool mask_add_source_to_list(void *data, obs_source_t *source);
gs_effect_t *mask_load_shader_effect(gs_effect_t *effect, const char *effect_file_path);
extern char *mask_load_shader_from_file(const char *file_name);
extern void mask_setting_visibility(const char *prop_name, bool visible, obs_properties_t *props);
extern void mask_texrender_set_texture(gs_texture_t *source, gs_texrender_t *dest);
extern float (*move_get_transition_filter)(obs_source_t *filter_from, obs_source_t **filter_to);
