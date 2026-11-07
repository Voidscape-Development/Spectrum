#include "obs-utils.h"

gs_texrender_t *stroke_create_or_reset_texrender(gs_texrender_t *render)
{
	if (!render) {
		render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	} else {
		gs_texrender_reset(render);
	}
	return render;
}

gs_texrender_t *stroke_create_or_reset_texrender_high(gs_texrender_t *render)
{
	if (!render) {
		render = gs_texrender_create(GS_RG32F, GS_ZS_NONE);
	} else {
		gs_texrender_reset(render);
	}
	return render;
}

gs_texrender_t *create_or_reset_texrender_format(gs_texrender_t *render, enum gs_color_format format)
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

void stroke_set_blending_parameters()
{
	gs_blend_state_push();
	gs_reset_blend_state();
	gs_enable_blending(false);
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
}

bool stroke_add_source_to_list(void *data, obs_source_t *source)
{
	// Audio-only sources (microphones, etc.) have nothing to draw.
	if ((obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO) == 0) {
		return true;
	}
	obs_property_t *p = data;
	const char *name = obs_source_get_name(source);
	size_t count = obs_property_list_item_count(p);
	size_t idx = 0;
	while (idx < count && strcmp(name, obs_property_list_item_string(p, idx)) > 0)
		idx++;
	obs_property_list_insert_string(p, idx, name, name);
	return true;
}

// Loads the shader file at `effect_file_path` into *effect
gs_effect_t *stroke_load_shader_effect(gs_effect_t *effect, const char *effect_file_path)
{
	if (effect != NULL) {
		obs_enter_graphics();
		gs_effect_destroy(effect);
		effect = NULL;
		obs_leave_graphics();
	}
	char *shader_text = NULL;
	struct dstr filename = {0};
	dstr_cat(&filename, obs_get_module_data_path(obs_current_module()));
	dstr_cat(&filename, effect_file_path);
	shader_text = stroke_load_shader_from_file(filename.array);
	if (!shader_text) {
		blog(LOG_WARNING, "[Spectrum] Unable to read shader file %s", filename.array);
		dstr_free(&filename);
		return NULL;
	}
	char *errors = NULL;

	obs_enter_graphics();
	effect = gs_effect_create(shader_text, NULL, &errors);
	obs_leave_graphics();

	bfree(shader_text);

	if (effect == NULL) {
		blog(LOG_WARNING, "[Spectrum] Unable to load %s file.  Errors:\n%s", effect_file_path,
		     (errors == NULL || strlen(errors) == 0 ? "(None)" : errors));
		bfree(errors);
	}

	dstr_free(&filename);

	return effect;
}

// Performs loading of shader from file.  Properly includes #include directives.
char *stroke_load_shader_from_file(const char *file_name)
{
	char *file = os_quick_read_utf8_file(file_name);
	if (file == NULL)
		return NULL;
	char **lines = strlist_split(file, '\n', true);
	struct dstr shader_file;
	dstr_init(&shader_file);

	size_t line_i = 0;
	while (lines[line_i] != NULL) {
		char *line = lines[line_i];
		line_i++;
		if (strncmp(line, "#include", 8) == 0) {
			// Open the included file, place contents here.
			const char *pos = strrchr(file_name, '/');
			const size_t length = pos - file_name + 1;
			struct dstr include_path = {0};
			dstr_ncopy(&include_path, file_name, length);
			char *start = strchr(line, '"') + 1;
			char *end = strrchr(line, '"');

			dstr_ncat(&include_path, start, end - start);
			char *abs_include_path = os_get_abs_path_ptr(include_path.array);
			char *file_contents = abs_include_path ? stroke_load_shader_from_file(abs_include_path) : NULL;
			if (file_contents) {
				dstr_cat(&shader_file, file_contents);
			} else {
				blog(LOG_WARNING, "[Spectrum] Unable to read included shader file %s",
				     include_path.array);
			}
			dstr_cat(&shader_file, "\n");
			bfree(abs_include_path);
			bfree(file_contents);
			dstr_free(&include_path);
		} else {
			// else place current line here.
			dstr_cat(&shader_file, line);
			dstr_cat(&shader_file, "\n");
		}
	}

	bfree(file);
	strlist_free(lines);
	return shader_file.array;
}

void stroke_setting_visibility(const char *prop_name, bool visible, obs_properties_t *props)
{
	obs_property_t *p = obs_properties_get(props, prop_name);
	obs_property_set_enabled(p, visible);
	obs_property_set_visible(p, visible);
}

