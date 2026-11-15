// Spectrum Retro: oil paint (anisotropic Kuwahara), ASCII art, retro
// palettes with ordered dithering, and VHS tape, each off by default.
//
// Every effect is its own shader technique. Only the enabled ones run, in a
// chain of passes (oil paint, ASCII, palette, VHS) through float textures
// that hold linear light; the last enabled pass draws to the output. With
// everything off the filter is skipped entirely.

#include "../common/fx-filter.h"
#include <stdio.h>

#include <graphics/vec4.h>
#include <math.h>
#include "../common/presets.h"

enum retro_palette {
	PAL_OFF = 0,
	PAL_GAMEBOY = 1,
	PAL_CGA = 2,
	PAL_CGA16 = 3,
	PAL_PICO8 = 4,
	PAL_1BIT = 5,
	PAL_CUSTOM = 6,
};

enum retro_match {
	MATCH_AUTO = 0,
	MATCH_COLOR = 1,
	MATCH_BRIGHTNESS = 2,
};

#define PAL_SIZE 16

static const uint32_t pal_gameboy[] = {0x0F380F, 0x306230, 0x8BAC0F, 0x9BBC0F};
static const uint32_t pal_cga[] = {0x000000, 0x55FFFF, 0xFF55FF, 0xFFFFFF};
static const uint32_t pal_cga16[] = {0x000000, 0x0000AA, 0x00AA00, 0x00AAAA, 0xAA0000, 0xAA00AA, 0xAA5500, 0xAAAAAA,
				     0x555555, 0x5555FF, 0x55FF55, 0x55FFFF, 0xFF5555, 0xFF55FF, 0xFFFF55, 0xFFFFFF};
static const uint32_t pal_pico8[] = {0x000000, 0x1D2B53, 0x7E2553, 0x008751, 0xAB5236, 0x5F574F, 0xC2C3C7, 0xFFF1E8,
				     0xFF004D, 0xFFA300, 0xFFEC27, 0x00E436, 0x29ADFF, 0x83769C, 0xFF77A8, 0xFFCCAA};
static const uint32_t pal_1bit[] = {0x000000, 0xFFFFFF};

struct retro_data {
	gs_texrender_t *input;
	gs_texrender_t *ping[2];
	gs_texrender_t *tensor[2];
};

static void retro_create_hook(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	fx->user = bzalloc(sizeof(struct retro_data));
}

static void retro_destroy_hook(fx_filter_t *fx)
{
	struct retro_data *d = fx->user;
	obs_enter_graphics();
	gs_texrender_destroy(d->input);
	for (int i = 0; i < 2; i++) {
		gs_texrender_destroy(d->ping[i]);
		gs_texrender_destroy(d->tensor[i]);
	}
	obs_leave_graphics();
	bfree(d);
}

static const struct fx_filter_def retro_def = {
	.effect_file = "shaders/fx/retro.effect",
	.dither_key = "rt_dither",
	.create = retro_create_hook,
	.destroy = retro_destroy_hook,
};

/* ------------------------------------------------------------------------
 * Palettes (Oklab, for the nearest match in the shader)
 * --------------------------------------------------------------------- */

static float srgb_to_linear_f(float u)
{
	return u <= 0.04045f ? u / 12.92f : powf((u + 0.055f) / 1.055f, 2.4f);
}

