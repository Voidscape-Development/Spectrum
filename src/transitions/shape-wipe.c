// Spectrum Shape Wipe: an iris wipe through a distance field shape (circle,
// rectangle, diamond, star, heart or polygon) around a movable center.

#include "transition-common.h"

#include <math.h>

#define SHAPE_CIRCLE 1
#define SHAPE_RECT 2
#define SHAPE_DIAMOND 3
#define SHAPE_STAR 4
#define SHAPE_HEART 5
#define SHAPE_POLYGON 6

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* Unit shapes, matching data/shaders/transitions/shape-wipe.effect. */

static float clampf(float v, float lo, float hi)
{
	return v < lo ? lo : (v > hi ? hi : v);
}

static float signf(float v)
{
	return v > 0.0f ? 1.0f : (v < 0.0f ? -1.0f : 0.0f);
}

static float sd_box(float x, float y)
{
	const float qx = fabsf(x) - 1.0f, qy = fabsf(y) - 1.0f;
	const float ox = fmaxf(qx, 0.0f), oy = fmaxf(qy, 0.0f);
	return fminf(fmaxf(qx, qy), 0.0f) + sqrtf(ox * ox + oy * oy);
}

// Folds p into the first sector of an n-fold shape (angle from the top).
static void fold(float *x, float *y, float an)
{
	const float a = atan2f(*x, -*y);
	const float bn = (a - 2.0f * an * floorf(a / (2.0f * an))) - an;
	const float len = sqrtf(*x * *x + *y * *y);
	*x = len * cosf(bn);
	*y = len * fabsf(sinf(bn));
}

static float sd_polygon(float x, float y, float n)
{
	const float an = (float)M_PI / n;
	const float ax = cosf(an), ay = sinf(an);
	fold(&x, &y, an);
	x -= ax;
	y -= ay;
	y += clampf(-y, 0.0f, ay);
	return sqrtf(x * x + y * y) * signf(x);
}

static float sd_star(float x, float y, float n, float m)
{
	const float an = (float)M_PI / n, en = (float)M_PI / m;
	const float ax = cosf(an), ay = sinf(an);
	const float ex = cosf(en), ey = sinf(en);
	fold(&x, &y, an);
	x -= ax;
	y -= ay;
	const float k = clampf(-(x * ex + y * ey), 0.0f, ay / ey);
	x += ex * k;
	y += ey * k;
	return sqrtf(x * x + y * y) * signf(x);
}

static float sd_heart(float x, float y)
{
	const float k = 2.0f / 1.2f;
	float px = fabsf(x / k), py = -y / k + 0.55f;
	float d;
	if (py + px > 1.0f) {
		const float dx = px - 0.25f, dy = py - 0.75f;
		d = sqrtf(dx * dx + dy * dy) - sqrtf(2.0f) / 4.0f;
	} else {
		const float ax = px, ay = py - 1.0f;
		const float h = 0.5f * fmaxf(px + py, 0.0f);
		const float bx = px - h, by = py - h;
		d = sqrtf(fminf(ax * ax + ay * ay, bx * bx + by * by)) * signf(px - py);
	}
	return d * k;
}

struct shape_params {
	int shape, points, sides;
	float inner;
};

static float shape_sd_unit(const struct shape_params *s, float x, float y)
{
	switch (s->shape) {
	case SHAPE_RECT:
		return sd_box(x, y);
	case SHAPE_DIAMOND:
		return sd_polygon(x, y, 4.0f);
	case SHAPE_STAR: {
		const float n = fmaxf((float)s->points, 3.0f);
		return sd_star(x, y, n, 2.0f + (n - 2.0f) * clampf(s->inner / 100.0f, 0.0f, 1.0f));
	}
	case SHAPE_HEART:
		return sd_heart(x, y);
	case SHAPE_POLYGON:
		return sd_polygon(x, y, fmaxf((float)s->sides, 3.0f));
	default:
		return sqrtf(x * x + y * y) - 1.0f;
	}
}

