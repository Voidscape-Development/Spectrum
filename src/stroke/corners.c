#include "corners.h"
#include "obs-utils.h"
#include "stroke-types.h"
#include <graphics/vec4.h>
#include <math.h>

#define SWAP(x, y)                         \
	do {                               \
		gs_texrender_t *tmp_ = (x); \
		(x) = (y);                 \
		(y) = tmp_;                \
	} while (0)

static void set_float(gs_effect_t *e, const char *name, float v)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	if (p)
		gs_effect_set_float(p, v);
}

static void set_vec2(gs_effect_t *e, const char *name, float x, float y)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	struct vec2 v;
	vec2_set(&v, x, y);
	if (p)
		gs_effect_set_vec2(p, &v);
}

static void set_tex(gs_effect_t *e, const char *name, gs_texrender_t *t)
{
	gs_eparam_t *p = gs_effect_get_param_by_name(e, name);
	gs_texture_t *tex = t ? gs_texrender_get_texture(t) : NULL;
	if (p && tex)
		gs_effect_set_texture(p, tex);
}

// Uniforms every outline pass uses (they are cleared after each technique).
static void bind_input(gs_effect_t *e, gs_texture_t *input, uint32_t w, uint32_t h, float threshold)
{
	gs_effect_set_texture(gs_effect_get_param_by_name(e, "image"), input);
	set_vec2(e, "uv_size", (float)w, (float)h);
	set_vec2(e, "arc_size", (float)(w + 1), (float)(h + 1));
	set_float(e, "threshold", threshold);
}

static void draw_into(gs_texrender_t **target, enum gs_color_format format, gs_effect_t *e, const char *technique,
		      uint32_t w, uint32_t h)
{
	*target = create_or_reset_texrender_format(*target, format);
	if (gs_texrender_begin(*target, w, h)) {
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		while (gs_effect_loop(e, technique))
			gs_draw_sprite(NULL, 0, w, h);
		gs_texrender_end(*target);
	}
}

// A pass over arc sized state: reads *a as arc_state, writes *b, then swaps.
static void state_pass(gs_effect_t *e, const char *technique, gs_texture_t *input, uint32_t w, uint32_t h,
		       float threshold, gs_texrender_t **a, gs_texrender_t **b)
{
	bind_input(e, input, w, h, threshold);
	set_tex(e, "arc_state", *a);
	draw_into(b, GS_RGBA32F, e, technique, w + 1, h + 1);
	SWAP(*a, *b);
}

// Pointer jumps for outlines up to four times the source's perimeter.
static int jump_steps(uint32_t w, uint32_t h)
{
	int steps = 1;
	while (steps < 30 && (1ull << steps) < 4ull * (w + h + 2))
		steps++;
	return steps;
}

gs_texrender_t *jf_field(gs_effect_t *e, gs_texture_t *input, uint32_t w, uint32_t h, float threshold, float extent,
			 gs_texrender_t **a, gs_texrender_t **b)
{
	stroke_set_blending_parameters();
	bind_input(e, input, w, h, threshold);
	draw_into(a, GS_RG32F, e, "DrawThreshold", w, h);
	int pass = 1;
	while (pass < (int)extent + 1)
		pass *= 2;
	for (; pass > 0; pass /= 2) {
		bind_input(e, gs_texrender_get_texture(*a), w, h, threshold);
		set_float(e, "offset", (float)pass);
		draw_into(b, GS_RG32F, e, "DrawJumpFloodStep", w, h);
		SWAP(*a, *b);
	}
	gs_blend_state_pop();
	return *a;
}

gs_texrender_t *jf_arc(gs_effect_t *e, gs_texture_t *input, uint32_t w, uint32_t h, float threshold, gs_texrender_t **a,
		       gs_texrender_t **b)
{
	const int steps = jump_steps(w, h);
	stroke_set_blending_parameters();
	state_pass(e, "DrawArcInit", input, w, h, threshold, a, b);
	for (int i = 0; i < steps; i++)
		state_pass(e, "DrawArcMin", input, w, h, threshold, a, b);
	state_pass(e, "DrawRankInit", input, w, h, threshold, a, b);
	for (int i = 0; i < steps; i++)
		state_pass(e, "DrawRankStep", input, w, h, threshold, a, b);
	state_pass(e, "DrawArcFinal", input, w, h, threshold, a, b);
	gs_blend_state_pop();
	return *a;
}