void stroke_texrender_set_texture(gs_texture_t *source, gs_texrender_t *dest)
{
	gs_effect_t *pass_through = obs_get_base_effect(OBS_EFFECT_DEFAULT);

	uint32_t w = gs_texture_get_width(source);
	uint32_t h = gs_texture_get_height(source);

	gs_eparam_t *image = gs_effect_get_param_by_name(pass_through, "image");
	gs_effect_set_texture(image, source);
	stroke_set_blending_parameters();
	if (gs_texrender_begin(dest, w, h)) {
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(pass_through, "Draw"))
			gs_draw_sprite(source, 0, w, h);
		gs_texrender_end(dest);
	}
	gs_blend_state_pop();
}

void source_ref_init(struct source_ref *ref)
{
	memset(ref, 0, sizeof(*ref));
	pthread_mutex_init_value(&ref->mutex);
	pthread_mutex_init(&ref->mutex, NULL);
	dstr_init_copy(&ref->name, "");
	dstr_init_copy(&ref->resolved_name, "");
}

static void release_held(struct source_ref *ref)
{
	obs_source_t *held = ref->held ? obs_weak_source_get_source(ref->held) : NULL;
	if (held) {
		if (ref->held_view == 2) {
			obs_source_dec_active(held);
		} else if (ref->held_view == 1) {
			obs_source_dec_showing(held);
		}
		obs_source_release(held);
	}
	obs_weak_source_release(ref->held);
	ref->held = NULL;
	ref->held_view = 0;
}

void source_ref_free(struct source_ref *ref)
{
	release_held(ref);
	obs_weak_source_release(ref->weak);
	ref->weak = NULL;
	dstr_free(&ref->name);
	dstr_free(&ref->resolved_name);
	pthread_mutex_destroy(&ref->mutex);
}

void source_ref_set_name(struct source_ref *ref, const char *name)
{
	if (!name) {
		name = "";
	}
	pthread_mutex_lock(&ref->mutex);
	// An empty dstr has a NULL array, which dstr_cmp() doesn't handle.
	const char *current = ref->name.array ? ref->name.array : "";
	if (strcmp(current, name) != 0) {
		dstr_copy(&ref->name, name);
		ref->name_changed = true;
	}
	pthread_mutex_unlock(&ref->mutex);
}

// How often to look again for a named source that doesn't exist (yet), e.g.
// while a scene collection is still loading, or after it was deleted.
#define SOURCE_REF_RETRY_SECONDS 0.5f

void source_ref_tick(struct source_ref *ref, float seconds)
{
	bool changed = false;
	pthread_mutex_lock(&ref->mutex);
	if (ref->name_changed) {
		dstr_copy_dstr(&ref->resolved_name, &ref->name);
		ref->name_changed = false;
		changed = true;
	}
	pthread_mutex_unlock(&ref->mutex);

	// Drop the reference if the name changed or the source was removed, so
	// it is looked up again (a source can be re-created with the same name).
	if (ref->weak && (changed || obs_weak_source_expired(ref->weak))) {
		obs_weak_source_release(ref->weak);
		ref->weak = NULL;
	}
	if (changed) {
		ref->retry_timer = 0.0f;
	}

	if (ref->weak || dstr_is_empty(&ref->resolved_name)) {
		return;
	}
	ref->retry_timer -= seconds;
	if (ref->retry_timer > 0.0f) {
		return;
	}
	ref->retry_timer = SOURCE_REF_RETRY_SECONDS;

	obs_source_t *source = obs_get_source_by_name(ref->resolved_name.array);
	if (source) {
		ref->weak = obs_source_get_weak_source(source);
		obs_source_release(source);
	}
}

obs_source_t *source_ref_get(struct source_ref *ref)
{
	return ref->weak ? obs_weak_source_get_source(ref->weak) : NULL;
}

bool source_ref_has_name(struct source_ref *ref)
{
	return !dstr_is_empty(&ref->resolved_name);
}

gs_texture_t *render_source_to_texture(obs_source_t *source, gs_texrender_t **texrender)
{
	const enum gs_color_space space = GS_CS_SRGB;
	const enum gs_color_format format = GS_RGBA;

	const uint32_t width = obs_source_get_width(source);
	const uint32_t height = obs_source_get_height(source);
	if (!width || !height) {
		return NULL;
	}

	*texrender = create_or_reset_texrender_format(*texrender, format);

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin_with_color_space(*texrender, width, height, space)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		obs_source_video_render(source);
		gs_texrender_end(*texrender);
	}
	gs_blend_state_pop();

	return gs_texrender_get_texture(*texrender);
}

