#include "obs-utils.h"

#define MAX_SHADER_INCLUDE_DEPTH 16

gs_texrender_t *blur_create_or_reset_texrender(gs_texrender_t *render, enum gs_color_format format)
{
	if (render && gs_texrender_get_format(render) != format) {
		gs_texrender_destroy(render);
		render = NULL;
	}
	if (!render) {
		render = gs_texrender_create(format, GS_ZS_NONE);
	} else {
		gs_texrender_reset(render);
	}
	return render;
}

void blur_set_blending_parameters(void)
{
	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_blending(false);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
}

void blur_set_render_parameters(void)
{
	// Culling
	gs_set_cull_mode(GS_NEITHER);

	// Enable all colors.
	gs_enable_color(true, true, true, true);

	// Depth- No depth test.
	gs_enable_depth_test(false);
	gs_depth_function(GS_ALWAYS);

	// Setup Stencil- No test, no write.
	gs_enable_stencil_test(false);
	gs_enable_stencil_write(false);
	gs_stencil_function(GS_STENCIL_BOTH, GS_ALWAYS);
	gs_stencil_op(GS_STENCIL_BOTH, GS_ZERO, GS_ZERO, GS_ZERO);
}

bool blur_add_source_to_list(void *data, obs_source_t *source)
{
	obs_property_t *p = data;
	const char *name = obs_source_get_name(source);
	size_t count = obs_property_list_item_count(p);
	size_t idx = 0;
	while (idx < count && strcmp(name, obs_property_list_item_string(p, idx)) > 0)
		idx++;
	obs_property_list_insert_string(p, idx, name, name);
	return true;
}

void blur_texrender_set_texture(gs_texture_t *source, gs_texrender_t *dest)
{
	gs_effect_t *pass_through = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	uint32_t w = gs_texture_get_width(source);
	uint32_t h = gs_texture_get_height(source);

	gs_eparam_t *image = gs_effect_get_param_by_name(pass_through, "image");
	gs_effect_set_texture(image, source);
	blur_set_blending_parameters();
	if (gs_texrender_begin(dest, w, h)) {
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(pass_through, "Draw"))
			gs_draw_sprite(source, 0, w, h);
		gs_texrender_end(dest);
	}
	gs_blend_state_pop();
}

bool load_effect_with_params(gs_effect_t **effect, const char *effect_file_path,
			     const struct effect_param_binding *bindings, size_t num_bindings, const char *find,
			     const char *replace)
{
	// File IO and text processing happen outside the graphics lock.
	struct dstr filename = {0};
	dstr_cat(&filename, obs_get_module_data_path(obs_current_module()));
	dstr_cat(&filename, effect_file_path);

	struct dstr shader_text = {0};
	char *raw = blur_load_shader_from_file(filename.array);
	if (raw) {
		dstr_copy(&shader_text, raw);
		bfree(raw);
		if (find && replace)
			dstr_replace(&shader_text, find, replace);
	} else {
		blog(LOG_WARNING, "[Spectrum] Unable to read shader file: %s", filename.array);
	}

	char *errors = NULL;
	obs_enter_graphics();
	if (*effect) {
		gs_effect_destroy(*effect);
		*effect = NULL;
	}
	for (size_t i = 0; i < num_bindings; i++)
		*bindings[i].param = NULL;

	if (shader_text.array)
		*effect = gs_effect_create(shader_text.array, filename.array, &errors);

	if (*effect) {
		for (size_t i = 0; i < num_bindings; i++)
			*bindings[i].param = gs_effect_get_param_by_name(*effect, bindings[i].name);
	}
	obs_leave_graphics();

	if (!*effect && shader_text.array) {
		blog(LOG_WARNING, "[Spectrum] Unable to load %s.  Errors:\n%s", effect_file_path,
		     (errors == NULL || strlen(errors) == 0 ? "(None)" : errors));
	}
	bfree(errors);
	dstr_free(&shader_text);
	dstr_free(&filename);

	return *effect != NULL;
}

static char *load_shader_from_file_depth(const char *file_name, int depth)
{
	if (depth > MAX_SHADER_INCLUDE_DEPTH) {
		blog(LOG_WARNING, "[Spectrum] Shader include depth exceeded at %s (circular #include?)", file_name);
		return NULL;
	}

	char *file = os_quick_read_utf8_file(file_name);
	if (file == NULL)
		return NULL;
	char **lines = strlist_split(file, '\n', true);
	struct dstr shader_file;
	dstr_init(&shader_file);

	// Directory of the current file, including the trailing separator.
	const char *slash = strrchr(file_name, '/');
	const char *backslash = strrchr(file_name, '\\');
	const char *sep = slash > backslash ? slash : backslash;
	const size_t dir_len = sep ? (size_t)(sep - file_name) + 1 : 0;

	for (size_t line_i = 0; lines[line_i] != NULL; line_i++) {
		const char *line = lines[line_i];
		if (strncmp(line, "#include", 8) != 0) {
			dstr_cat(&shader_file, line);
			dstr_cat(&shader_file, "\n");
			continue;
		}

		// Open the included file, place contents here.
		const char *start = strchr(line, '"');
		const char *end = start ? strchr(start + 1, '"') : NULL;
		if (!start || !end) {
			blog(LOG_WARNING, "[Spectrum] Malformed #include in %s: %s", file_name, line);
			continue;
		}
		start++;

		struct dstr include_path = {0};
		dstr_ncopy(&include_path, file_name, dir_len);
		dstr_ncat(&include_path, start, (size_t)(end - start));
		char *abs_include_path = os_get_abs_path_ptr(include_path.array);
		char *file_contents = load_shader_from_file_depth(
			abs_include_path ? abs_include_path : include_path.array, depth + 1);
		if (file_contents) {
			dstr_cat(&shader_file, file_contents);
			dstr_cat(&shader_file, "\n");
		} else {
			blog(LOG_WARNING, "[Spectrum] Unable to read included shader: %s", include_path.array);
		}
		bfree(abs_include_path);
		bfree(file_contents);
		dstr_free(&include_path);
	}

	bfree(file);
	strlist_free(lines);

	return shader_file.array;
}

// Performs loading of shader from file.  Properly includes #include directives.
char *blur_load_shader_from_file(const char *file_name)
{
	return load_shader_from_file_depth(file_name, 0);
}