static void set_detect(struct corners *c, gs_effect_t *e)
{
	set_float(e, "corner_angle", RAD(c->angle));
	set_float(e, "corner_radius", c->radius);
	set_tex(e, "corner_mark", c->mark);
}

void corners_render(struct corners *c, gs_effect_t *e, gs_texture_t *input, gs_texture_t *arc, uint32_t w, uint32_t h,
		    float threshold)
{
	if (!c->enabled || !e || !input || !w || !h)
		return;
	stroke_set_blending_parameters();

	// Content box (contour mode: its centre splits the corners into
	// quadrants), reduced 8x8 per pass down to 1x1.
	if (c->mode == CORNER_MODE_CONTOUR || c->rect == CORNER_RECT_CONTENT) {
		uint32_t sw = w, sh = h;
		bool init = true;
		do {
			const uint32_t ow = (sw + 7) / 8, oh = (sh + 7) / 8;
			bind_input(e, input, w, h, threshold);
			set_float(e, "bounds_init", init ? 1.0f : 0.0f);
			set_vec2(e, "bounds_src", (float)sw, (float)sh);
			if (!init)
				set_tex(e, "arc_state", c->a);
			draw_into(&c->b, GS_RGBA32F, e, "DrawBoundsReduce", ow, oh);
			SWAP(c->a, c->b);
			sw = ow;
			sh = oh;
			init = false;
		} while (sw > 1 || sh > 1);
		SWAP(c->bounds, c->a);
	}

	if (c->mode == CORNER_MODE_CONTOUR && arc) {
		const int steps = jump_steps(w, h);
		set_detect(c, e);
		state_pass(e, "DrawCornerTurn", input, w, h, threshold, &c->a, &c->b);
		set_detect(c, e);
		state_pass(e, "DrawCornerMark", input, w, h, threshold, &c->a, &c->b);
		SWAP(c->a, c->mark);
		// Next corner ahead, then the last one behind; then both.
		for (int ahead = 1; ahead >= 0; ahead--) {
			set_detect(c, e);
			set_float(e, "link_ahead", (float)ahead);
			state_pass(e, "DrawLinkInit", input, w, h, threshold, &c->a, &c->b);
			for (int i = 0; i < steps; i++)
				state_pass(e, "DrawLinkStep", input, w, h, threshold, &c->a, &c->b);
			if (ahead)
				SWAP(c->a, c->next);
		}
		set_tex(e, "corner_next", c->next);
		state_pass(e, "DrawLinkFinal", input, w, h, threshold, &c->a, &c->b);
		SWAP(c->a, c->link);
	}
	gs_blend_state_pop();
}

void corners_bind(struct corners *c, gs_effect_t *e, gs_texture_t *arc, uint32_t w, uint32_t h,
		  const struct vec4 *frame)
{
	set_float(e, "corner_enabled", c->enabled ? 1.0f : 0.0f);
	if (!c->enabled)
		return;
	set_float(e, "corner_mode", (float)c->mode);
	gs_eparam_t *p = gs_effect_get_param_by_name(e, "corner_frame");
	if (p)
		gs_effect_set_vec4(p, frame);
	const bool content = c->mode == CORNER_MODE_CONTOUR || c->rect == CORNER_RECT_CONTENT;
	set_float(e, "corner_content", content && c->bounds ? 1.0f : 0.0f);
	set_tex(e, "corner_bounds", c->bounds);

	// Pulse: the arms shrink by up to `depth` and grow back.
	const double pulse = 1.0 - c->pulse_depth * (0.5 - 0.5 * cos(2.0 * M_PI * c->pulse_speed * c->time));
	set_vec2(e, "corner_arm", (float)(c->arm_h * pulse), (float)(c->arm_v * pulse));
	set_float(e, "corner_pct", c->pct ? 1.0f : 0.0f);
	set_float(e, "corner_sharp", c->sharp ? 1.0f : 0.0f);
	set_float(e, "corner_cap", (float)c->cap);
	p = gs_effect_get_param_by_name(e, "corner_on");
	if (p) {
		struct vec4 on;
		vec4_set(&on, c->on[0], c->on[1], c->on[2], c->on[3]);
		gs_effect_set_vec4(p, &on);
	}
	// Slide: whole segments travelled (mod 4, for the rectangle's corner
	// switches) and the fraction of the current one.
	const double travel = c->slide_speed * c->time;
	const double cycle = floor(travel);
	set_float(e, "corner_phase", (float)(travel - cycle));
	set_float(e, "corner_cycle", (float)(cycle - 4.0 * floor(cycle / 4.0)));
	set_detect(c, e);
	set_tex(e, "corner_link", c->link);
	p = gs_effect_get_param_by_name(e, "arc_field");
	if (p && arc)
		gs_effect_set_texture(p, arc);
	set_vec2(e, "arc_size", (float)(w + 1), (float)(h + 1));
}

