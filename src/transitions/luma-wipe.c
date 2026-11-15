// Spectrum Luma Wipe: wipes through the brightness of a luma map, which can
// be any image, any source (animated maps work) or built-in clouds, with a
// soft edge and an edge glow.

#include "transition-common.h"
#include "../stroke/obs-utils.h"

#include <graphics/image-file.h>
#include <util/dstr.h>

#define MAP_IMAGE 1
#define MAP_SOURCE 2
#define MAP_CLOUDS 3

struct luma_data {
	int type;
	struct source_ref map_ref;
	gs_texrender_t *map_texrender;

	struct dstr image_path;
	gs_image_file_t image; // texture created/freed inside the graphics context
	bool image_loaded;
};

static void luma_create_hook(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct luma_data *d = bzalloc(sizeof(struct luma_data));
	source_ref_init(&d->map_ref);
	dstr_init(&d->image_path);
	fx->user = d;
}

static void free_image(struct luma_data *d)
{
	obs_enter_graphics();
	gs_image_file_free(&d->image);
	obs_leave_graphics();
	d->image_loaded = false;
}

static void luma_update_hook(fx_filter_t *fx, obs_data_t *settings)
{
	struct luma_data *d = fx->user;
	d->type = (int)obs_data_get_int(settings, "lw_type");
	source_ref_set_name(&d->map_ref, obs_data_get_string(settings, "lw_source"));

	const char *path = obs_data_get_string(settings, "lw_image");
	if (!path)
		path = "";
	if (d->image_path.array && strcmp(d->image_path.array, path) == 0)
		return;
	dstr_copy(&d->image_path, path);

	// Decode outside the graphics context, then swap the texture in
	// inside it, so the render thread never sees a half loaded image.
	gs_image_file_t image;
	memset(&image, 0, sizeof(image));
	if (*path)
		gs_image_file_init(&image, path);
	obs_enter_graphics();
	gs_image_file_free(&d->image);
	d->image = image;
	gs_image_file_init_texture(&d->image);
	d->image_loaded = d->image.texture != NULL;
	obs_leave_graphics();
	if (*path && !d->image_loaded)
		blog(LOG_WARNING, "[Spectrum] Luma wipe: unable to load image '%s'", path);
}

static void luma_tick_hook(fx_filter_t *fx, float seconds)
{
	struct luma_data *d = fx->user;
	source_ref_tick(&d->map_ref, seconds);
	source_ref_update_activation(&d->map_ref, fx->context, d->type == MAP_SOURCE, seconds);
}

static void luma_destroy_hook(fx_filter_t *fx)
{
	struct luma_data *d = fx->user;
	obs_enter_graphics();
	if (d->map_texrender)
		gs_texrender_destroy(d->map_texrender);
	obs_leave_graphics();
	free_image(d);
	source_ref_free(&d->map_ref);
	dstr_free(&d->image_path);
	bfree(d);
}

// Renders the map source in float (linear light), so a smooth map doesn't
// wipe in 1/255 steps.
static gs_texture_t *render_map(obs_source_t *source, gs_texrender_t **texrender)
{
	const uint32_t width = obs_source_get_width(source);
	const uint32_t height = obs_source_get_height(source);
	if (!width || !height)
		return NULL;
	if (!*texrender)
		*texrender = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	gs_texrender_reset(*texrender);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	const bool previous = gs_framebuffer_srgb_enabled();
	gs_enable_framebuffer_srgb(false);
	if (gs_texrender_begin_with_color_space(*texrender, width, height, GS_CS_SRGB_16F)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		obs_source_video_render(source);
		gs_texrender_end(*texrender);
	}
	gs_enable_framebuffer_srgb(previous);
	gs_blend_state_pop();
	return gs_texrender_get_texture(*texrender);
}

static void luma_set_params(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, obs_data_t *to, float t,
			    uint32_t width, uint32_t height)
{
	UNUSED_PARAMETER(settings);
	UNUSED_PARAMETER(to);
	UNUSED_PARAMETER(t);
	UNUSED_PARAMETER(width);
	UNUSED_PARAMETER(height);
	struct luma_data *d = fx->user;
	gs_texture_t *map = NULL;
	bool linear = false;
	if (d->type == MAP_IMAGE && d->image_loaded) {
		map = d->image.texture;
	} else if (d->type == MAP_SOURCE) {
		obs_source_t *source = source_ref_get(&d->map_ref);
		if (source && !is_self_reference(fx->context, source)) {
			map = render_map(source, &d->map_texrender);
			linear = true;
		}
		obs_source_release(source);
	}
	fx_set_texture(effect, "lw_map", map);
	fx_set_float(effect, "lw_map_ok", map ? 1.0f : 0.0f);
	fx_set_float(effect, "lw_map_linear", linear ? 1.0f : 0.0f);
}

