// Spectrum Warp: corner pin, mesh warp, perspective tilt, bend, page curl
// and roll.
//
// Every mode is an inverse mapping evaluated per output pixel: the shader
// works out which source pixel lands there, samples it with a footprint
// (area) filter when the source is squeezed, and anti-aliases the source's
// edges from the mapping's derivatives. Corner pin and perspective tilt are
// exact homographies; the mesh warp is a smooth (Catmull-Rom) displacement
// of the control points, inverted on the CPU into a small lookup map.

#include "../common/fx-filter.h"
#include "../common/presets.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#define MODE_CORNER_PIN 1
#define MODE_MESH3 2
#define MODE_MESH4 3
#define MODE_TILT 4
#define MODE_BEND 5
#define MODE_PAGE_CURL 6
#define MODE_ROLL 7

#define MESH_MAX 4
// Nodes of the inverse mesh map along the longer side of the output.
#define MAP_NODES 96

static const char *corner_keys[4][2] = {
	{"wp_c0_x", "wp_c0_y"},
	{"wp_c1_x", "wp_c1_y"},
	{"wp_c2_x", "wp_c2_y"},
	{"wp_c3_x", "wp_c3_y"},
};

static void mesh_key(char *buf, size_t size, int row, int col, char axis)
{
	snprintf(buf, size, "wp_mesh_%d%d_%c", row, col, axis);
}

struct warp_data {
	gs_texture_t *map;
	uint32_t map_w, map_h;
	// What the map was built for.
	double map_key[2 * MESH_MAX * MESH_MAX + 6];
	bool map_valid;
};

static uint32_t warp_padding(obs_data_t *settings)
{
	const long long pad = obs_data_get_int(settings, "wp_padding");
	return pad > 0 ? (uint32_t)pad : 0;
}

static void warp_create_hook(fx_filter_t *fx, obs_data_t *settings)
{
	UNUSED_PARAMETER(settings);
	fx->user = bzalloc(sizeof(struct warp_data));
}

static void warp_destroy_hook(fx_filter_t *fx)
{
	struct warp_data *d = fx->user;
	obs_enter_graphics();
	gs_texture_destroy(d->map);
	obs_leave_graphics();
	bfree(d);
}

/* ------------------------------------------------------------------------
 * 3x3 matrices (row major, double)
 * --------------------------------------------------------------------- */

static void mat3_mul(const double a[9], const double b[9], double out[9])
{
	double r[9];
	for (int i = 0; i < 3; i++)
		for (int j = 0; j < 3; j++)
			r[i * 3 + j] = a[i * 3] * b[j] + a[i * 3 + 1] * b[3 + j] + a[i * 3 + 2] * b[6 + j];
	memcpy(out, r, sizeof(r));
}

static double mat3_det(const double m[9])
{
	return m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6]) +
	       m[2] * (m[3] * m[7] - m[4] * m[6]);
}

static bool mat3_inverse(const double m[9], double out[9])
{
	const double det = mat3_det(m);
	if (fabs(det) < 1e-12)
		return false;
	const double inv = 1.0 / det;
	out[0] = (m[4] * m[8] - m[5] * m[7]) * inv;
	out[1] = (m[2] * m[7] - m[1] * m[8]) * inv;
	out[2] = (m[1] * m[5] - m[2] * m[4]) * inv;
	out[3] = (m[5] * m[6] - m[3] * m[8]) * inv;
	out[4] = (m[0] * m[8] - m[2] * m[6]) * inv;
	out[5] = (m[2] * m[3] - m[0] * m[5]) * inv;
	out[6] = (m[3] * m[7] - m[4] * m[6]) * inv;
	out[7] = (m[1] * m[6] - m[0] * m[7]) * inv;
	out[8] = (m[0] * m[4] - m[1] * m[3]) * inv;
	return true;
}