bool is_self_reference(obs_source_t *context, obs_source_t *source)
{
	if (!source) {
		return false;
	}
	if (source == context) {
		return true;
	}
	// For a filter: its parent source, which renders this filter.
	if (obs_source_get_type(context) == OBS_SOURCE_TYPE_FILTER) {
		return source == obs_filter_get_parent(context);
	}
	return false;
}

static bool is_supported_space(enum gs_color_space space)
{
	return space == GS_CS_SRGB || space == GS_CS_SRGB_16F || space == GS_CS_709_EXTENDED;
}

enum gs_color_space filter_working_space(obs_source_t *filter, size_t count,
					 const enum gs_color_space *preferred_spaces)
{
	// Work in the space we are drawn into when we can. OBS then converts
	// the filtered source on the way in (tonemapping HDR for an SDR
	// canvas, or expanding SDR for an HDR one) rather than converting our
	// output, which would go through a texture of the source's size and
	// clip padding drawn outside the source's bounds.
	for (size_t i = 0; i < count; i++) {
		if (is_supported_space(preferred_spaces[i])) {
			return preferred_spaces[i];
		}
	}

	const enum gs_color_space potential_spaces[] = {
		GS_CS_SRGB,
		GS_CS_SRGB_16F,
		GS_CS_709_EXTENDED,
	};
	obs_source_t *target = obs_filter_get_target(filter);
	return target ? obs_source_get_color_space(target, OBS_COUNTOF(potential_spaces), potential_spaces)
		      : GS_CS_SRGB;
}

static void set_effect_flag(gs_effect_t *effect, const char *name, bool value)
{
	gs_eparam_t *param = gs_effect_get_param_by_name(effect, name);
	if (param) {
		gs_effect_set_float(param, value ? 1.0f : 0.0f);
	}
}

void set_encode_input(gs_effect_t *effect, enum gs_color_space space)
{
	set_effect_flag(effect, "encode_input", space != GS_CS_SRGB);
}

void set_hdr_output(gs_effect_t *effect, enum gs_color_space space)
{
	set_effect_flag(effect, "hdr_output", space != GS_CS_SRGB);
}

struct find_source_data {
	obs_source_t *target;
	bool found;
};

static void find_source_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	UNUSED_PARAMETER(parent);
	struct find_source_data *data = param;
	if (child == data->target) {
		data->found = true;
	}
}

// True if showing `child` would (indirectly) show `owner` again.
static bool would_cycle(obs_source_t *child, obs_source_t *owner)
{
	if (child == owner) {
		return true;
	}
	struct find_source_data data = {owner, false};
	obs_source_enum_full_tree(child, find_source_cb, &data);
	return data.found;
}

// How often to re-check for a cycle while holding a source: scenes can be
// edited to create one after the reference was made.
#define SOURCE_REF_CYCLE_CHECK_SECONDS 1.0f

void source_ref_update_activation(struct source_ref *ref, obs_source_t *owner, bool used, float seconds)
{
	obs_source_t *child = used ? source_ref_get(ref) : NULL;
	int want_view = 0;
	if (child && owner) {
		want_view = obs_source_active(owner) ? 2 : obs_source_showing(owner) ? 1 : 0;
	}

	obs_source_t *held = ref->held ? obs_weak_source_get_source(ref->held) : NULL;

	if (child && want_view) {
		ref->cycle_timer -= seconds;
		if (child != held || ref->cycle_timer <= 0.0f) {
			ref->cycle = would_cycle(child, owner);
			ref->cycle_timer = SOURCE_REF_CYCLE_CHECK_SECONDS;
		}
		if (ref->cycle) {
			want_view = 0;
		}
	}

	if (child != held || want_view != ref->held_view) {
		// Take the new hold before dropping the old one, so a source
		// that stays referenced never briefly stops showing (which
		// would e.g. make a browser source refresh).
		if (child && want_view) {
			if (want_view == 2) {
				obs_source_inc_active(child);
			} else {
				obs_source_inc_showing(child);
			}
		}
		release_held(ref);
		if (child && want_view) {
			ref->held = obs_source_get_weak_source(child);
			ref->held_view = want_view;
		}
	}

	obs_source_release(held);
	obs_source_release(child);
}