static void rgb_to_oklab(float r, float g, float b, float *out)
{
	r = srgb_to_linear_f(r);
	g = srgb_to_linear_f(g);
	b = srgb_to_linear_f(b);
	const float l = cbrtf(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
	const float m = cbrtf(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
	const float s = cbrtf(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);
	out[0] = 0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s;
	out[1] = 1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s;
	out[2] = 0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s;
}

static void set_palette(gs_effect_t *effect, obs_data_t *settings)
{
	const int which = (int)obs_data_get_int(settings, "rt_palette");
	uint32_t custom[4];
	const uint32_t *colors = NULL;
	size_t count = 0;
	switch (which) {
	case PAL_GAMEBOY:
		colors = pal_gameboy;
		count = OBS_COUNTOF(pal_gameboy);
		break;
	case PAL_CGA:
		colors = pal_cga;
		count = OBS_COUNTOF(pal_cga);
		break;
	case PAL_CGA16:
		colors = pal_cga16;
		count = OBS_COUNTOF(pal_cga16);
		break;
	case PAL_PICO8:
		colors = pal_pico8;
		count = OBS_COUNTOF(pal_pico8);
		break;
	case PAL_1BIT:
		colors = pal_1bit;
		count = OBS_COUNTOF(pal_1bit);
		break;
	default: {
		// OBS colors are 0xAABBGGRR.
		long long n = obs_data_get_int(settings, "rt_pal_count");
		count = n < 2 ? 2 : (n > 4 ? 4 : (size_t)n);
		for (size_t i = 0; i < count; i++) {
			char key[32];
			snprintf(key, sizeof(key), "rt_pal_color%d", (int)i + 1);
			const uint32_t c = (uint32_t)obs_data_get_int(settings, key);
			custom[i] = ((c & 0xFF) << 16) | (c & 0xFF00) | ((c >> 16) & 0xFF);
		}
		colors = custom;
		break;
	}
	}

	float lmin = 1.0f, lmax = 0.0f;
	for (size_t i = 0; i < PAL_SIZE; i++) {
		char name[16];
		snprintf(name, sizeof(name), "pal%d", (int)i);
		float lab[3] = {0.0f, 0.0f, 0.0f};
		if (i < count) {
			const uint32_t c = colors[i];
			rgb_to_oklab((float)((c >> 16) & 0xFF) / 255.0f, (float)((c >> 8) & 0xFF) / 255.0f,
				     (float)(c & 0xFF) / 255.0f, lab);
			lmin = fminf(lmin, lab[0]);
			lmax = fmaxf(lmax, lab[0]);
		}
		fx_set_vec4(effect, name, lab[0], lab[1], lab[2], i < count ? 1.0f : 0.0f);
	}
	fx_set_vec2(effect, "pal_lrange", lmin, lmax);

	// Shade palettes match brightness over their own range; color palettes
	// match the nearest color.
	long long match = obs_data_get_int(settings, "rt_pal_match");
	if (match == MATCH_AUTO)
		match = (which == PAL_GAMEBOY || which == PAL_1BIT) ? MATCH_BRIGHTNESS : MATCH_COLOR;
	fx_set_float(effect, "pal_luma", match == MATCH_BRIGHTNESS ? 1.0f : 0.0f);
}

/* ------------------------------------------------------------------------
 * Rendering
 * --------------------------------------------------------------------- */

static gs_texrender_t *ensure_tr(gs_texrender_t **tr, enum gs_color_format format)
{
	if (!*tr)
		*tr = gs_texrender_create(format, GS_ZS_NONE);
	gs_texrender_reset(*tr);
	return *tr;
}

static void set_image(gs_effect_t *effect, gs_texture_t *tex, bool srgb)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(effect, "image");
	if (!p)
		return;
	// The captured input is 8-bit sRGB: sample it as linear light. The
	// float intermediates already hold linear light.
	if (srgb)
		gs_effect_set_texture_srgb(p, tex);
	else
		gs_effect_set_texture(p, tex);
}

// Draws one full-size pass of `tech` into `dst` (float, no sRGB encoding).
static gs_texture_t *run_pass(gs_effect_t *effect, const char *tech, gs_texrender_t *dst, uint32_t w, uint32_t h)
{
	if (gs_texrender_begin(dst, w, h)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(effect, tech))
			gs_draw_sprite(NULL, 0, w, h);
		gs_texrender_end(dst);
	}
	return gs_texrender_get_texture(dst);
}