gs_texture_t *corners_render_mask(struct corners *c, gs_texture_t *input, uint32_t w, uint32_t h, float threshold,
				  const struct vec4 *frame)
{
	if (!c->enabled || !input)
		return NULL;
	if (!c->effect)
		c->effect = stroke_load_shader_effect(NULL, "/shaders/stroke/corners.effect");
	gs_effect_t *e = c->effect;
	if (!e)
		return NULL;

	gs_texrender_t *field = NULL;
	gs_texture_t *arc = NULL;
	if (c->mode == CORNER_MODE_CONTOUR) {
		field = jf_field(e, input, w, h, threshold, (float)(w > h ? w : h), &c->jf_a, &c->jf_b);
		arc = gs_texrender_get_texture(jf_arc(e, input, w, h, threshold, &c->arc_a, &c->arc_b));
	}
	corners_render(c, e, input, arc, w, h, threshold);

	stroke_set_blending_parameters();
	bind_input(e, input, w, h, threshold);
	set_tex(e, "distance_field", field);
	corners_bind(c, e, arc, w, h, frame);
	draw_into(&c->mask, GS_R16F, e, "DrawCornerMask", w, h);
	gs_blend_state_pop();
	return gs_texrender_get_texture(c->mask);
}

void corners_free(struct corners *c)
{
	gs_texrender_t **all[] = {&c->bounds, &c->mark, &c->next,  &c->link,  &c->a,   &c->b,
				  &c->jf_a,   &c->jf_b, &c->arc_a, &c->arc_b, &c->mask};
	for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
		gs_texrender_destroy(*all[i]);
		*all[i] = NULL;
	}
	gs_effect_destroy(c->effect);
	c->effect = NULL;
}

bool corners_tick(struct corners *c, float seconds)
{
	if (!c->enabled || (c->pulse_speed == 0.0f && c->slide_speed == 0.0f))
		return false;
	c->time += seconds;
	return true;
}

void corners_update(struct corners *c, obs_data_t *s, bool on)
{
	c->enabled = on;
	c->mode = (enum corner_mode)obs_data_get_int(s, "corner_mode");
	c->rect = (enum corner_rect)obs_data_get_int(s, "corner_rect");
	c->cap = (enum corner_cap)obs_data_get_int(s, "corner_cap");
	c->glow = (enum corner_glow)obs_data_get_int(s, "corner_glow");
	c->pct = obs_data_get_int(s, "corner_unit") == CORNER_UNIT_PCT;
	c->arm_h = c->pct ? (float)obs_data_get_double(s, "corner_arm_h_pct") / 100.0f
			  : (float)obs_data_get_double(s, "corner_arm_h_px");
	c->arm_v = c->pct ? (float)obs_data_get_double(s, "corner_arm_v_pct") / 100.0f
			  : (float)obs_data_get_double(s, "corner_arm_v_px");
	c->sharp = obs_data_get_int(s, "corner_bend") == 0;
	c->angle = (float)obs_data_get_double(s, "corner_angle");
	c->radius = (float)obs_data_get_double(s, "corner_radius");
	c->on[0] = obs_data_get_bool(s, "corner_tl");
	c->on[1] = obs_data_get_bool(s, "corner_tr");
	c->on[2] = obs_data_get_bool(s, "corner_br");
	c->on[3] = obs_data_get_bool(s, "corner_bl");
	c->pulse_speed = (float)obs_data_get_double(s, "corner_pulse_speed");
	c->pulse_depth = (float)obs_data_get_double(s, "corner_pulse_depth") / 100.0f;
	c->slide_speed = (float)obs_data_get_double(s, "corner_slide_speed");
}