// The projective map of the unit square onto a quad (Heckbert): corners
// (0,0), (1,0), (1,1), (0,1) go to q[0..3].
static void square_to_quad(const double q[4][2], double m[9])
{
	const double x0 = q[0][0], y0 = q[0][1], x1 = q[1][0], y1 = q[1][1];
	const double x2 = q[2][0], y2 = q[2][1], x3 = q[3][0], y3 = q[3][1];
	const double sx = x0 - x1 + x2 - x3, sy = y0 - y1 + y2 - y3;
	double g = 0.0, h = 0.0;
	if (fabs(sx) > 1e-9 || fabs(sy) > 1e-9) {
		const double dx1 = x1 - x2, dx2 = x3 - x2, dy1 = y1 - y2, dy2 = y3 - y2;
		const double den = dx1 * dy2 - dx2 * dy1;
		if (fabs(den) > 1e-12) {
			g = (sx * dy2 - dx2 * sy) / den;
			h = (dx1 * sy - sx * dy1) / den;
		}
	}
	m[0] = x1 - x0 + g * x1;
	m[1] = x3 - x0 + h * x3;
	m[2] = x0;
	m[3] = y1 - y0 + g * y1;
	m[4] = y3 - y0 + h * y3;
	m[5] = y0;
	m[6] = g;
	m[7] = h;
	m[8] = 1.0;
}

// Sets the inverse homography (output frame px -> homogeneous source px)
// from the forward one (source px -> output frame px). The forward map is
// scaled so that the source's centre has a positive w: output points with
// a positive w after the inverse are in front of the "camera".
static void set_homography(gs_effect_t *effect, double fwd[9], double sw, double sh)
{
	const double cw = fwd[6] * sw * 0.5 + fwd[7] * sh * 0.5 + fwd[8];
	if (cw < 0.0)
		for (int i = 0; i < 9; i++)
			fwd[i] = -fwd[i];
	double inv[9];
	if (!mat3_inverse(fwd, inv)) {
		fx_set_float(effect, "hom_valid", 0.0f);
		return;
	}
	fx_set_vec4(effect, "hom0", (float)inv[0], (float)inv[1], (float)inv[2], 0.0f);
	fx_set_vec4(effect, "hom1", (float)inv[3], (float)inv[4], (float)inv[5], 0.0f);
	fx_set_vec4(effect, "hom2", (float)inv[6], (float)inv[7], (float)inv[8], 0.0f);
	fx_set_float(effect, "hom_valid", 1.0f);
	// A mirrored mapping shows the back of the source.
	fx_set_float(effect, "backside", mat3_det(fwd) < 0.0 ? 1.0f : 0.0f);
}

static void corner_pin_params(gs_effect_t *effect, obs_data_t *s, obs_data_t *to, float t, double sw, double sh)
{
	const double rest[4][2] = {{0.0, 0.0}, {sw, 0.0}, {sw, sh}, {0.0, sh}};
	double q[4][2];
	for (int i = 0; i < 4; i++) {
		q[i][0] = rest[i][0] + fx_lerp_setting(s, to, t, corner_keys[i][0]);
		q[i][1] = rest[i][1] + fx_lerp_setting(s, to, t, corner_keys[i][1]);
	}
	double m[9];
	square_to_quad(q, m);
	const double scale[9] = {1.0 / sw, 0.0, 0.0, 0.0, 1.0 / sh, 0.0, 0.0, 0.0, 1.0};
	mat3_mul(m, scale, m);
	set_homography(effect, m, sw, sh);
}

static void tilt_params(gs_effect_t *effect, obs_data_t *s, obs_data_t *to, float t, double sw, double sh)
{
	const double deg = M_PI / 180.0;
	const double ax = fx_lerp_setting(s, to, t, "wp_tilt_x") * deg;
	const double ay = fx_lerp_setting(s, to, t, "wp_tilt_y") * deg;
	const double az = fx_lerp_setting(s, to, t, "wp_tilt_z") * deg;
	double fov = fx_lerp_setting(s, to, t, "wp_fov");
	fov = fmin(fmax(fov, 1.0), 170.0) * deg;
	const double zoom = fmax(fx_lerp_setting(s, to, t, "wp_tilt_zoom"), 1.0) / 100.0;

	// R = Rz * Ry * Rx (x: top edge tips away for positive angles).
	const double cx = cos(ax), sx = sin(ax), cy = cos(ay), sy = sin(ay), cz = cos(az), sz = sin(az);
	const double rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
	const double ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
	const double rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1};
	double r[9];
	mat3_mul(ry, rx, r);
	mat3_mul(rz, r, r);

	// Camera at distance d from the source plane; the focal length equals
	// d so an untilted source maps onto itself (times the zoom).
	const double half_diag = 0.5 * sqrt(sw * sw + sh * sh);
	const double d = half_diag / tan(fov * 0.5);
	const double f = d * zoom;
	const double ccx = sw * 0.5, ccy = sh * 0.5;
	// (qx, qy, 1) with q = s - centre -> homogeneous output.
	double m[9] = {
		f * r[0] + ccx * r[6],
		f * r[1] + ccx * r[7],
		ccx * d,
		f * r[3] + ccy * r[6],
		f * r[4] + ccy * r[7],
		ccy * d,
		r[6],
		r[7],
		d,
	};
	const double shift[9] = {1, 0, -ccx, 0, 1, -ccy, 0, 0, 1};
	mat3_mul(m, shift, m);
	set_homography(effect, m, sw, sh);
}

