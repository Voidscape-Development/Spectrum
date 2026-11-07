#include "fx-filter.h"

#include <graphics/vec2.h>
#include <graphics/vec4.h>
#include <util/bmem.h>
#include <util/darray.h>

#include <math.h>
#include <string.h>

// Provided by the Move Transition plugin (see plugin-main.c).
extern float (*move_get_transition_filter)(obs_source_t *filter_from, obs_source_t **filter_to);

/* ------------------------------------------------------------------------
 * Move Transition ids
 * --------------------------------------------------------------------- */

#define MAX_MOVE_IDS 64
static const char *move_ids[MAX_MOVE_IDS];
static size_t move_id_count = 0;

void fx_filter_register_move_id(const char *id)
{
	if (move_id_count < MAX_MOVE_IDS)
		move_ids[move_id_count++] = id;
}

size_t fx_filter_move_id_count(void)
{
	return move_id_count;
}

const char *fx_filter_move_id(size_t index)
{
	return index < move_id_count ? move_ids[index] : NULL;
}

/* ------------------------------------------------------------------------
 * Helpers
 * --------------------------------------------------------------------- */

void fx_set_float(gs_effect_t *effect, const char *name, float value)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, name);
	if (p)
		gs_effect_set_float(p, value);
}

void fx_set_int(gs_effect_t *effect, const char *name, int value)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, name);
	if (p)
		gs_effect_set_int(p, value);
}

void fx_set_vec2(gs_effect_t *effect, const char *name, float x, float y)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, name);
	if (p) {
		struct vec2 v;
		vec2_set(&v, x, y);
		gs_effect_set_vec2(p, &v);
	}
}

void fx_set_vec4(gs_effect_t *effect, const char *name, float x, float y, float z, float w)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, name);
	if (p) {
		struct vec4 v;
		vec4_set(&v, x, y, z, w);
		gs_effect_set_vec4(p, &v);
	}
}

void fx_set_texture(gs_effect_t *effect, const char *name, gs_texture_t *texture)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, name);
	if (p)
		gs_effect_set_texture(p, texture);
}

float fx_lerp_setting(obs_data_t *settings, obs_data_t *to, float t, const char *key)
{
	const float a = (float)obs_data_get_double(settings, key);
	if (!to || t <= 0.0f)
		return a;
	const float b = (float)obs_data_get_double(to, key);
	return a + (b - a) * t;
}