// Sets every uniform for the next pass: OBS forgets all effect parameter
// values when a technique ends.
static void bind_pass(fx_filter_t *fx, obs_data_t *settings, uint32_t w, uint32_t h, gs_texture_t *image, bool srgb,
		      gs_texture_t *tensor, float dir_x, float dir_y)
{
	gs_effect_t *effect = fx->effect;
	fx_filter_set_common(fx, effect, settings, NULL, 0.0f, w, h, true, 0.0f);
	set_image(effect, image, srgb);
	set_palette(effect, settings);
	fx_set_texture(effect, "tensor_tex", tensor);
	fx_set_vec2(effect, "pass_dir", dir_x, dir_y);
	fx_set_vec2(effect, "pass_texel", 1.0f / (float)w, 1.0f / (float)h);
}

enum stage { STAGE_OIL, STAGE_ASCII, STAGE_PALETTE, STAGE_VHS };

static void retro_render(void *data, gs_effect_t *unused)
{
	UNUSED_PARAMETER(unused);
	fx_filter_t *fx = data;
	struct retro_data *d = fx->user;

	obs_source_t *target = obs_filter_get_target(fx->context);
	const uint32_t w = obs_source_get_base_width(target);
	const uint32_t h = obs_source_get_base_height(target);
	if (!w || !h || !fx_filter_ensure_effect(fx)) {
		obs_source_skip_video_filter(fx->context);
		return;
	}
	gs_effect_t *effect = fx->effect;

	// Move Transition: interpolate towards the counterpart's settings.
	obs_data_t *settings = fx_filter_settings(fx);
	obs_source_t *move_to = NULL;
	const float move_t = fx_move_target(fx->context, &move_to);
	if (move_to) {
		fx_filter_t *other = obs_obj_get_data(move_to);
		if (other && other->def == fx->def) {
			obs_data_t *to = fx_filter_settings(other);
			obs_data_t *mixed = fx_lerp_settings(settings, to, move_t);
			obs_data_release(to);
			obs_data_release(settings);
			settings = mixed;
		}
	}

	enum stage stages[4];
	int n = 0;
	if (obs_data_get_double(settings, "rt_oil_radius") >= 1.0)
		stages[n++] = STAGE_OIL;
	if (obs_data_get_double(settings, "rt_ascii") > 0.0)
		stages[n++] = STAGE_ASCII;
	if (obs_data_get_int(settings, "rt_palette") != PAL_OFF)
		stages[n++] = STAGE_PALETTE;
	if (obs_data_get_double(settings, "rt_vhs") > 0.0)
		stages[n++] = STAGE_VHS;
	if (!n) {
		obs_data_release(settings);
		obs_source_skip_video_filter(fx->context);
		return;
	}

	const enum gs_color_space preferred[] = {GS_CS_SRGB, GS_CS_SRGB_16F, GS_CS_709_EXTENDED};
	const enum gs_color_space space = obs_source_get_color_space(target, OBS_COUNTOF(preferred), preferred);
	if (space == GS_CS_709_EXTENDED) {
		// HDR is not supported by these effects.
		obs_data_release(settings);
		obs_source_skip_video_filter(fx->context);
		return;
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	// The filter input, 8-bit sRGB (sampled as linear light below).
	ensure_tr(&d->input, GS_RGBA);
	bool have_input = false;
	if (gs_texrender_begin_with_color_space(d->input, w, h, GS_CS_SRGB)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		if (obs_source_process_filter_begin(fx->context, GS_RGBA, OBS_ALLOW_DIRECT_RENDERING)) {
			obs_source_process_filter_end(fx->context, obs_get_base_effect(OBS_EFFECT_DEFAULT), w, h);
			have_input = true;
		}
		gs_texrender_end(d->input);
	}
	gs_texture_t *cur = have_input ? gs_texrender_get_texture(d->input) : NULL;
	if (!cur) {
		gs_blend_state_pop();
		obs_data_release(settings);
		obs_source_skip_video_filter(fx->context);
		return;
	}
	bool cur_srgb = true;
	int ping = 0;

	const bool previous_srgb = gs_framebuffer_srgb_enabled();
	for (int i = 0; i < n; i++) {
		const bool last = i == n - 1;
		const char *tech = "Vhs";
		gs_texture_t *tensor = cur;

		switch (stages[i]) {
		case STAGE_OIL:
			tech = "Oil";
			if (obs_data_get_double(settings, "rt_oil_anisotropy") > 0.0) {
				// Structure tensor, smoothed by a separable Gaussian.
				gs_enable_framebuffer_srgb(false);
				bind_pass(fx, settings, w, h, cur, cur_srgb, cur, 0.0f, 0.0f);
				tensor = run_pass(effect, "Tensor", ensure_tr(&d->tensor[0], GS_RGBA16F), w, h);
				bind_pass(fx, settings, w, h, cur, cur_srgb, tensor, 1.0f / (float)w, 0.0f);
				tensor = run_pass(effect, "TensorBlur", ensure_tr(&d->tensor[1], GS_RGBA16F), w, h);
				bind_pass(fx, settings, w, h, cur, cur_srgb, tensor, 0.0f, 1.0f / (float)h);
				tensor = run_pass(effect, "TensorBlur", ensure_tr(&d->tensor[0], GS_RGBA16F), w, h);
			}
			break;
		case STAGE_ASCII:
			tech = "Ascii";
			break;
		case STAGE_PALETTE:
			tech = "Palette";
			break;
		case STAGE_VHS:
			tech = "Vhs";
			break;
		}
		bind_pass(fx, settings, w, h, cur, cur_srgb, tensor, 0.0f, 0.0f);
		// Palette colors are exact: no output noise.
		if (!last || stages[i] == STAGE_PALETTE)
			fx_set_float(effect, "dither_step", 0.0f);

		if (!last) {
			gs_enable_framebuffer_srgb(false);
			cur = run_pass(effect, tech, ensure_tr(&d->ping[ping], GS_RGBA16F), w, h);
			cur_srgb = false;
			ping ^= 1;
			if (!cur)
				break;
			continue;
		}

		// The last pass draws onto the output: linear light through the
		// sRGB framebuffer, straight alpha.
		gs_enable_framebuffer_srgb(true);
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		while (gs_effect_loop(effect, tech))
			gs_draw_sprite(NULL, 0, w, h);
		gs_blend_state_pop();
	}
	gs_enable_framebuffer_srgb(previous_srgb);
	gs_blend_state_pop();

	obs_data_release(settings);
}

/* ------------------------------------------------------------------------
 * Source info
 * --------------------------------------------------------------------- */

static const char *retro_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("RetroFilter");
}