/* ------------------------------------------------------------------------
 * Mesh warp
 * --------------------------------------------------------------------- */

struct mesh {
	int n;
	double sw, sh;
	double off[MESH_MAX][MESH_MAX][2]; // [row][col] px
};

static double catmull_rom(double p0, double p1, double p2, double p3, double t)
{
	const double t2 = t * t, t3 = t2 * t;
	return 0.5 * ((2.0 * p1) + (-p0 + p2) * t + (2.0 * p0 - 5.0 * p1 + 4.0 * p2 - p3) * t2 +
		      (-p0 + 3.0 * p1 - 3.0 * p2 + p3) * t3);
}

// A control value with linear extrapolation one step past the ends.
static double mesh_ctrl(const struct mesh *m, int row, int col, int axis)
{
	const int n = m->n;
	if (row < 0)
		return 2.0 * mesh_ctrl(m, 0, col, axis) - mesh_ctrl(m, 1, col, axis);
	if (row >= n)
		return 2.0 * mesh_ctrl(m, n - 1, col, axis) - mesh_ctrl(m, n - 2, col, axis);
	if (col < 0)
		return 2.0 * m->off[row][0][axis] - m->off[row][1][axis];
	if (col >= n)
		return 2.0 * m->off[row][n - 1][axis] - m->off[row][n - 2][axis];
	return m->off[row][col][axis];
}

// Smooth displacement at source px s (constant past the edges, so the map
// stays invertible outside the source).
static void mesh_offset(const struct mesh *m, double sx, double sy, double out[2])
{
	const int n = m->n;
	const double u = fmin(fmax(sx / m->sw, 0.0), 1.0) * (n - 1);
	const double v = fmin(fmax(sy / m->sh, 0.0), 1.0) * (n - 1);
	const int i = (int)fmin(floor(u), n - 2), j = (int)fmin(floor(v), n - 2);
	const double tu = u - i, tv = v - j;
	for (int axis = 0; axis < 2; axis++) {
		double rows[4];
		for (int k = 0; k < 4; k++) {
			const int row = j - 1 + k;
			rows[k] = catmull_rom(mesh_ctrl(m, row, i - 1, axis), mesh_ctrl(m, row, i, axis),
					      mesh_ctrl(m, row, i + 1, axis), mesh_ctrl(m, row, i + 2, axis), tu);
		}
		out[axis] = catmull_rom(rows[0], rows[1], rows[2], rows[3], tv);
	}
}

// Solves s + offset(s) = x for s (Newton). Returns the residual in px.
static double mesh_invert(const struct mesh *m, double x, double y, double *sx, double *sy)
{
	double o[2];
	mesh_offset(m, x, y, o);
	double px = x - o[0], py = y - o[1];
	double err = 0.0;
	for (int it = 0; it < 24; it++) {
		mesh_offset(m, px, py, o);
		const double fx = px + o[0] - x, fy = py + o[1] - y;
		err = sqrt(fx * fx + fy * fy);
		if (err < 1e-3)
			break;
		const double e = 0.5;
		double ox[2], oy[2];
		mesh_offset(m, px + e, py, ox);
		mesh_offset(m, px, py + e, oy);
		const double j00 = 1.0 + (ox[0] - o[0]) / e, j01 = (oy[0] - o[0]) / e;
		const double j10 = (ox[1] - o[1]) / e, j11 = 1.0 + (oy[1] - o[1]) / e;
		const double det = j00 * j11 - j01 * j10;
		double dx, dy;
		if (fabs(det) < 1e-6) {
			dx = fx;
			dy = fy;
		} else {
			dx = (j11 * fx - j01 * fy) / det;
			dy = (-j10 * fx + j00 * fy) / det;
		}
		// Damped, so folds don't throw the iteration far away.
		const double len = sqrt(dx * dx + dy * dy), max_step = 0.5 * fmax(m->sw, m->sh);
		if (len > max_step) {
			dx *= max_step / len;
			dy *= max_step / len;
		}
		px -= dx;
		py -= dy;
	}
	*sx = px;
	*sy = py;
	return err;
}

