// Spectrum Composite: blends another source or an image file onto the
// filtered source with Photoshop-style blend modes, an opacity, a placement
// (stretch, fit, fill, tile or original size, then moved, scaled and
// rotated) and an optional opacity mask (another source or a gradient).

#include "../common/fx-filter.h"
#include "../stroke/obs-utils.h"
#include "../common/audio-reactive.h"

#include <util/dstr.h>

#define CM_GRADIENT "cm_mask_gradient_"

#define LAYER_SOURCE 1
#define LAYER_IMAGE 2

#define PLACE_STRETCH 1
#define PLACE_FIT 2
#define PLACE_FILL 3
#define PLACE_TILE 4
#define PLACE_ORIGINAL 5

#define MASK_NONE 0
#define MASK_LUMA 1
#define MASK_ALPHA 2
#define MASK_GRADIENT 3

struct composite_data {
	struct source_ref layer_ref;
	struct source_ref mask_ref;
	gs_texrender_t *layer_texrender;
	gs_texrender_t *mask_texrender;

	// Image file layer: a private image source (it handles every format
	// OBS reads, and animated GIFs).
	pthread_mutex_t image_mutex;
	obs_source_t *image; // protected by image_mutex
	struct dstr image_path;

	int layer_type;
	int mask_type;
};

static void composite_create_hook(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	struct composite_data *d = bzalloc(sizeof(struct composite_data));
	source_ref_init(&d->layer_ref);
	source_ref_init(&d->mask_ref);
	pthread_mutex_init(&d->image_mutex, NULL);
	fx->user = d;
}

static void set_image_source(struct composite_data *d, obs_source_t *image)
{
	pthread_mutex_lock(&d->image_mutex);
	obs_source_t *old = d->image;
	d->image = image;
	pthread_mutex_unlock(&d->image_mutex);
	if (old) {
		obs_source_dec_showing(old);
		obs_source_release(old);
	}
}

static void composite_update_hook(fx_filter_t *fx, obs_data_t *settings)
{
	struct composite_data *d = fx->user;
	d->layer_type = (int)obs_data_get_int(settings, "cm_layer_type");
	d->mask_type = (int)obs_data_get_int(settings, "cm_mask_type");
	source_ref_set_name(&d->layer_ref, obs_data_get_string(settings, "cm_layer_source"));
	source_ref_set_name(&d->mask_ref, obs_data_get_string(settings, "cm_mask_source"));

	const char *path = d->layer_type == LAYER_IMAGE ? obs_data_get_string(settings, "cm_layer_image") : "";
	if (!path)
		path = "";
	if (strcmp(path, d->image_path.array ? d->image_path.array : "") != 0) {
		dstr_copy(&d->image_path, path);
		obs_source_t *image = NULL;
		if (*path) {
			obs_data_t *is = obs_data_create();
			obs_data_set_string(is, "file", path);
			image = obs_source_create_private("image_source", "Spectrum Composite Image", is);
			obs_data_release(is);
			if (image)
				obs_source_inc_showing(image);
		}
		set_image_source(d, image);
	}
}

static void composite_tick_hook(fx_filter_t *fx, float seconds)
{
	struct composite_data *d = fx->user;
	source_ref_tick(&d->layer_ref, seconds);
	source_ref_tick(&d->mask_ref, seconds);
	obs_source_t *owner = obs_filter_get_parent(fx->context);
	if (owner) {
		const bool enabled = obs_source_enabled(fx->context);
		source_ref_update_activation(&d->layer_ref, owner, enabled && d->layer_type == LAYER_SOURCE, seconds);
		source_ref_update_activation(&d->mask_ref, owner,
					     enabled && (d->mask_type == MASK_LUMA || d->mask_type == MASK_ALPHA),
					     seconds);
	}
}

static void composite_destroy_hook(fx_filter_t *fx)
{
	struct composite_data *d = fx->user;
	obs_enter_graphics();
	gs_texrender_destroy(d->layer_texrender);
	gs_texrender_destroy(d->mask_texrender);
	obs_leave_graphics();
	set_image_source(d, NULL);
	pthread_mutex_destroy(&d->image_mutex);
	dstr_free(&d->image_path);
	source_ref_free(&d->layer_ref);
	source_ref_free(&d->mask_ref);
	bfree(d);
}

