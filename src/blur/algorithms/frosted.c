#include "frosted.h"
#include "lens.h"

void set_frosted_blur_types(obs_properties_t *props)
{
	obs_property_t *p = obs_properties_get(props, "blur_type");
	obs_property_list_clear(p);
	obs_property_list_add_int(p, obs_module_text(TYPE_AREA_LABEL), TYPE_AREA);
}

void frosted_setup_callbacks(blur_filter_data_t *data)
{
	data->video_render = render_video_frosted;
	data->load_effect = load_effect_frosted;
	data->update = NULL;
}

void load_effect_frosted(blur_filter_data_t *filter)
{
	load_effect_with_params(&filter->effect, "/shaders/blur/frosted_glass.effect", NULL, 0, NULL, NULL);
}

struct frost_params {
	float radius;
	float grain;
	float amount;
	float angle;
	float tint_amount;
	struct vec4 tint_color;
};

static void frost_params_from(const blur_filter_data_t *d, struct frost_params *p)
{
	p->radius = d->radius;
	p->grain = d->frost_grain;
	p->amount = d->frost_amount;
	p->angle = d->frost_angle;
	p->tint_amount = d->frost_tint ? d->frost_tint_amount : 0.0f;
	p->tint_color = d->frost_tint_color;
}

// Current settings, interpolated during a Move Transition.
static void frost_resolve_params(blur_filter_data_t *data, struct frost_params *p)
{
	frost_params_from(data, p);
	float t, fade;
	blur_filter_data_t *to = blur_move_peer(data, &t, &fade);
	if (to) {
		struct frost_params q;
		frost_params_from(to, &q);
		p->radius = blur_lerpf(p->radius, q.radius, t);
		p->grain = blur_lerpf(p->grain, q.grain, t);
		p->amount = blur_lerpf(p->amount, q.amount, t);
		p->angle = blur_lerpf(p->angle, q.angle, t);
		// A tint that is only on at one end fades in/out in its own color.
		if (q.tint_amount <= 0.0f)
			q.tint_color = p->tint_color;
		else if (p->tint_amount <= 0.0f)
			p->tint_color = q.tint_color;
		p->tint_amount = blur_lerpf(p->tint_amount, q.tint_amount, t);
		p->tint_color.x = blur_lerpf(p->tint_color.x, q.tint_color.x, t);
		p->tint_color.y = blur_lerpf(p->tint_color.y, q.tint_color.y, t);
		p->tint_color.z = blur_lerpf(p->tint_color.z, q.tint_color.z, t);
	} else {
		p->radius *= fade;
		p->amount *= fade;
		p->tint_amount *= fade;
	}
}

void render_video_frosted(blur_filter_data_t *data)
{
	gs_effect_t *effect = data->effect;
	gs_texture_t *texture = gs_texrender_get_texture(data->input_texrender);
	if (!effect || !texture)
		return;

	struct frost_params p;
	frost_resolve_params(data, &p);
	if (p.radius < 0.5f && p.amount < 0.01f && p.tint_amount <= 0.0f) {
		blur_passthrough(data, texture);
		return;
	}

	texture = blend_composite(texture, data);
	const uint32_t w = data->width, h = data->height;

	// 1. Linear premultiplied float copy of the input.
	blur_fx_set_linear_common(data, effect, w, h);
	blur_fx_texture(effect, "image", texture);
	gs_texture_t *blurred = blur_fx_draw(effect, "Prefilter", &data->frost_linear, GS_RGBA16F, w, h);
	if (!blurred)
		return;

	// 2. Separable gaussian (horizontal into frost_blur, vertical back into
	//    frost_linear).
	if (p.radius >= 0.5f) {
		blur_fx_float(effect, "radius", p.radius);
		blur_fx_texture(effect, "image", blurred);
		blur_fx_vec2(effect, "blur_dir", 1.0f / (float)w, 0.0f);
		gs_texture_t *tmp = blur_fx_draw(effect, "Blur", &data->frost_blur, GS_RGBA16F, w, h);
		if (!tmp)
			return;
		// (OBS clears effect parameters after each draw.)
		blur_fx_float(effect, "radius", p.radius);
		blur_fx_texture(effect, "image", tmp);
		blur_fx_vec2(effect, "blur_dir", 0.0f, 1.0f / (float)h);
		blurred = blur_fx_draw(effect, "Blur", &data->frost_linear, GS_RGBA16F, w, h);
		if (!blurred)
			return;
	}

	// 3. Look the blurred image up through the glass.
	const float rads = p.angle * (float)M_PI / 180.0f;
	blur_fx_set_linear_common(data, effect, w, h);
	blur_fx_texture(effect, "image", blurred);
	blur_fx_float(effect, "frost_style", (float)data->frost_style);
	blur_fx_float(effect, "grain", fmaxf(p.grain, 0.5f));
	blur_fx_float(effect, "amount", p.amount);
	blur_fx_vec2(effect, "pattern_rot", cosf(rads), sinf(rads));
	blur_fx_vec4(effect, "tint_color", &p.tint_color);
	blur_fx_float(effect, "tint_amount", fminf(fmaxf(p.tint_amount / 100.0f, 0.0f), 1.0f));
	blur_fx_draw(effect, "Frost", &data->output_texrender, data->color_format, w, h);
}