static void mesh_params(struct warp_data *d, gs_effect_t *effect, obs_data_t *s, obs_data_t *to, float t, int n,
			double sw, double sh, uint32_t ow, uint32_t oh, double pad)
{
	struct mesh m = {.n = n, .sw = sw, .sh = sh};
	double key[2 * MESH_MAX * MESH_MAX + 6] = {0};
	int k = 0;
	for (int row = 0; row < n; row++) {
		for (int col = 0; col < n; col++) {
			char kx[32], ky[32];
			mesh_key(kx, sizeof(kx), row, col, 'x');
			mesh_key(ky, sizeof(ky), row, col, 'y');
			m.off[row][col][0] = fx_lerp_setting(s, to, t, kx);
			m.off[row][col][1] = fx_lerp_setting(s, to, t, ky);
			key[k++] = m.off[row][col][0];
			key[k++] = m.off[row][col][1];
		}
	}
	key[2 * MESH_MAX * MESH_MAX + 0] = n;
	key[2 * MESH_MAX * MESH_MAX + 1] = sw;
	key[2 * MESH_MAX * MESH_MAX + 2] = sh;
	key[2 * MESH_MAX * MESH_MAX + 3] = ow;
	key[2 * MESH_MAX * MESH_MAX + 4] = oh;
	key[2 * MESH_MAX * MESH_MAX + 5] = pad;

	// Nodes span the whole output, MAP_NODES along the longer side.
	const double longer = fmax(ow, oh);
	const uint32_t mw = (uint32_t)fmax(4.0, round(MAP_NODES * ow / longer));
	const uint32_t mh = (uint32_t)fmax(4.0, round(MAP_NODES * oh / longer));

	if (!d->map_valid || memcmp(key, d->map_key, sizeof(key)) != 0 || d->map_w != mw || d->map_h != mh) {
		float *data = bmalloc(sizeof(float) * 4 * mw * mh);
		for (uint32_t j = 0; j < mh; j++) {
			for (uint32_t i = 0; i < mw; i++) {
				const double x = (double)i / (mw - 1) * ow - pad;
				const double y = (double)j / (mh - 1) * oh - pad;
				double sx, sy;
				const double err = mesh_invert(&m, x, y, &sx, &sy);
				float *p = data + 4 * (j * mw + i);
				p[0] = (float)sx;
				p[1] = (float)sy;
				p[2] = err < 0.5 ? 1.0f : 0.0f;
				p[3] = 1.0f;
			}
		}
		if (d->map && (d->map_w != mw || d->map_h != mh)) {
			gs_texture_destroy(d->map);
			d->map = NULL;
		}
		if (!d->map)
			d->map = gs_texture_create(mw, mh, GS_RGBA32F, 1, (const uint8_t **)&data, GS_DYNAMIC);
		else
			gs_texture_set_image(d->map, (const uint8_t *)data, mw * 4 * sizeof(float), false);
		bfree(data);
		d->map_w = mw;
		d->map_h = mh;
		memcpy(d->map_key, key, sizeof(key));
		d->map_valid = d->map != NULL;
	}
	fx_set_texture(effect, "map_tex", d->map);
	fx_set_vec2(effect, "map_nodes", (float)mw, (float)mh);
}

/* ------------------------------------------------------------------------
 * Curl
 * --------------------------------------------------------------------- */

static void curl_params(gs_effect_t *effect, obs_data_t *s, obs_data_t *to, float t, double sw, double sh)
{
	const double a = fx_lerp_setting(s, to, t, "wp_curl_angle") * M_PI / 180.0;
	const double amount = fx_lerp_setting(s, to, t, "wp_curl_amount") / 100.0;
	const double dx = cos(a), dy = sin(a);
	// The fold travels from the corner furthest along the direction.
	const double proj[4] = {0.0, sw * dx, sw * dx + sh * dy, sh * dy};
	double lo = proj[0], hi = proj[0];
	for (int i = 1; i < 4; i++) {
		lo = fmin(lo, proj[i]);
		hi = fmax(hi, proj[i]);
	}
	fx_set_vec2(effect, "curl_dir", (float)dx, (float)dy);
	fx_set_float(effect, "curl_fold", (float)(hi - amount * (hi - lo)));
}