// Renders a source in float (linear light, premultiplied alpha, as OBS
// composites), so smooth layers and masks don't step in 1/255 increments.
static gs_texture_t *render_layer(obs_source_t *source, gs_texrender_t **texrender, uint32_t *w, uint32_t *h)
{
	const uint32_t width = obs_source_get_width(source);
	const uint32_t height = obs_source_get_height(source);
	if (!width || !height)
		return NULL;
	if (!*texrender)
		*texrender = gs_texrender_create(GS_RGBA16F, GS_ZS_NONE);
	gs_texrender_reset(*texrender);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
	bool ok = false;
	if (gs_texrender_begin_with_color_space(*texrender, width, height, GS_CS_SRGB_16F)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f, 100.0f);
		obs_source_video_render(source);
		gs_texrender_end(*texrender);
		ok = true;
	}
	gs_blend_state_pop();
	*w = width;
	*h = height;
	return ok ? gs_texrender_get_texture(*texrender) : NULL;
}

// A referenced source's texture, unless it would render this filter again.
static gs_texture_t *render_ref(fx_filter_t *fx, struct source_ref *ref, gs_texrender_t **texrender, uint32_t *w,
				uint32_t *h)
{
	gs_texture_t *tex = NULL;
	obs_source_t *source = source_ref_get(ref);
	if (source && !ref->cycle && !is_self_reference(fx->context, source))
		tex = render_layer(source, texrender, w, h);
	obs_source_release(source);
	return tex;
}

static void composite_set_params(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, obs_data_t *to, float t,
				 uint32_t width, uint32_t height)
{
	UNUSED_PARAMETER(to);
	UNUSED_PARAMETER(t);
	struct composite_data *d = fx->user;

	uint32_t lw = 0, lh = 0;
	gs_texture_t *layer = NULL;
	if (d->layer_type == LAYER_IMAGE) {
		pthread_mutex_lock(&d->image_mutex);
		obs_source_t *image = obs_source_get_ref(d->image);
		pthread_mutex_unlock(&d->image_mutex);
		if (image)
			layer = render_layer(image, &d->layer_texrender, &lw, &lh);
		obs_source_release(image);
	} else {
		layer = render_ref(fx, &d->layer_ref, &d->layer_texrender, &lw, &lh);
	}

	// Size of the layer before the scale setting, in output pixels.
	float bw = (float)lw, bh = (float)lh;
	if (layer) {
		const int placement = (int)obs_data_get_int(settings, "cm_placement");
		const float sx = (float)width / (float)lw, sy = (float)height / (float)lh;
		if (placement == PLACE_STRETCH) {
			bw = (float)width;
			bh = (float)height;
		} else if (placement == PLACE_FIT || placement == PLACE_FILL) {
			const float s = placement == PLACE_FIT ? (sx < sy ? sx : sy) : (sx > sy ? sx : sy);
			bw = (float)lw * s;
			bh = (float)lh * s;
		}
	}
	fx_set_texture(effect, "layer_tex", layer);
	fx_set_float(effect, "layer_enabled", layer ? 1.0f : 0.0f);
	fx_set_vec2(effect, "layer_size", bw > 0.0f ? bw : 1.0f, bh > 0.0f ? bh : 1.0f);
	fx_set_vec2(effect, "layer_texels", lw ? (float)lw : 1.0f, lh ? (float)lh : 1.0f);

	gs_texture_t *mask = NULL;
	int mask_mode = MASK_NONE;
	uint32_t mw = 0, mh = 0;
	if (d->mask_type == MASK_LUMA || d->mask_type == MASK_ALPHA) {
		mask = render_ref(fx, &d->mask_ref, &d->mask_texrender, &mw, &mh);
	} else if (d->mask_type == MASK_GRADIENT) {
		const bool follow = obs_data_get_bool(settings, "cm_mask_layer") && layer;
		const uint32_t gw = follow ? (uint32_t)(bw + 0.5f) : width;
		const uint32_t gh = follow ? (uint32_t)(bh + 0.5f) : height;
		mask = gradient_render_fill(&fx->gradient, gw ? gw : 1, gh ? gh : 1);
	}
	if (mask)
		mask_mode = d->mask_type;
	fx_set_texture(effect, "mask_tex", mask);
	fx_set_int(effect, "mask_mode", mask_mode);
}