// The smallest shape radius (px) whose distance field is at most -margin at
// every corner of the frame, i.e. the size that fully covers it.
static float cover_radius(const struct shape_params *s, obs_data_t *settings, float w, float h, float margin)
{
	const float cx = (float)obs_data_get_double(settings, "sw_center_x") * 0.01f * w;
	const float cy = (float)obs_data_get_double(settings, "sw_center_y") * 0.01f * h;
	const bool open = obs_data_get_int(settings, "sw_direction") != 2;
	// Fully open (or not yet closing): the rotation at that end.
	const float deg = (float)obs_data_get_double(settings, "sw_rotation") +
			  (open ? (float)obs_data_get_double(settings, "sw_spin") : 0.0f);
	const float rot = deg * (float)M_PI / 180.0f;
	const float cs = cosf(rot), sn = sinf(rot);
	const bool stretch = obs_data_get_bool(settings, "sw_stretch");

	float px[4], py[4];
	const float corners[4][2] = {{0.0f, 0.0f}, {w, 0.0f}, {0.0f, h}, {w, h}};
	for (int i = 0; i < 4; i++) {
		const float x = corners[i][0] - cx, y = corners[i][1] - cy;
		px[i] = cs * x + sn * y;
		py[i] = -sn * x + cs * y;
		if (stretch)
			px[i] *= h / w;
	}

	const float diag = sqrtf(w * w + h * h);
	float lo = 0.0f, hi = diag * 64.0f;
	for (int iter = 0; iter < 48; iter++) {
		const float r = 0.5f * (lo + hi);
		bool covers = true;
		for (int i = 0; i < 4 && covers; i++)
			covers = r * shape_sd_unit(s, px[i] / r, py[i] / r) <= -margin;
		if (covers)
			hi = r;
		else
			lo = r;
	}
	return hi;
}

// Matches tr_margin() in transition-common.effect (aa = 1 px).
static float edge_margin(obs_data_t *settings, float soft)
{
	float m = 0.5f * fmaxf(soft, 1.0f);
	const float e = (float)obs_data_get_double(settings, "tr_edge_width");
	if (obs_data_get_int(settings, "tr_edge_mode") != 0 && obs_data_get_double(settings, "tr_edge_intensity") > 0.0)
		m = fmaxf(m, obs_data_get_int(settings, "tr_edge_style") == 2 ? 0.5f * e + 1.0f : 1.5f * e);
	return m;
}

static void shape_wipe_set_params(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, obs_data_t *to, float t,
				  uint32_t width, uint32_t height)
{
	UNUSED_PARAMETER(fx);
	UNUSED_PARAMETER(to);
	UNUSED_PARAMETER(t);
	const struct shape_params s = {
		.shape = (int)obs_data_get_int(settings, "sw_shape"),
		.points = (int)obs_data_get_int(settings, "sw_points"),
		.sides = (int)obs_data_get_int(settings, "sw_sides"),
		.inner = (float)obs_data_get_double(settings, "sw_inner"),
	};
	const float margin = edge_margin(settings, (float)obs_data_get_double(settings, "sw_softness"));
	fx_set_float(effect, "sw_cover", cover_radius(&s, settings, (float)width, (float)height, margin));
}

static const struct tr_def shape_wipe_def = {
	.fx =
		{
			.effect_file = "shaders/transitions/shape-wipe.effect",
			.gradient_prefix = TR_EDGE_PREFIX,
			.set_params = shape_wipe_set_params,
		},
};

static const char *shape_wipe_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("ShapeWipeTransition");
}

static void *shape_wipe_create(obs_data_t *settings, obs_source_t *source)
{
	return tr_create(&shape_wipe_def, settings, source);
}

