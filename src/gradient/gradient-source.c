#include "gradient.h"

#include <obs-module.h>
#include "../common/presets.h"

#define SOURCE_PREFIX "gradient_"

struct gradient_source {
	obs_source_t *context;
	uint32_t width;
	uint32_t height;
	gradient_t gradient;
};

static const char *gradient_source_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("GradientSource");
}

static void gradient_source_update(void *data, obs_data_t *settings)
{
	struct gradient_source *s = data;
	s->width = (uint32_t)obs_data_get_int(settings, "width");
	s->height = (uint32_t)obs_data_get_int(settings, "height");
	gradient_update(&s->gradient, settings);
}

static void *gradient_source_create(obs_data_t *settings, obs_source_t *source)
{
	struct gradient_source *s = bzalloc(sizeof(struct gradient_source));
	s->context = source;
	gradient_init(&s->gradient, source, SOURCE_PREFIX);
	gradient_source_update(s, settings);
	return s;
}

static void gradient_source_destroy(void *data)
{
	struct gradient_source *s = data;
	gradient_free(&s->gradient);
	bfree(s);
}

static void gradient_source_defaults(obs_data_t *settings)
{
	struct obs_video_info ovi;
	if (obs_get_video_info(&ovi)) {
		obs_data_set_default_int(settings, "width", ovi.base_width);
		obs_data_set_default_int(settings, "height", ovi.base_height);
	} else {
		obs_data_set_default_int(settings, "width", 1920);
		obs_data_set_default_int(settings, "height", 1080);
	}
	gradient_defaults(settings, SOURCE_PREFIX);
}

static obs_properties_t *gradient_source_properties(void *data)
{
	struct gradient_source *s = data;
	obs_properties_t *props = obs_properties_create();

	obs_properties_add_int(props, "width", obs_module_text("GradientSource.Width"), 1, 16384, 1);
	obs_properties_add_int(props, "height", obs_module_text("GradientSource.Height"), 1, 16384, 1);

	gradient_properties(props, SOURCE_PREFIX, s ? &s->gradient : NULL);
	spectrum_presets_properties(props, data ? ((struct gradient_source *)data)->context : NULL);
	return props;
}

static void gradient_source_tick(void *data, float seconds)
{
	struct gradient_source *s = data;
	gradient_tick(&s->gradient, seconds);
}

static void gradient_source_render(void *data, gs_effect_t *effect)
{
	UNUSED_PARAMETER(effect);
	struct gradient_source *s = data;
	gradient_draw(&s->gradient, s->width, s->height);
}

static uint32_t gradient_source_width(void *data)
{
	struct gradient_source *s = data;
	return s->width;
}

static uint32_t gradient_source_height(void *data)
{
	struct gradient_source *s = data;
	return s->height;
}

// Prefer a float render target when OBS offers one (HDR or 16-bit float
// canvases): it cannot band, and the shader then skips dithering. Otherwise
// render 8-bit sRGB, where the shader dithers.
static enum gs_color_space gradient_source_color_space(void *data, size_t count,
						       const enum gs_color_space *preferred_spaces)
{
	UNUSED_PARAMETER(data);
	for (size_t i = 0; i < count; i++) {
		if (preferred_spaces[i] == GS_CS_SRGB_16F || preferred_spaces[i] == GS_CS_709_EXTENDED)
			return GS_CS_SRGB_16F;
	}
	return GS_CS_SRGB;
}

struct obs_source_info spectrum_gradient_source = {
	.id = "spectrum_gradient_source",
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.get_name = gradient_source_name,
	.create = gradient_source_create,
	.destroy = gradient_source_destroy,
	.update = gradient_source_update,
	.get_defaults = gradient_source_defaults,
	.get_properties = gradient_source_properties,
	.video_tick = gradient_source_tick,
	.video_render = gradient_source_render,
	.get_width = gradient_source_width,
	.get_height = gradient_source_height,
	.video_get_color_space = gradient_source_color_space,
	.icon_type = OBS_ICON_TYPE_COLOR,
};