static const struct tr_def luma_wipe_def = {
	.fx =
		{
			.effect_file = "shaders/transitions/luma-wipe.effect",
			.gradient_prefix = TR_EDGE_PREFIX,
			.set_params = luma_set_params,
			.create = luma_create_hook,
			.update = luma_update_hook,
			.tick = luma_tick_hook,
			.destroy = luma_destroy_hook,
		},
};

static const char *luma_wipe_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("LumaWipeTransition");
}

static void *luma_wipe_create(obs_data_t *settings, obs_source_t *source)
{
	return tr_create(&luma_wipe_def, settings, source);
}

static void luma_wipe_defaults(obs_data_t *settings)
{
	tr_defaults_common(settings);
	obs_data_set_default_int(settings, "lw_type", MAP_CLOUDS);
	obs_data_set_default_string(settings, "lw_image", "");
	obs_data_set_default_string(settings, "lw_source", "");
	obs_data_set_default_double(settings, "lw_softness", 10.0);
	obs_data_set_default_double(settings, "lw_contrast", 100.0);
	obs_data_set_default_bool(settings, "lw_invert", false);
	obs_data_set_default_double(settings, "lw_scale", 200.0);
	obs_data_set_default_double(settings, "lw_seed", 0.0);
	tr_edge_defaults(settings, 0, 4.0, 100.0);
}

static bool type_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int type = (int)obs_data_get_int(settings, "lw_type");
	obs_property_set_visible(obs_properties_get(props, "lw_image"), type == MAP_IMAGE);
	obs_property_set_visible(obs_properties_get(props, "lw_source"), type == MAP_SOURCE);
	obs_property_set_visible(obs_properties_get(props, "lw_scale"), type == MAP_CLOUDS);
	obs_property_set_visible(obs_properties_get(props, "lw_seed"), type == MAP_CLOUDS);
	return true;
}

static obs_properties_t *luma_wipe_properties(void *data)
{
	fx_filter_t *fx = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(props, "lw_type", obs_module_text("LumaWipe.Map"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("LumaWipe.Map.Image"), MAP_IMAGE);
	obs_property_list_add_int(p, obs_module_text("LumaWipe.Map.Source"), MAP_SOURCE);
	obs_property_list_add_int(p, obs_module_text("LumaWipe.Map.Clouds"), MAP_CLOUDS);
	obs_property_set_modified_callback(p, type_modified);

	p = obs_properties_add_path(props, "lw_image", obs_module_text("LumaWipe.Image"), OBS_PATH_FILE,
				    "Images (*.png *.jpg *.jpeg *.bmp *.tga *.gif *.webp);;All Files (*.*)", NULL);
	obs_property_set_long_description(p, obs_module_text("LumaWipe.Image.Description"));

	p = obs_properties_add_list(props, "lw_source", obs_module_text("LumaWipe.Source"), OBS_COMBO_TYPE_EDITABLE,
				    OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(stroke_add_source_to_list, p);
	obs_enum_scenes(stroke_add_source_to_list, p);
	obs_property_list_insert_string(p, 0, "None", "");

	tr_add_easing_property(props);
	tr_slider(props, "lw_scale", "Noise.Scale", 4.0, 4000.0, 1.0, "px");
	tr_slider(props, "lw_seed", "Noise.Seed", 0.0, 1000.0, 1.0, NULL);
	tr_slider(props, "lw_softness", "Transition.Softness", 0.0, 100.0, 0.1, "%");
	tr_slider(props, "lw_contrast", "LumaWipe.Contrast", 10.0, 400.0, 0.1, "%");
	obs_properties_add_bool(props, "lw_invert", obs_module_text("LumaWipe.Invert"));

	tr_add_edge_properties(props, fx, "%", 50.0);
	tr_finish_properties(props, data);
	return props;
}

struct obs_source_info spectrum_luma_wipe_transition = {
	.id = "spectrum_luma_wipe_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = luma_wipe_name,
	.create = luma_wipe_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = tr_video_render,
	.audio_render = tr_audio_render,
	.get_properties = luma_wipe_properties,
	.get_defaults = luma_wipe_defaults,
	.video_get_color_space = tr_video_get_color_space,
};