static void *retro_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&retro_def, settings, source);
}

static void retro_defaults(obs_data_t *settings)
{
	obs_data_set_default_double(settings, "rt_oil_radius", 0.0);
	obs_data_set_default_double(settings, "rt_oil_sharpness", 50.0);
	obs_data_set_default_double(settings, "rt_oil_anisotropy", 100.0);

	obs_data_set_default_double(settings, "rt_ascii", 0.0);
	obs_data_set_default_double(settings, "rt_ascii_size", 10.0);
	obs_data_set_default_int(settings, "rt_ascii_mode", 0);
	obs_data_set_default_int(settings, "rt_ascii_color", 0xFF40FF40);
	obs_data_set_default_int(settings, "rt_ascii_bg_color", 0xFF000000);

	obs_data_set_default_int(settings, "rt_palette", PAL_OFF);
	obs_data_set_default_int(settings, "rt_pal_dither", 2);
	obs_data_set_default_double(settings, "rt_pal_strength", 100.0);
	obs_data_set_default_double(settings, "rt_pal_pixel", 2.0);
	obs_data_set_default_int(settings, "rt_pal_match", MATCH_AUTO);
	obs_data_set_default_int(settings, "rt_pal_count", 4);
	obs_data_set_default_int(settings, "rt_pal_color1", 0xFF2A1A1A);
	obs_data_set_default_int(settings, "rt_pal_color2", 0xFF7A4A3A);
	obs_data_set_default_int(settings, "rt_pal_color3", 0xFF60B0E0);
	obs_data_set_default_int(settings, "rt_pal_color4", 0xFFD8F0FF);

	obs_data_set_default_double(settings, "rt_vhs", 0.0);
	obs_data_set_default_double(settings, "rt_vhs_tracking", 40.0);
	obs_data_set_default_double(settings, "rt_vhs_wobble", 1.5);
	obs_data_set_default_double(settings, "rt_vhs_bleed", 6.0);
	obs_data_set_default_double(settings, "rt_vhs_chroma_shift", 2.0);
	obs_data_set_default_double(settings, "rt_vhs_noise", 30.0);
	obs_data_set_default_double(settings, "rt_vhs_head", 60.0);

	obs_data_set_default_bool(settings, "rt_dither", true);
}