void corners_defaults(obs_data_t *s)
{
	obs_data_set_default_int(s, "corner_glow", CORNER_GLOW_OFF);
	obs_data_set_default_int(s, "corner_mode", CORNER_MODE_BOUNDS);
	obs_data_set_default_int(s, "corner_rect", CORNER_RECT_FRAME);
	obs_data_set_default_int(s, "corner_unit", CORNER_UNIT_PCT);
	obs_data_set_default_double(s, "corner_arm_h_px", 40.0);
	obs_data_set_default_double(s, "corner_arm_v_px", 40.0);
	obs_data_set_default_double(s, "corner_arm_h_pct", 15.0);
	obs_data_set_default_double(s, "corner_arm_v_pct", 15.0);
	obs_data_set_default_int(s, "corner_bend", 0);
	obs_data_set_default_int(s, "corner_cap", CORNER_CAP_SQUARE);
	obs_data_set_default_double(s, "corner_angle", 45.0);
	obs_data_set_default_double(s, "corner_radius", 6.0);
	obs_data_set_default_bool(s, "corner_tl", true);
	obs_data_set_default_bool(s, "corner_tr", true);
	obs_data_set_default_bool(s, "corner_br", true);
	obs_data_set_default_bool(s, "corner_bl", true);
	obs_data_set_default_double(s, "corner_pulse_speed", 0.0);
	obs_data_set_default_double(s, "corner_pulse_depth", 50.0);
	obs_data_set_default_double(s, "corner_slide_speed", 0.0);
}

static const char *const corner_props[] = {
	"corner_mode",        "corner_rect",        "corner_unit",        "corner_arm_h_px", "corner_arm_v_px",
	"corner_arm_h_pct",   "corner_arm_v_pct",   "corner_tl",          "corner_tr",       "corner_bl",
	"corner_br",          "corner_bend",        "corner_cap",         "corner_angle",    "corner_radius",
	"corner_pulse_speed", "corner_pulse_depth", "corner_slide_speed",
};

void corners_refresh(obs_properties_t *props, obs_data_t *settings, bool on)
{
	for (size_t i = 0; i < sizeof(corner_props) / sizeof(corner_props[0]); i++)
		stroke_setting_visibility(corner_props[i], on, props);
	if (!on)
		return;
	const bool contour = obs_data_get_int(settings, "corner_mode") == CORNER_MODE_CONTOUR;
	const bool pct = obs_data_get_int(settings, "corner_unit") == CORNER_UNIT_PCT;
	stroke_setting_visibility("corner_rect", !contour, props);
	stroke_setting_visibility("corner_angle", contour, props);
	stroke_setting_visibility("corner_radius", contour, props);
	stroke_setting_visibility("corner_arm_h_px", !pct, props);
	stroke_setting_visibility("corner_arm_v_px", !pct, props);
	stroke_setting_visibility("corner_arm_h_pct", pct, props);
	stroke_setting_visibility("corner_arm_v_pct", pct, props);
}

static bool corners_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const bool on = obs_properties_get(props, "corner_glow")
				? obs_data_get_int(settings, "corner_glow") != CORNER_GLOW_OFF
				: obs_data_get_int(settings, "stroke_dash_style") == STROKE_DASH_CORNERS;
	corners_refresh(props, settings, on);
	return true;
}