static void warp_set_params(fx_filter_t *fx, gs_effect_t *effect, obs_data_t *settings, obs_data_t *to, float t,
			    uint32_t width, uint32_t height)
{
	struct warp_data *d = fx->user;
	const uint32_t pad = warp_padding(settings);
	const double sw = (double)width - 2.0 * pad, sh = (double)height - 2.0 * pad;
	if (sw < 1.0 || sh < 1.0)
		return;
	fx_set_vec2(effect, "src_size", (float)sw, (float)sh);
	fx_set_float(effect, "hom_valid", 0.0f);
	fx_set_float(effect, "backside", 0.0f);
	fx_set_texture(effect, "map_tex", NULL);
	fx_set_vec2(effect, "map_nodes", 2.0f, 2.0f);
	fx_set_vec2(effect, "curl_dir", 1.0f, 0.0f);
	fx_set_float(effect, "curl_fold", 0.0f);

	obs_data_t *mode_src = (to && t >= 0.5f) ? to : settings;
	const int mode = (int)obs_data_get_int(mode_src, "wp_mode");
	// Move Transition between two different modes: interpolate the
	// settings of the mode shown.
	if (to && obs_data_get_int(settings, "wp_mode") != obs_data_get_int(to, "wp_mode"))
		to = NULL;

	switch (mode) {
	case MODE_CORNER_PIN:
		corner_pin_params(effect, settings, to, t, sw, sh);
		break;
	case MODE_TILT:
		tilt_params(effect, settings, to, t, sw, sh);
		break;
	case MODE_MESH3:
	case MODE_MESH4:
		mesh_params(d, effect, settings, to, t, mode == MODE_MESH3 ? 3 : 4, sw, sh, width, height, pad);
		break;
	case MODE_PAGE_CURL:
	case MODE_ROLL:
		curl_params(effect, settings, to, t, sw, sh);
		break;
	default:
		break;
	}
}

static const struct fx_filter_def warp_def = {
	.effect_file = "shaders/fx/warp.effect",
	.dither_key = "wp_dither",
	.set_params = warp_set_params,
	.padding = warp_padding,
	.create = warp_create_hook,
	.destroy = warp_destroy_hook,
};

/* ------------------------------------------------------------------------
 * obs_source_info
 * --------------------------------------------------------------------- */

static const char *warp_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("WarpFilter");
}

static void *warp_create(obs_data_t *settings, obs_source_t *source)
{
	return fx_filter_create(&warp_def, settings, source);
}

static void warp_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "wp_mode", MODE_CORNER_PIN);
	obs_data_set_default_int(settings, "wp_padding", 0);
	for (int i = 0; i < 4; i++) {
		obs_data_set_default_double(settings, corner_keys[i][0], 0.0);
		obs_data_set_default_double(settings, corner_keys[i][1], 0.0);
	}
	for (int row = 0; row < MESH_MAX; row++) {
		for (int col = 0; col < MESH_MAX; col++) {
			char k[32];
			mesh_key(k, sizeof(k), row, col, 'x');
			obs_data_set_default_double(settings, k, 0.0);
			mesh_key(k, sizeof(k), row, col, 'y');
			obs_data_set_default_double(settings, k, 0.0);
		}
	}
	obs_data_set_default_double(settings, "wp_tilt_x", 0.0);
	obs_data_set_default_double(settings, "wp_tilt_y", 25.0);
	obs_data_set_default_double(settings, "wp_tilt_z", 0.0);
	obs_data_set_default_double(settings, "wp_fov", 50.0);
	obs_data_set_default_double(settings, "wp_tilt_zoom", 100.0);
	obs_data_set_default_double(settings, "wp_bend_angle", 60.0);
	obs_data_set_default_int(settings, "wp_bend_axis", 1);
	obs_data_set_default_double(settings, "wp_curl_amount", 35.0);
	obs_data_set_default_double(settings, "wp_curl_angle", 45.0);
	obs_data_set_default_double(settings, "wp_curl_radius", 40.0);
	obs_data_set_default_int(settings, "wp_back_color", 0xB3F0F0F0);
	obs_data_set_default_double(settings, "wp_shading", 60.0);
	obs_data_set_default_double(settings, "wp_shadow", 50.0);
	obs_data_set_default_bool(settings, "wp_area_filter", true);
	obs_data_set_default_bool(settings, "wp_dither", false);
}