static const struct fx_filter_def composite_def = {
	.effect_file = "shaders/fx/composite.effect",
	.gradient_prefix = CM_GRADIENT,
	.dither_key = "cm_dither",
	.set_params = composite_set_params,
	.create = composite_create_hook,
	.update = composite_update_hook,
	.tick = composite_tick_hook,
	.destroy = composite_destroy_hook,
};

static const char *composite_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("CompositeFilter");
}

static void *composite_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&composite_def, settings, source);
}

static void composite_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "cm_layer_type", LAYER_SOURCE);
	obs_data_set_default_string(settings, "cm_layer_source", "");
	obs_data_set_default_string(settings, "cm_layer_image", "");
	obs_data_set_default_int(settings, "cm_blend", 1);
	obs_data_set_default_double(settings, "cm_opacity", 100.0);
	obs_data_set_default_int(settings, "cm_placement", PLACE_STRETCH);
	obs_data_set_default_double(settings, "cm_x", 50.0);
	obs_data_set_default_double(settings, "cm_y", 50.0);
	obs_data_set_default_double(settings, "cm_scale", 100.0);
	obs_data_set_default_double(settings, "cm_rotation", 0.0);
	obs_data_set_default_int(settings, "cm_composite", 1);
	obs_data_set_default_bool(settings, "cm_swap", false);
	obs_data_set_default_int(settings, "cm_mask_type", MASK_NONE);
	obs_data_set_default_string(settings, "cm_mask_source", "");
	obs_data_set_default_bool(settings, "cm_mask_invert", false);
	obs_data_set_default_bool(settings, "cm_mask_layer", false);
	obs_data_set_default_bool(settings, "cm_dither", true);
	gradient_defaults(settings, CM_GRADIENT);

	// The mask gradient defaults to transparent -> opaque white.
	obs_data_t *tmp = obs_data_create();
	const struct gradient_stop stops[2] = {
		{0.0f, 0x00FFFFFFu, 0.5f, GRADIENT_EASE_LINEAR},
		{1.0f, 0xFFFFFFFFu, 0.5f, GRADIENT_EASE_LINEAR},
	};
	gradient_write_stops(tmp, CM_GRADIENT, stops, 2);
	obs_data_array_t *arr = obs_data_get_array(tmp, CM_GRADIENT "stops");
	obs_data_set_default_array(settings, CM_GRADIENT "stops", arr);
	obs_data_array_release(arr);
	obs_data_release(tmp);
}

static void vis(obs_properties_t *props, const char *key, bool visible)
{
	obs_property_set_visible(obs_properties_get(props, key), visible);
}