static void color_to_vec4(uint32_t c, struct vec4 *v)
{
	vec4_set(v, (float)(c & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f, (float)((c >> 16) & 0xFF) / 255.0f,
		 (float)((c >> 24) & 0xFF) / 255.0f);
}

static bool is_reserved(const char *name)
{
	static const char *reserved[] = {"ViewProj",
					 "image",
					 "uv_size",
					 "linear_input",
					 "dither_step",
					 "time",
					 "gradient_lut",
					 "gradient_lut_size",
					 "gradient_offset",
					 "gradient_repeat",
					 "gradient_hue",
					 "pad_px",
					 NULL};
	for (size_t i = 0; reserved[i]; i++)
		if (strcmp(name, reserved[i]) == 0)
			return true;
	return false;
}

// Binds every uniform that has a setting of the same name.
static void bind_settings(gs_effect_t *effect, obs_data_t *from, obs_data_t *to, float t)
{
	const size_t count = gs_effect_get_num_params(effect);
	for (size_t i = 0; i < count; i++) {
		gs_eparam_t *param = gs_effect_get_param_by_idx(effect, i);
		struct gs_effect_param_info info;
		gs_effect_get_param_info(param, &info);
		if (is_reserved(info.name))
			continue;
		obs_data_item_t *item = obs_data_item_byname(from, info.name);
		if (!item)
			continue;
		const enum obs_data_number_type num = obs_data_item_numtype(item);
		obs_data_item_release(&item);

		switch (info.type) {
		case GS_SHADER_PARAM_FLOAT: {
			const float v = fx_lerp_setting(from, to, t, info.name);
			gs_effect_set_float(param, v);
			break;
		}
		case GS_SHADER_PARAM_INT: {
			obs_data_t *src = (to && t >= 0.5f) ? to : from;
			gs_effect_set_int(param, (int)obs_data_get_int(src, info.name));
			break;
		}
		case GS_SHADER_PARAM_BOOL: {
			obs_data_t *src = (to && t >= 0.5f) ? to : from;
			gs_effect_set_bool(param, obs_data_get_bool(src, info.name));
			break;
		}
		case GS_SHADER_PARAM_VEC4: {
			if (num != OBS_DATA_NUM_INT)
				break;
			struct vec4 a;
			color_to_vec4((uint32_t)obs_data_get_int(from, info.name), &a);
			if (to && t > 0.0f) {
				struct vec4 b;
				color_to_vec4((uint32_t)obs_data_get_int(to, info.name), &b);
				a.x += (b.x - a.x) * t;
				a.y += (b.y - a.y) * t;
				a.z += (b.z - a.z) * t;
				a.w += (b.w - a.w) * t;
			}
			gs_effect_set_vec4(param, &a);
			break;
		}
		default:
			break;
		}
	}
}

/* ------------------------------------------------------------------------
 * Lifetime
 * --------------------------------------------------------------------- */

fx_filter_t *fx_filter_create(const struct fx_filter_def *def, obs_data_t *settings, obs_source_t *context)
{
	fx_filter_t *fx = bzalloc(sizeof(fx_filter_t));
	fx->context = context;
	fx->def = def;
	fx->is_filter = obs_source_get_type(context) == OBS_SOURCE_TYPE_FILTER;
	pthread_mutex_init(&fx->mutex, NULL);
	if (def->gradient_prefix) {
		fx->has_gradient = true;
		gradient_init(&fx->gradient, context, def->gradient_prefix);
	}
	fx_filter_update(fx, settings);
	return fx;
}

void fx_filter_destroy(fx_filter_t *fx)
{
	if (!fx)
		return;
	obs_enter_graphics();
	if (fx->effect)
		gs_effect_destroy(fx->effect);
	obs_leave_graphics();
	if (fx->has_gradient)
		gradient_free(&fx->gradient);
	obs_data_release(fx->snapshot);
	pthread_mutex_destroy(&fx->mutex);
	bfree(fx);
}

void fx_filter_update(fx_filter_t *fx, obs_data_t *settings)
{
	obs_data_t *snapshot = obs_data_create_from_json(obs_data_get_json_with_defaults(settings));
	pthread_mutex_lock(&fx->mutex);
	obs_data_t *old = fx->snapshot;
	fx->snapshot = snapshot;
	pthread_mutex_unlock(&fx->mutex);
	obs_data_release(old);

	fx->width = (uint32_t)obs_data_get_int(settings, "width");
	fx->height = (uint32_t)obs_data_get_int(settings, "height");
	if (fx->has_gradient)
		gradient_update(&fx->gradient, settings);
}

obs_data_t *fx_filter_settings(fx_filter_t *fx)
{
	pthread_mutex_lock(&fx->mutex);
	obs_data_t *s = fx->snapshot;
	obs_data_addref(s);
	pthread_mutex_unlock(&fx->mutex);
	return s;
}

void fx_filter_tick(fx_filter_t *fx, float seconds)
{
	// Wrapped to keep float precision; effects use periods that divide it.
	fx->time = fmodf(fx->time + seconds, 3600.0f);
	if (fx->has_gradient)
		gradient_tick(&fx->gradient, seconds);
}

/* ------------------------------------------------------------------------
 * Rendering
 * --------------------------------------------------------------------- */

static bool ensure_effect(fx_filter_t *fx)
{
	if (fx->effect)
		return true;
	if (fx->load_failed)
		return false;
	char *path = obs_module_file(fx->def->effect_file);
	char *errors = NULL;
	fx->effect = gs_effect_create_from_file(path, &errors);
	if (!fx->effect) {
		blog(LOG_WARNING, "[Spectrum] Unable to load %s: %s", path ? path : fx->def->effect_file,
		     errors ? errors : "(no errors)");
		fx->load_failed = true;
	}
	bfree(errors);
	bfree(path);
	return fx->effect != NULL;
}

// The Move Transition counterpart of this filter (same kind), if any.
static fx_filter_t *move_target(fx_filter_t *fx, float *t)
{
	*t = 0.0f;
	if (!fx->is_filter || !move_get_transition_filter)
		return NULL;
	obs_source_t *to_source = NULL;
	const float f = move_get_transition_filter(fx->context, &to_source);
	if (!to_source || f <= 0.0f)
		return NULL;
	if (strcmp(obs_source_get_unversioned_id(to_source), obs_source_get_unversioned_id(fx->context)) != 0)
		return NULL;
	fx_filter_t *to = obs_obj_get_data(to_source);
	if (!to || to->def != fx->def)
		return NULL;
	*t = f;
	return to;
}

static void set_common(fx_filter_t *fx, gs_effect_t *e, obs_data_t *settings, obs_data_t *to, float t, uint32_t w,
		       uint32_t h, bool linear, float pad)
{
	fx_set_vec2(e, "uv_size", (float)w, (float)h);
	fx_set_float(e, "linear_input", linear ? 1.0f : 0.0f);
	const bool dither = !fx->def->dither_key || obs_data_get_bool(settings, fx->def->dither_key);
	fx_set_float(e, "dither_step", dither ? spectrum_target_dither_step() : 0.0f);
	fx_set_float(e, "time", fx->time);
	fx_set_float(e, "pad_px", pad);
	if (fx->has_gradient)
		gradient_bind_lut(&fx->gradient, e);
	bind_settings(e, settings, to, t);
	if (fx->def->set_params)
		fx->def->set_params(fx, e, settings, to, t, w, h);
}

void fx_filter_render(fx_filter_t *fx)
{
	if (!ensure_effect(fx)) {
		if (fx->is_filter)
			obs_source_skip_video_filter(fx->context);
		return;
	}

	obs_data_t *settings = fx_filter_settings(fx);
	const char *tech = fx->def->technique ? fx->def->technique(settings) : "Draw";

	if (!fx->is_filter) {
		const uint32_t w = fx->width, h = fx->height;
		if (w && h) {
			set_common(fx, fx->effect, settings, NULL, 0.0f, w, h, true, 0.0f);
			const bool previous = gs_framebuffer_srgb_enabled();
			gs_enable_framebuffer_srgb(true);
			while (gs_effect_loop(fx->effect, tech))
				gs_draw_sprite(NULL, 0, w, h);
			gs_enable_framebuffer_srgb(previous);
		}
		obs_data_release(settings);
		return;
	}

	obs_source_t *target = obs_filter_get_target(fx->context);
	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	if (!w || !h) {
		obs_source_skip_video_filter(fx->context);
		obs_data_release(settings);
		return;
	}

	const enum gs_color_space preferred[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
	const enum gs_color_space space = obs_source_get_color_space(target, OBS_COUNTOF(preferred), preferred);
	if (space == GS_CS_709_EXTENDED) {
		// HDR is not supported by these effects.
		obs_source_skip_video_filter(fx->context);
		obs_data_release(settings);
		return;
	}

	float t = 0.0f;
	fx_filter_t *to = move_target(fx, &t);
	obs_data_t *to_settings = to ? fx_filter_settings(to) : NULL;

	const uint32_t pad = fx->def->padding ? fx->def->padding(settings) : 0;
	const uint32_t ow = w + 2 * pad, oh = h + 2 * pad;

	if (obs_source_process_filter_begin_with_color_space(fx->context, gs_get_format_from_space(space), space,
							     OBS_NO_DIRECT_RENDERING)) {
		// For filters flagged OBS_SOURCE_SRGB, OBS samples the input and
		// writes the output in linear light inside
		// obs_source_process_filter_tech_end(), whatever
		// gs_get_linear_srgb() says here.
		const bool linear = (obs_source_get_output_flags(fx->context) & OBS_SOURCE_SRGB) != 0 ||
				    gs_get_linear_srgb();
		set_common(fx, fx->effect, settings, to_settings, t, ow, oh, linear, (float)pad);
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_matrix_push();
		if (pad)
			gs_matrix_translate3f(-(float)pad, -(float)pad, 0.0f);
		obs_source_process_filter_tech_end(fx->context, fx->effect, ow, oh, tech);
		gs_matrix_pop();
		gs_blend_state_pop();
	}

	obs_data_release(to_settings);
	obs_data_release(settings);
}

/* ------------------------------------------------------------------------
 * Generic obs_source_info callbacks
 * --------------------------------------------------------------------- */

void fx_cb_destroy(void *data)
{
	fx_filter_destroy(data);
}

void fx_cb_update(void *data, obs_data_t *settings)
{
	fx_filter_update(data, settings);
}

void fx_cb_tick(void *data, float seconds)
{
	fx_filter_tick(data, seconds);
}

void fx_cb_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	fx_filter_render(data);
}

uint32_t fx_cb_width(void *data)
{
	fx_filter_t *fx = data;
	return fx->width;
}

uint32_t fx_cb_height(void *data)
{
	fx_filter_t *fx = data;
	return fx->height;
}

void fx_add_size_properties(obs_properties_t *props)
{
	obs_properties_add_int(props, "width", obs_module_text("GradientSource.Width"), 1, 16384, 1);
	obs_properties_add_int(props, "height", obs_module_text("GradientSource.Height"), 1, 16384, 1);
}

void fx_size_defaults(obs_data_t *settings)
{
	struct obs_video_info ovi;
	const bool have = obs_get_video_info(&ovi);
	obs_data_set_default_int(settings, "width", have ? ovi.base_width : 1920);
	obs_data_set_default_int(settings, "height", have ? ovi.base_height : 1080);
}

void fx_add_blend_list(obs_properties_t *props, const char *key)
{
	obs_property_t *p = obs_properties_add_list(props, key, obs_module_text("Fx.BlendMode"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Normal"), 1);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Multiply"), 2);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Screen"), 3);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Overlay"), 4);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.SoftLight"), 5);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Add"), 6);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Color"), 7);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Luminosity"), 8);
	obs_property_list_add_int(p, obs_module_text("Fx.Blend.Hue"), 9);
}