static void vis(obs_properties_t *props, const char *key, bool visible)
{
	obs_property_set_visible(obs_properties_get(props, key), visible);
}

static bool mode_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	UNUSED_PARAMETER(p);
	const int m = (int)obs_data_get_int(settings, "wp_mode");
	const bool mesh = m == MODE_MESH3 || m == MODE_MESH4;
	const int n = m == MODE_MESH3 ? 3 : 4;
	const bool curl = m == MODE_PAGE_CURL || m == MODE_ROLL;
	vis(props, "wp_corner_group", m == MODE_CORNER_PIN);
	vis(props, "wp_mesh_group", mesh);
	for (int row = 0; row < MESH_MAX; row++) {
		for (int col = 0; col < MESH_MAX; col++) {
			char k[32];
			mesh_key(k, sizeof(k), row, col, 'x');
			vis(props, k, mesh && row < n && col < n);
			mesh_key(k, sizeof(k), row, col, 'y');
			vis(props, k, mesh && row < n && col < n);
		}
	}
	vis(props, "wp_reset_points", m == MODE_CORNER_PIN || mesh);
	vis(props, "wp_tilt_x", m == MODE_TILT);
	vis(props, "wp_tilt_y", m == MODE_TILT);
	vis(props, "wp_tilt_z", m == MODE_TILT);
	vis(props, "wp_fov", m == MODE_TILT);
	vis(props, "wp_tilt_zoom", m == MODE_TILT);
	vis(props, "wp_bend_angle", m == MODE_BEND);
	vis(props, "wp_bend_axis", m == MODE_BEND);
	vis(props, "wp_curl_amount", curl);
	vis(props, "wp_curl_angle", curl);
	vis(props, "wp_curl_radius", curl);
	vis(props, "wp_shadow", curl);
	vis(props, "wp_shading", curl);
	vis(props, "wp_back_color", curl || m == MODE_TILT || m == MODE_CORNER_PIN);
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

static bool reset_points_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	fx_filter_t *fx = data;
	if (!fx)
		return false;
	obs_data_t *settings = obs_source_get_settings(fx->context);
	for (int i = 0; i < 4; i++) {
		obs_data_unset_user_value(settings, corner_keys[i][0]);
		obs_data_unset_user_value(settings, corner_keys[i][1]);
	}
	for (int row = 0; row < MESH_MAX; row++) {
		for (int col = 0; col < MESH_MAX; col++) {
			char k[32];
			mesh_key(k, sizeof(k), row, col, 'x');
			obs_data_unset_user_value(settings, k);
			mesh_key(k, sizeof(k), row, col, 'y');
			obs_data_unset_user_value(settings, k);
		}
	}
	obs_source_update(fx->context, NULL);
	obs_data_release(settings);
	return true;
}