static obs_property_t *add_list(obs_properties_t *props, const char *name, const char *label, const char *const *items,
				int count)
{
	obs_property_t *p =
		obs_properties_add_list(props, name, obs_module_text(label), OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	for (int i = 0; i < count; i++)
		obs_property_list_add_int(p, obs_module_text(items[i]), i);
	return p;
}

static void add_slider(obs_properties_t *props, const char *name, const char *label, double min, double max,
		       double step, const char *suffix)
{
	obs_property_t *p = obs_properties_add_float_slider(props, name, obs_module_text(label), min, max, step);
	obs_property_float_set_suffix(p, suffix);
}

void corners_properties(obs_properties_t *props, bool glow)
{
	obs_property_t *p;
	if (glow) {
		static const char *const glow_items[] = {"Corners.Glow.Off", "Corners.Glow.Mask",
							 "Corners.Glow.Brackets"};
		p = add_list(props, "corner_glow", "Corners.Glow", glow_items, 3);
		obs_property_set_long_description(p, obs_module_text("Corners.Glow.Description"));
		obs_property_set_modified_callback(p, corners_modified);
	}
	static const char *const modes[] = {"Corners.Mode.Bounds", "Corners.Mode.Contour"};
	p = add_list(props, "corner_mode", "Corners.Mode", modes, 2);
	obs_property_set_long_description(p, obs_module_text("Corners.Mode.Description"));
	obs_property_set_modified_callback(p, corners_modified);
	static const char *const rects[] = {"Corners.Rect.Frame", "Corners.Rect.Content"};
	add_list(props, "corner_rect", "Corners.Rect", rects, 2);
	add_slider(props, "corner_angle", "Corners.Angle", 5.0, 175.0, 1.0, "°");
	p = obs_properties_get(props, "corner_angle");
	obs_property_set_long_description(p, obs_module_text("Corners.Angle.Description"));
	add_slider(props, "corner_radius", "Corners.Radius", 1.0, 48.0, 0.5, "px");
	p = obs_properties_get(props, "corner_radius");
	obs_property_set_long_description(p, obs_module_text("Corners.Radius.Description"));

	static const char *const units[] = {"Corners.Unit.Px", "Corners.Unit.Pct"};
	p = add_list(props, "corner_unit", "Corners.Unit", units, 2);
	obs_property_set_modified_callback(p, corners_modified);
	add_slider(props, "corner_arm_h_px", "Corners.ArmH", 0.0, 2000.0, 1.0, "px");
	add_slider(props, "corner_arm_v_px", "Corners.ArmV", 0.0, 2000.0, 1.0, "px");
	add_slider(props, "corner_arm_h_pct", "Corners.ArmH", 0.0, 50.0, 0.1, "%");
	add_slider(props, "corner_arm_v_pct", "Corners.ArmV", 0.0, 50.0, 0.1, "%");
	p = obs_properties_get(props, "corner_arm_h_pct");
	obs_property_set_long_description(p, obs_module_text("Corners.Arm.Description"));
	p = obs_properties_get(props, "corner_arm_v_pct");
	obs_property_set_long_description(p, obs_module_text("Corners.Arm.Description"));

	static const char *const bends[] = {"Corners.Bend.Sharp", "Corners.Bend.Round"};
	static const char *const caps[] = {"Corners.Cap.Square", "Corners.Cap.Round", "Corners.Cap.Fade"};
	if (!glow)
		add_list(props, "corner_bend", "Corners.Bend", bends, 2);
	p = add_list(props, "corner_cap", "Corners.Cap", caps, glow ? 1 : 3);
	// Glows have no width to round: square or fade.
	if (glow)
		obs_property_list_add_int(p, obs_module_text(caps[2]), CORNER_CAP_FADE);

	obs_properties_add_bool(props, "corner_tl", obs_module_text("Corners.TL"));
	obs_properties_add_bool(props, "corner_tr", obs_module_text("Corners.TR"));
	obs_properties_add_bool(props, "corner_bl", obs_module_text("Corners.BL"));
	obs_properties_add_bool(props, "corner_br", obs_module_text("Corners.BR"));
	p = obs_properties_get(props, "corner_tl");
	obs_property_set_long_description(p, obs_module_text("Corners.Toggle.Description"));

	add_slider(props, "corner_pulse_speed", "Corners.PulseSpeed", 0.0, 10.0, 0.01, "Hz");
	add_slider(props, "corner_pulse_depth", "Corners.PulseDepth", 0.0, 100.0, 1.0, "%");
	add_slider(props, "corner_slide_speed", "Corners.SlideSpeed", -10.0, 10.0, 0.01, "/s");
	p = obs_properties_get(props, "corner_slide_speed");
	obs_property_set_long_description(p, obs_module_text("Corners.SlideSpeed.Description"));
}