static obs_property_t *slider(obs_properties_t *props, const char *key, const char *text, double min, double max,
			      double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, key, obs_module_text(text), min, max, step);
	if (suffix)
		obs_property_float_set_suffix(p, suffix);
	return p;
}

static void group(obs_properties_t *props, const char *key, const char *text, obs_properties_t *content)
{
	obs_properties_add_group(props, key, obs_module_text(text), OBS_GROUP_NORMAL, content);
}

static bool palette_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const bool custom = obs_data_get_int(settings, "rt_palette") == PAL_CUSTOM;
	const long long count = obs_data_get_int(settings, "rt_pal_count");
	obs_property_set_visible(obs_properties_get(props, "rt_pal_count"), custom);
	for (int i = 1; i <= 4; i++) {
		char key[32];
		snprintf(key, sizeof(key), "rt_pal_color%d", i);
		obs_property_set_visible(obs_properties_get(props, key), custom && i <= count);
	}
	return true;
}

static bool ascii_mode_changed(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const long long mode = obs_data_get_int(settings, "rt_ascii_mode");
	obs_property_set_visible(obs_properties_get(props, "rt_ascii_color"), mode == 2);
	obs_property_set_visible(obs_properties_get(props, "rt_ascii_bg_color"), mode != 1);
	return true;
}