static void shape_wipe_defaults(obs_data_t *settings)
{
	tr_defaults_common(settings);
	obs_data_set_default_int(settings, "sw_shape", SHAPE_CIRCLE);
	obs_data_set_default_int(settings, "sw_direction", 1);
	obs_data_set_default_double(settings, "sw_center_x", 50.0);
	obs_data_set_default_double(settings, "sw_center_y", 50.0);
	obs_data_set_default_double(settings, "sw_rotation", 0.0);
	obs_data_set_default_double(settings, "sw_spin", 0.0);
	obs_data_set_default_int(settings, "sw_points", 5);
	obs_data_set_default_double(settings, "sw_inner", 50.0);
	obs_data_set_default_int(settings, "sw_sides", 6);
	obs_data_set_default_bool(settings, "sw_stretch", false);
	obs_data_set_default_double(settings, "sw_softness", 2.0);
	tr_edge_defaults(settings, 0, 12.0, 100.0);
	obs_data_set_default_int(settings, "tr_edge_style", 2);
	obs_data_set_default_int(settings, "tr_edge_color", 0xFFFFFFFF);
}

static bool shape_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int shape = (int)obs_data_get_int(settings, "sw_shape");
	obs_property_set_visible(obs_properties_get(props, "sw_points"), shape == SHAPE_STAR);
	obs_property_set_visible(obs_properties_get(props, "sw_inner"), shape == SHAPE_STAR);
	obs_property_set_visible(obs_properties_get(props, "sw_sides"), shape == SHAPE_POLYGON);
	return true;
}

static obs_properties_t *shape_wipe_properties(void *data)
{
	fx_filter_t *fx = data;
	obs_properties_t *props = obs_properties_create();

	obs_property_t *p = obs_properties_add_list(props, "sw_shape", obs_module_text("ShapeWipe.Shape"),
						    OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("ShapeWipe.Shape.Circle"), SHAPE_CIRCLE);
	obs_property_list_add_int(p, obs_module_text("ShapeWipe.Shape.Rectangle"), SHAPE_RECT);
	obs_property_list_add_int(p, obs_module_text("ShapeWipe.Shape.Diamond"), SHAPE_DIAMOND);
	obs_property_list_add_int(p, obs_module_text("ShapeWipe.Shape.Star"), SHAPE_STAR);
	obs_property_list_add_int(p, obs_module_text("ShapeWipe.Shape.Heart"), SHAPE_HEART);
	obs_property_list_add_int(p, obs_module_text("ShapeWipe.Shape.Polygon"), SHAPE_POLYGON);
	obs_property_set_modified_callback(p, shape_modified);

	p = obs_properties_add_list(props, "sw_direction", obs_module_text("ShapeWipe.Direction"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("ShapeWipe.Direction.Open"), 1);
	obs_property_list_add_int(p, obs_module_text("ShapeWipe.Direction.Close"), 2);

	tr_add_easing_property(props);
	obs_properties_add_int_slider(props, "sw_points", obs_module_text("ShapeWipe.Points"), 3, 24, 1);
	tr_slider(props, "sw_inner", "ShapeWipe.Inner", 0.0, 100.0, 0.1, "%");
	obs_properties_add_int_slider(props, "sw_sides", obs_module_text("ShapeWipe.Sides"), 3, 24, 1);
	tr_slider(props, "sw_center_x", "Gradient.CenterX", 0.0, 100.0, 0.1, "%");
	tr_slider(props, "sw_center_y", "Gradient.CenterY", 0.0, 100.0, 0.1, "%");
	tr_slider(props, "sw_rotation", "ShapeWipe.Rotation", -360.0, 360.0, 0.1, "°");
	tr_slider(props, "sw_spin", "ShapeWipe.Spin", -1080.0, 1080.0, 0.1, "°");
	obs_properties_add_bool(props, "sw_stretch", obs_module_text("ShapeWipe.Stretch"));
	tr_slider(props, "sw_softness", "Transition.Softness", 0.0, 1000.0, 0.5, "px");

	tr_add_edge_properties(props, fx, "px", 500.0);
	tr_finish_properties(props, data);
	return props;
}

struct obs_source_info spectrum_shape_wipe_transition = {
	.id = "spectrum_shape_wipe_transition",
	.type = OBS_SOURCE_TYPE_TRANSITION,
	.get_name = shape_wipe_name,
	.create = shape_wipe_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = tr_video_render,
	.audio_render = tr_audio_render,
	.get_properties = shape_wipe_properties,
	.get_defaults = shape_wipe_defaults,
	.video_get_color_space = tr_video_get_color_space,
};