static obs_properties_t *warp_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(props, "wp_mode", obs_module_text("Warp.Mode"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Warp.Mode.CornerPin"), MODE_CORNER_PIN);
	obs_property_list_add_int(p, obs_module_text("Warp.Mode.Mesh3"), MODE_MESH3);
	obs_property_list_add_int(p, obs_module_text("Warp.Mode.Mesh4"), MODE_MESH4);
	obs_property_list_add_int(p, obs_module_text("Warp.Mode.Tilt"), MODE_TILT);
	obs_property_list_add_int(p, obs_module_text("Warp.Mode.Bend"), MODE_BEND);
	obs_property_list_add_int(p, obs_module_text("Warp.Mode.PageCurl"), MODE_PAGE_CURL);
	obs_property_list_add_int(p, obs_module_text("Warp.Mode.Roll"), MODE_ROLL);
	obs_property_set_modified_callback(p, mode_modified);

	p = obs_properties_add_int_slider(props, "wp_padding", obs_module_text("Warp.Padding"), 0, 2000, 1);
	obs_property_int_set_suffix(p, " px");
	obs_property_set_long_description(p, obs_module_text("Warp.Padding.Description"));

	// Corner pin: offsets of each corner from where it starts, px.
	obs_properties_t *g = obs_properties_create();
	static const char *corner_names[4][2] = {
		{"Warp.TopLeftX", "Warp.TopLeftY"},
		{"Warp.TopRightX", "Warp.TopRightY"},
		{"Warp.BottomRightX", "Warp.BottomRightY"},
		{"Warp.BottomLeftX", "Warp.BottomLeftY"},
	};
	for (int i = 0; i < 4; i++)
		for (int axis = 0; axis < 2; axis++)
			slider(g, corner_keys[i][axis], corner_names[i][axis], -4000.0, 4000.0, 0.5, "px");
	obs_properties_add_group(props, "wp_corner_group", obs_module_text("Warp.Corners"), OBS_GROUP_NORMAL, g);

	g = obs_properties_create();
	for (int row = 0; row < MESH_MAX; row++) {
		for (int col = 0; col < MESH_MAX; col++) {
			for (int axis = 0; axis < 2; axis++) {
				char k[32], text[128];
				mesh_key(k, sizeof(k), row, col, axis ? 'y' : 'x');
				snprintf(text, sizeof(text), "%s %d,%d %s", obs_module_text("Warp.MeshPoint"), row + 1,
					 col + 1, axis ? "Y" : "X");
				p = obs_properties_add_float_slider(g, k, text, -4000.0, 4000.0, 0.5);
				obs_property_float_set_suffix(p, "px");
			}
		}
	}
	obs_properties_add_group(props, "wp_mesh_group", obs_module_text("Warp.Mesh"), OBS_GROUP_NORMAL, g);
	obs_properties_add_button2(props, "wp_reset_points", obs_module_text("Warp.ResetPoints"), reset_points_clicked,
				   data);

	slider(props, "wp_tilt_x", "Warp.TiltX", -180.0, 180.0, 0.1, "°");
	slider(props, "wp_tilt_y", "Warp.TiltY", -180.0, 180.0, 0.1, "°");
	slider(props, "wp_tilt_z", "Warp.TiltZ", -180.0, 180.0, 0.1, "°");
	slider(props, "wp_fov", "Warp.Fov", 1.0, 170.0, 0.1, "°");
	slider(props, "wp_tilt_zoom", "Warp.Zoom", 1.0, 400.0, 0.1, "%");

	slider(props, "wp_bend_angle", "Warp.BendAngle", -360.0, 360.0, 0.1, "°");
	p = obs_properties_add_list(props, "wp_bend_axis", obs_module_text("Warp.BendAxis"), OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, obs_module_text("Warp.BendAxis.Horizontal"), 1);
	obs_property_list_add_int(p, obs_module_text("Warp.BendAxis.Vertical"), 2);

	slider(props, "wp_curl_amount", "Warp.CurlAmount", 0.0, 100.0, 0.1, "%");
	slider(props, "wp_curl_angle", "Warp.CurlAngle", -180.0, 180.0, 0.1, "°");
	slider(props, "wp_curl_radius", "Warp.CurlRadius", 1.0, 1000.0, 0.5, "px");
	slider(props, "wp_shading", "Warp.Shading", 0.0, 100.0, 0.1, "%");
	slider(props, "wp_shadow", "Warp.Shadow", 0.0, 100.0, 0.1, "%");
	obs_properties_add_color_alpha(props, "wp_back_color", obs_module_text("Warp.BackColor"));

	p = obs_properties_add_bool(props, "wp_area_filter", obs_module_text("Warp.AreaFilter"));
	obs_property_set_long_description(p, obs_module_text("Warp.AreaFilter.Description"));
	obs_properties_add_bool(props, "wp_dither", obs_module_text("Gradient.Dither"));
	spectrum_presets_properties(props, data ? ((fx_filter_t *)data)->context : NULL);
	return props;
}

struct obs_source_info spectrum_warp_filter = {
	.id = "spectrum_warp_filter",
	.type = OBS_SOURCE_TYPE_FILTER,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_SRGB,
	.get_name = warp_name,
	.create = warp_create,
	.destroy = fx_cb_destroy,
	.update = fx_cb_update,
	.video_tick = fx_cb_tick,
	.video_render = fx_cb_render,
	.get_properties = warp_properties,
	.get_defaults = warp_defaults,
};