static bool composite_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int layer = (int)obs_data_get_int(settings, "cm_layer_type");
	vis(props, "cm_layer_source", layer == LAYER_SOURCE);
	vis(props, "cm_layer_image", layer == LAYER_IMAGE);
	const int mask = (int)obs_data_get_int(settings, "cm_mask_type");
	vis(props, "cm_mask_source", mask == MASK_LUMA || mask == MASK_ALPHA);
	vis(props, "cm_mask_invert", mask != MASK_NONE);
	vis(props, "cm_mask_layer", mask != MASK_NONE);
	gradient_set_visible(props, CM_GRADIENT, mask == MASK_GRADIENT);
	if (mask == MASK_GRADIENT)
		gradient_refresh_visibility(props, CM_GRADIENT, settings);
	return true;
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static obs_property_t *source_list(obs_properties_t *props, const char *key, const char *text)
{
	obs_property_t *p = obs_properties_add_list(props, key, obs_module_text(text), OBS_COMBO_TYPE_EDITABLE,
						    OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(stroke_add_source_to_list, p);
	obs_enum_scenes(stroke_add_source_to_list, p);
	obs_property_list_insert_string(p, 0, "None", "");
	return p;
}

static obs_properties_t *composite_properties(void *data)
{
	fx_filter_t *fx = data;
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *g;
	obs_property_t *p;

	// Layer
	g = obs_properties_create();
	p = obs_properties_add_list(g, "cm_layer_type", obs_module_text("Composite.LayerType"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Composite.LayerType.Source"), LAYER_SOURCE);
	obs_property_list_add_int(p, obs_module_text("Composite.LayerType.Image"), LAYER_IMAGE);
	obs_property_set_modified_callback(p, composite_modified);
	source_list(g, "cm_layer_source", "Composite.LayerSource");
	obs_properties_add_path(g, "cm_layer_image", obs_module_text("Composite.LayerImage"), OBS_PATH_FILE,
				"Images (*.png *.jpg *.jpeg *.bmp *.tga *.gif *.webp *.psd);;All files (*.*)", NULL);
	fx_add_blend_list(g, "cm_blend");
	slider(g, "cm_opacity", "Composite.Opacity", 0.0, 100.0, 0.1, "%");
	p = obs_properties_add_list(g, "cm_composite", obs_module_text("Composite.Mode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Composite.Mode.Over"), 1);
	obs_property_list_add_int(p, obs_module_text("Composite.Mode.Inside"), 2);
	obs_property_set_long_description(p, obs_module_text("Composite.Mode.Description"));
	p = obs_properties_add_bool(g, "cm_swap", obs_module_text("Composite.Swap"));
	obs_property_set_long_description(p, obs_module_text("Composite.Swap.Description"));
	obs_properties_add_group(props, "cm_layer_group", obs_module_text("Composite.Layer"), OBS_GROUP_NORMAL, g);

	// Placement
	g = obs_properties_create();
	p = obs_properties_add_list(g, "cm_placement", obs_module_text("Composite.Placement"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Composite.Placement.Stretch"), PLACE_STRETCH);
	obs_property_list_add_int(p, obs_module_text("Composite.Placement.Fit"), PLACE_FIT);
	obs_property_list_add_int(p, obs_module_text("Composite.Placement.Fill"), PLACE_FILL);
	obs_property_list_add_int(p, obs_module_text("Composite.Placement.Tile"), PLACE_TILE);
	obs_property_list_add_int(p, obs_module_text("Composite.Placement.Original"), PLACE_ORIGINAL);
	slider(g, "cm_x", "Composite.PositionX", -100.0, 200.0, 0.1, "%");
	slider(g, "cm_y", "Composite.PositionY", -100.0, 200.0, 0.1, "%");
	slider(g, "cm_scale", "Composite.Scale", 1.0, 1000.0, 0.1, "%");
	slider(g, "cm_rotation", "Composite.Rotation", -360.0, 360.0, 0.1, "°");
	obs_properties_add_group(props, "cm_place_group", obs_module_text("Composite.PlacementGroup"), OBS_GROUP_NORMAL,
				 g);

	// Opacity mask
	g = obs_properties_create();
	p = obs_properties_add_list(g, "cm_mask_type", obs_module_text("Composite.MaskType"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Composite.Mask.None"), MASK_NONE);
	obs_property_list_add_int(p, obs_module_text("Composite.Mask.Luma"), MASK_LUMA);
	obs_property_list_add_int(p, obs_module_text("Composite.Mask.Alpha"), MASK_ALPHA);
	obs_property_list_add_int(p, obs_module_text("Composite.Mask.Gradient"), MASK_GRADIENT);
	obs_property_set_modified_callback(p, composite_modified);
	source_list(g, "cm_mask_source", "Composite.MaskSource");
	obs_properties_add_bool(g, "cm_mask_invert", obs_module_text("Composite.MaskInvert"));
	obs_properties_add_bool(g, "cm_mask_layer", obs_module_text("Composite.MaskLayer"));
	obs_properties_add_group(props, "cm_mask_group", obs_module_text("Composite.MaskGroup"), OBS_GROUP_NORMAL, g);

	gradient_properties(props, CM_GRADIENT, fx ? &fx->gradient : NULL);

	obs_properties_add_bool(props, "cm_dither", obs_module_text("Gradient.Dither"));
	spectrum_reactive_properties(props, fx ? fx->context : NULL);
	return props;
}

struct obs_source_info spectrum_composite_filter = {
	.id = "spectrum_composite_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = composite_name,
	.create = composite_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = fx_cb_render,
	.get_properties = composite_properties,
	.get_defaults = composite_defaults,
};