static obs_properties_t *retro_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_properties_t *g;
	obs_property_t *p;

	g = obs_properties_create();
	slider(g, "rt_oil_radius", "Retro.OilRadius", 0.0, 24.0, 0.1, "px");
	slider(g, "rt_oil_sharpness", "Retro.Sharpness", 0.0, 100.0, 0.1, "%");
	slider(g, "rt_oil_anisotropy", "Retro.Anisotropy", 0.0, 100.0, 0.1, "%");
	group(props, "rt_oil_group", "Retro.Oil", g);

	g = obs_properties_create();
	slider(g, "rt_ascii", "Fx.Amount", 0.0, 100.0, 0.1, "%");
	slider(g, "rt_ascii_size", "Retro.CellSize", 4.0, 64.0, 0.5, "px");
	p = obs_properties_add_list(g, "rt_ascii_mode", obs_module_text("Retro.ColorMode"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Retro.ColorMode.Original"), 0);
	obs_property_list_add_int(p, obs_module_text("Retro.ColorMode.Mono"), 1);
	obs_property_list_add_int(p, obs_module_text("Retro.ColorMode.Custom"), 2);
	obs_property_set_modified_callback(p, ascii_mode_changed);
	obs_properties_add_color_alpha(g, "rt_ascii_color", obs_module_text("Retro.TextColor"));
	obs_properties_add_color_alpha(g, "rt_ascii_bg_color", obs_module_text("Retro.BackgroundColor"));
	group(props, "rt_ascii_group", "Retro.Ascii", g);

	g = obs_properties_create();
	p = obs_properties_add_list(g, "rt_palette", obs_module_text("Retro.Palette"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Retro.Palette.Off"), PAL_OFF);
	obs_property_list_add_int(p, obs_module_text("Retro.Palette.GameBoy"), PAL_GAMEBOY);
	obs_property_list_add_int(p, obs_module_text("Retro.Palette.Cga"), PAL_CGA);
	obs_property_list_add_int(p, obs_module_text("Retro.Palette.Cga16"), PAL_CGA16);
	obs_property_list_add_int(p, obs_module_text("Retro.Palette.Pico8"), PAL_PICO8);
	obs_property_list_add_int(p, obs_module_text("Retro.Palette.OneBit"), PAL_1BIT);
	obs_property_list_add_int(p, obs_module_text("Retro.Palette.Custom"), PAL_CUSTOM);
	obs_property_set_modified_callback(p, palette_changed);
	p = obs_properties_add_list(g, "rt_pal_dither", obs_module_text("Retro.DitherPattern"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Retro.Dither.None"), 0);
	obs_property_list_add_int(p, obs_module_text("Retro.Dither.Bayer2"), 1);
	obs_property_list_add_int(p, obs_module_text("Retro.Dither.Bayer4"), 2);
	obs_property_list_add_int(p, obs_module_text("Retro.Dither.Bayer8"), 3);
	obs_property_list_add_int(p, obs_module_text("Retro.Dither.BlueNoise"), 4);
	p = obs_properties_add_list(g, "rt_pal_match", obs_module_text("Retro.Match"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Retro.Match.Auto"), MATCH_AUTO);
	obs_property_list_add_int(p, obs_module_text("Retro.Match.Color"), MATCH_COLOR);
	obs_property_list_add_int(p, obs_module_text("Retro.Match.Brightness"), MATCH_BRIGHTNESS);
	slider(g, "rt_pal_strength", "Retro.DitherStrength", 0.0, 100.0, 0.1, "%");
	slider(g, "rt_pal_pixel", "Retro.PixelSize", 1.0, 32.0, 1.0, "px");
	p = obs_properties_add_int_slider(g, "rt_pal_count", obs_module_text("Retro.ColorCount"), 2, 4, 1);
	obs_property_set_modified_callback(p, palette_changed);
	for (int i = 1; i <= 4; i++) {
		char key[32], text[32];
		snprintf(key, sizeof(key), "rt_pal_color%d", i);
		snprintf(text, sizeof(text), "Retro.PaletteColor%d", i);
		obs_properties_add_color(g, key, obs_module_text(text));
	}
	group(props, "rt_palette_group", "Retro.Dithering", g);

	g = obs_properties_create();
	slider(g, "rt_vhs", "Fx.Amount", 0.0, 100.0, 0.1, "%");
	slider(g, "rt_vhs_tracking", "Retro.Tracking", 0.0, 100.0, 0.1, "%");
	slider(g, "rt_vhs_wobble", "Retro.Wobble", 0.0, 10.0, 0.1, "px");
	slider(g, "rt_vhs_bleed", "Retro.ChromaBleed", 0.0, 40.0, 0.1, "px");
	slider(g, "rt_vhs_chroma_shift", "Retro.ChromaShift", 0.0, 20.0, 0.1, "px");
	slider(g, "rt_vhs_noise", "Retro.TapeNoise", 0.0, 100.0, 0.1, "%");
	slider(g, "rt_vhs_head", "Retro.HeadSwitching", 0.0, 100.0, 0.1, "%");
	group(props, "rt_vhs_group", "Retro.Vhs", g);

	obs_properties_add_bool(props, "rt_dither", obs_module_text("Gradient.Dither"));
	spectrum_presets_properties(props, data ? ((fx_filter_t *)data)->context : NULL);
	return props;
}

struct obs_source_info spectrum_retro_filter = {
	.id = "spectrum_retro_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = retro_name,
	.create = retro_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = retro_render,
	.get_properties = retro_properties,
	.get_defaults = retro_defaults,
};
