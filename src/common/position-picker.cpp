#include "position-picker.hpp"

#include <obs-frontend-api.h>
#include <graphics/vec4.h>

#include <QDialogButtonBox>
#include <QFontMetricsF>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

namespace spectrum_picker {

namespace {

constexpr int kMaxSnapshotWidth = 960;
constexpr int kMaxSnapshotHeight = 540;
constexpr double kHandleRadius = 7.0;
constexpr double kPi = 3.14159265358979323846;

QString T(const char *id)
{
	return QString::fromUtf8(obs_module_text(id));
}

#define PT(label, kx, ky) {HandleType::PointPercent, label, kx, ky, nullptr, nullptr, -1, false, 0.0, nullptr, {0}}
#define PT_IF(label, kx, ky, ck, ...) \
	{HandleType::PointPercent, label, kx, ky, nullptr, nullptr, -1, false, 0.0, ck, {__VA_ARGS__}}
#define ANGLE(label, key, anchor, y_up) {HandleType::Angle, label, nullptr, nullptr, key, nullptr, anchor, y_up, 0.0, nullptr, {0}}
#define ANGLE_IF(label, key, anchor, y_up, ck, ...) \
	{HandleType::Angle, label, nullptr, nullptr, key, nullptr, anchor, y_up, 0.0, ck, {__VA_ARGS__}}

// Handles per filter/source type (unversioned id).
const std::map<std::string, std::vector<HandleSpec>> &specs()
{
	static const std::map<std::string, std::vector<HandleSpec>> table = {
		{"spectrum_gradient_source",
		 {PT("Picker.GradientCenter", "gradient_center_x", "gradient_center_y"),
		  ANGLE("Picker.GradientAngle", "gradient_angle", 0, false)}},
		{"spectrum_shape_source",
		 {ANGLE("Picker.Rotation", "sh_rotation", -1, false),
		  {HandleType::PointOffset,
		   "Picker.ShadowOffset",
		   "sh_shadow_x",
		   "sh_shadow_y",
		   nullptr,
		   nullptr,
		   -1,
		   false,
		   0.0,
		   "sh_shadow",
		   {0}},
		  PT_IF("Picker.GradientCenter", "sh_gradient_center_x", "sh_gradient_center_y", "sh_fill_type", 2),
		  ANGLE_IF("Picker.GradientAngle", "sh_gradient_angle", 2, false, "sh_fill_type", 2)}},
		{"spectrum_stroke_filter",
		 {PT_IF("Picker.GradientCenter", "stroke_gradient_center_x", "stroke_gradient_center_y",
			"stroke_fill_type", 4),
		  ANGLE_IF("Picker.GradientAngle", "stroke_gradient_angle", 0, false, "stroke_fill_type", 4)}},
		{"spectrum_glow_filter",
		 {PT_IF("Picker.GradientCenter", "glow_gradient_center_x", "glow_gradient_center_y", "glow_fill_type",
			4),
		  ANGLE_IF("Picker.GradientAngle", "glow_gradient_angle", 0, false, "glow_fill_type", 4)}},
		{"spectrum_shadow_filter",
		 {{HandleType::Polar,
		   "Picker.ShadowOffset",
		   nullptr,
		   nullptr,
		   "glow_offset_angle",
		   "glow_offset_distance",
		   -1,
		   false,
		   50.0,
		   nullptr,
		   {0}},
		  PT_IF("Picker.GradientCenter", "glow_gradient_center_x", "glow_gradient_center_y", "glow_fill_type",
			4),
		  ANGLE_IF("Picker.GradientAngle", "glow_gradient_angle", 1, false, "glow_fill_type", 4)}},
		{"spectrum_long_shadow_filter", {ANGLE("Picker.ShadowDirection", "ls_angle", -1, false)}},
		{"spectrum_bevel_filter", {ANGLE("Picker.LightAngle", "bv_angle", -1, true)}},
		{"spectrum_stylize_filter",
		 {{HandleType::Angle,
		   "Picker.AberrationAngle",
		   nullptr,
		   nullptr,
		   "st_ca_angle",
		   nullptr,
		   -1,
		   false,
		   0.0,
		   nullptr,
		   {0}}}},
		{"spectrum_distort_filter",
		 {PT_IF("Picker.DistortCenter", "ds_center_x", "ds_center_y", "ds_mode", 2, 3, 4),
		  ANGLE_IF("Picker.WaveAngle", "ds_angle", -1, false, "ds_mode", 1)}},
		{"spectrum_light_filter",
		 {PT("Picker.RaysSource", "li_rays_x", "li_rays_y"),
		  PT("Picker.FlarePosition", "li_flare_x", "li_flare_y"),
		  PT("Picker.PointLight", "li_light_x", "li_light_y")}},
	};
	return table;
}

bool condition_met(obs_data_t *settings, const HandleSpec &spec)
{
	if (!spec.cond_key)
		return true;
	if (spec.cond_values[0] == 0)
		return obs_data_get_bool(settings, spec.cond_key);
	const long long v = obs_data_get_int(settings, spec.cond_key);
	for (int value : spec.cond_values) {
		if (value == 0)
			break;
		if (v == value)
			return true;
	}
	return false;
}

} // namespace

/* ------------------------------------------------------------------------
 * PickerCanvas
 * --------------------------------------------------------------------- */

PickerCanvas::PickerCanvas(obs_source_t *target, std::vector<Handle> handles, QWidget *parent)
	: QWidget(parent),
	  _target(obs_source_get_ref(target)),
	  _handles(std::move(handles))
{
	obs_source_t *parent_source =
		obs_source_get_type(target) == OBS_SOURCE_TYPE_FILTER ? obs_filter_get_parent(target) : target;
	_rendered = obs_source_get_ref(parent_source);
	obs_data_t *settings = obs_source_get_settings(_target);
	_original = obs_data_create_from_json(obs_data_get_json(settings));
	obs_data_release(settings);

	setMouseTracking(true);
	setMinimumSize(320, 180);
	loadHandles();
	refreshSnapshot();
}

PickerCanvas::~PickerCanvas()
{
	obs_enter_graphics();
	gs_texrender_destroy(_texrender);
	gs_stagesurface_destroy(_stage);
	obs_leave_graphics();
	obs_data_release(_original);
	obs_source_release(_rendered);
	obs_source_release(_target);
}

QSize PickerCanvas::sizeHint() const
{
	if (_sourceWidth > 0 && _sourceHeight > 0) {
		const double s = std::min(1.0, std::min(800.0 / _sourceWidth, 450.0 / _sourceHeight));
		return QSize((int)(_sourceWidth * s), (int)(_sourceHeight * s));
	}
	return QSize(640, 360);
}

void PickerCanvas::refreshSnapshot()
{
	if (!_rendered)
		return;
	const uint32_t w = obs_source_get_width(_rendered);
	const uint32_t h = obs_source_get_height(_rendered);
	if (!w || !h)
		return;
	if ((int)w != _sourceWidth || (int)h != _sourceHeight) {
		_sourceWidth = (int)w;
		_sourceHeight = (int)h;
		loadHandles();
		updateGeometry();
	}

	const double scale = std::min(1.0, std::min((double)kMaxSnapshotWidth / w, (double)kMaxSnapshotHeight / h));
	const uint32_t sw = std::max(1u, (uint32_t)(w * scale)), sh = std::max(1u, (uint32_t)(h * scale));

	obs_enter_graphics();
	if (!_texrender)
		_texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	gs_texrender_reset(_texrender);
	if (!_stage || _stageW != sw || _stageH != sh) {
		gs_stagesurface_destroy(_stage);
		_stage = gs_stagesurface_create(sw, sh, GS_RGBA);
		_stageW = sw;
		_stageH = sh;
	}
	if (gs_texrender_begin_with_color_space(_texrender, sw, sh, GS_CS_SRGB)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)w, 0.0f, (float)h, -100.0f, 100.0f);
		gs_blend_state_push();
		gs_blend_function_separate(GS_BLEND_SRCALPHA, GS_BLEND_INVSRCALPHA, GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		obs_source_video_render(_rendered);
		gs_blend_state_pop();
		gs_texrender_end(_texrender);
		gs_stage_texture(_stage, gs_texrender_get_texture(_texrender));
		uint8_t *data;
		uint32_t linesize;
		if (gs_stagesurface_map(_stage, &data, &linesize)) {
			QImage image((int)sw, (int)sh, QImage::Format_RGBA8888);
			for (uint32_t y = 0; y < sh; y++)
				memcpy(image.scanLine((int)y), data + y * linesize, sw * 4);
			gs_stagesurface_unmap(_stage);
			_image = image;
		}
	}
	obs_leave_graphics();
	update();
}

void PickerCanvas::loadHandles()
{
	obs_data_t *settings = obs_source_get_settings(_target);
	const double cx = _sourceWidth * 0.5, cy = _sourceHeight * 0.5;
	for (Handle &h : _handles) {
		const HandleSpec &s = *h.spec;
		h.visible = condition_met(settings, s);
		switch (s.type) {
		case HandleType::PointPercent:
			h.pos = QPointF(obs_data_get_double(settings, s.key_x) / 100.0 * _sourceWidth,
					obs_data_get_double(settings, s.key_y) / 100.0 * _sourceHeight);
			break;
		case HandleType::PointOffset:
			h.pos = QPointF(cx + obs_data_get_double(settings, s.key_x),
					cy + obs_data_get_double(settings, s.key_y));
			break;
		case HandleType::Angle:
			h.pos = QPointF(obs_data_get_double(settings, s.key_angle), 0.0);
			break;
		case HandleType::Polar: {
			const double a = obs_data_get_double(settings, s.key_angle) * kPi / 180.0;
			const double d = obs_data_get_double(settings, s.key_distance);
			h.pos = QPointF(cx + std::cos(a) * d, cy + std::sin(a) * d);
			break;
		}
		}
	}
	obs_data_release(settings);
}

QRectF PickerCanvas::imageRect() const
{
	if (_sourceWidth <= 0 || _sourceHeight <= 0)
		return QRectF(rect());
	// A margin so handles on (or just past) the edges stay reachable.
	const double margin = 28.0;
	const double aw = std::max(width() - 2.0 * margin, 1.0), ah = std::max(height() - 2.0 * margin, 1.0);
	const double s = std::min(aw / _sourceWidth, ah / _sourceHeight);
	const double w = _sourceWidth * s, h = _sourceHeight * s;
	return QRectF((width() - w) * 0.5, (height() - h) * 0.5, w, h);
}

QPointF PickerCanvas::toWidget(const QPointF &p) const
{
	const QRectF r = imageRect();
	return QPointF(r.left() + p.x() / std::max(_sourceWidth, 1) * r.width(),
		       r.top() + p.y() / std::max(_sourceHeight, 1) * r.height());
}

QPointF PickerCanvas::toSource(const QPointF &p) const
{
	const QRectF r = imageRect();
	return QPointF((p.x() - r.left()) / r.width() * _sourceWidth, (p.y() - r.top()) / r.height() * _sourceHeight);
}

QPointF PickerCanvas::anchorWidgetPos(const Handle &h) const
{
	const int a = h.spec->anchor;
	if (a >= 0 && a < (int)_handles.size() && _handles[a].visible &&
	    (_handles[a].spec->type == HandleType::PointPercent || _handles[a].spec->type == HandleType::PointOffset))
		return toWidget(_handles[a].pos);
	return imageRect().center();
}

QPointF PickerCanvas::handleWidgetPos(int index) const
{
	const Handle &h = _handles[index];
	if (h.spec->type == HandleType::Angle) {
		const QRectF r = imageRect();
		const double radius = std::min(r.width(), r.height()) * 0.3;
		double a = h.pos.x() * kPi / 180.0;
		if (h.spec->y_up)
			a = -a;
		return anchorWidgetPos(h) + QPointF(std::cos(a), std::sin(a)) * radius;
	}
	return toWidget(h.pos);
}

void PickerCanvas::applyHandle(int index, const QPointF &p)
{
	Handle &h = _handles[index];
	const HandleSpec &s = *h.spec;
	obs_data_t *changes = obs_data_create();
	const double cx = _sourceWidth * 0.5, cy = _sourceHeight * 0.5;

	switch (s.type) {
	case HandleType::PointPercent:
		h.pos = p;
		obs_data_set_double(changes, s.key_x, p.x() / _sourceWidth * 100.0);
		obs_data_set_double(changes, s.key_y, p.y() / _sourceHeight * 100.0);
		break;
	case HandleType::PointOffset:
		h.pos = p;
		obs_data_set_double(changes, s.key_x, std::round((p.x() - cx) * 2.0) / 2.0);
		obs_data_set_double(changes, s.key_y, std::round((p.y() - cy) * 2.0) / 2.0);
		break;
	case HandleType::Angle: {
		const QPointF anchor = toSource(anchorWidgetPos(h));
		double a = std::atan2(p.y() - anchor.y(), p.x() - anchor.x()) * 180.0 / kPi;
		if (s.y_up)
			a = -a;
		a = std::round(a * 10.0) / 10.0;
		h.pos = QPointF(a, 0.0);
		obs_data_set_double(changes, s.key_angle, a);
		break;
	}
	case HandleType::Polar: {
		const double dx = p.x() - cx, dy = p.y() - cy;
		const double d = std::min(std::sqrt(dx * dx + dy * dy), s.max_distance);
		const double a = std::atan2(dy, dx);
		h.pos = QPointF(cx + std::cos(a) * d, cy + std::sin(a) * d);
		obs_data_set_double(changes, s.key_angle, std::round(a * 1800.0 / kPi) / 10.0);
		obs_data_set_double(changes, s.key_distance, std::round(d));
		break;
	}
	}
	obs_source_update(_target, changes);
	obs_data_release(changes);
	update();
}

void PickerCanvas::restoreOriginal()
{
	obs_data_t *restore = obs_data_create();
	for (const Handle &h : _handles) {
		for (const char *key : {h.spec->key_x, h.spec->key_y, h.spec->key_angle, h.spec->key_distance}) {
			if (!key)
				continue;
			obs_data_item_t *item = obs_data_item_byname(_original, key);
			if (item) {
				obs_data_set_double(restore, key, obs_data_item_get_double(item));
				obs_data_item_release(&item);
			} else {
				// It was at its default: clear the user value again.
				obs_data_t *settings = obs_source_get_settings(_target);
				obs_data_unset_user_value(settings, key);
				obs_data_release(settings);
			}
		}
	}
	obs_source_update(_target, restore);
	obs_data_release(restore);
}

void PickerCanvas::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.fillRect(rect(), palette().color(QPalette::Dark));
	const QRectF r = imageRect();

	// Checkerboard under transparent areas.
	p.save();
	p.setClipRect(r);
	const int cell = 10;
	for (int y = 0; y * cell < r.height(); y++)
		for (int x = 0; x * cell < r.width(); x++)
			p.fillRect(QRectF(r.left() + x * cell, r.top() + y * cell, cell, cell),
				   (x + y) % 2 ? QColor(150, 150, 150) : QColor(110, 110, 110));
	p.restore();
	if (!_image.isNull()) {
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.drawImage(r, _image);
	}

	p.setRenderHint(QPainter::Antialiasing);
	for (int i = 0; i < (int)_handles.size(); i++) {
		const Handle &h = _handles[i];
		if (!h.visible)
			continue;
		const QPointF pos = handleWidgetPos(i);
		const bool active = i == _drag;
		const QColor accent = active ? QColor(255, 210, 63) : QColor(76, 201, 240);

		if (h.spec->type == HandleType::Angle || h.spec->type == HandleType::Polar) {
			const QPointF from = h.spec->type == HandleType::Angle ? anchorWidgetPos(h) : r.center();
			p.setPen(QPen(QColor(0, 0, 0, 160), 4.0));
			p.drawLine(from, pos);
			p.setPen(QPen(accent, 2.0, Qt::DashLine));
			p.drawLine(from, pos);
			// Arrow head showing the direction.
			const QPointF d = pos - from;
			const double len = std::hypot(d.x(), d.y());
			if (len > 1.0) {
				const QPointF u = d / len, n(-u.y(), u.x());
				QPainterPath arrow;
				arrow.moveTo(pos + u * 10.0);
				arrow.lineTo(pos - u * 4.0 + n * 6.0);
				arrow.lineTo(pos - u * 4.0 - n * 6.0);
				arrow.closeSubpath();
				p.setPen(QPen(Qt::black, 1.5));
				p.setBrush(accent);
				p.drawPath(arrow);
			}
		} else {
			p.setPen(QPen(Qt::black, 3.0));
			p.setBrush(Qt::NoBrush);
			p.drawEllipse(pos, kHandleRadius, kHandleRadius);
			p.setPen(QPen(accent, 2.0));
			p.setBrush(QColor(accent.red(), accent.green(), accent.blue(), 90));
			p.drawEllipse(pos, kHandleRadius, kHandleRadius);
			p.drawLine(pos - QPointF(kHandleRadius + 4, 0), pos + QPointF(kHandleRadius + 4, 0));
			p.drawLine(pos - QPointF(0, kHandleRadius + 4), pos + QPointF(0, kHandleRadius + 4));
		}

		const QString label = T(h.spec->label);
		// Keep the label inside the widget.
		const QFontMetricsF fm(font());
		QPointF text_pos = pos + QPointF(kHandleRadius + 6, -kHandleRadius - 4);
		text_pos.setX(std::clamp(text_pos.x(), 4.0, width() - fm.horizontalAdvance(label) - 4.0));
		text_pos.setY(std::clamp(text_pos.y(), fm.ascent() + 2.0, height() - fm.descent() - 2.0));
		QPainterPath text;
		text.addText(text_pos, font(), label);
		p.setPen(QPen(QColor(0, 0, 0, 200), 3.0));
		p.setBrush(Qt::NoBrush);
		p.drawPath(text);
		p.setPen(Qt::NoPen);
		p.setBrush(Qt::white);
		p.drawPath(text);
	}
}

void PickerCanvas::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;
	// The nearest visible handle within reach; later handles win ties.
	int best = -1;
	double best_d = 14.0;
	for (int i = 0; i < (int)_handles.size(); i++) {
		if (!_handles[i].visible)
			continue;
		const QPointF d = handleWidgetPos(i) - event->position();
		const double dist = std::hypot(d.x(), d.y());
		if (dist <= best_d) {
			best = i;
			best_d = dist;
		}
	}
	_drag = best;
	if (_drag >= 0)
		applyHandle(_drag, toSource(event->position()));
}

void PickerCanvas::mouseMoveEvent(QMouseEvent *event)
{
	if (_drag >= 0) {
		applyHandle(_drag, toSource(event->position()));
		return;
	}
	bool over = false;
	for (int i = 0; i < (int)_handles.size() && !over; i++) {
		if (!_handles[i].visible)
			continue;
		const QPointF d = handleWidgetPos(i) - event->position();
		over = std::hypot(d.x(), d.y()) <= 14.0;
	}
	setCursor(over ? Qt::SizeAllCursor : Qt::ArrowCursor);
}

void PickerCanvas::mouseReleaseEvent(QMouseEvent *)
{
	_drag = -1;
	update();
}

/* ------------------------------------------------------------------------
 * PositionPickerDialog
 * --------------------------------------------------------------------- */

PositionPickerDialog::PositionPickerDialog(obs_source_t *target, std::vector<Handle> handles, QWidget *parent)
	: QDialog(parent)
{
	setWindowTitle(T("Picker.Title").arg(QString::fromUtf8(obs_source_get_name(target))));
	auto *layout = new QVBoxLayout(this);
	_canvas = new PickerCanvas(target, std::move(handles), this);
	layout->addWidget(_canvas, 1);
	auto *hint = new QLabel(T("Picker.Hint"), this);
	hint->setWordWrap(true);
	layout->addWidget(hint);
	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	layout->addWidget(buttons);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	// Live snapshot, about 15 times a second.
	connect(&_timer, &QTimer::timeout, _canvas, &PickerCanvas::refreshSnapshot);
	_timer.start(66);
}

} // namespace spectrum_picker

/* ------------------------------------------------------------------------
 * C entry points
 * --------------------------------------------------------------------- */

using namespace spectrum_picker;

static const std::vector<HandleSpec> *find_specs(obs_source_t *source)
{
	const char *id = obs_source_get_unversioned_id(source);
	if (!id)
		return nullptr;
	auto it = specs().find(id);
	return it == specs().end() ? nullptr : &it->second;
}

static bool picker_clicked(obs_properties_t *props, obs_property_t *property, void *data)
{
	UNUSED_PARAMETER(props);
	UNUSED_PARAMETER(property);
	obs_source_t *source = static_cast<obs_source_t *>(data);
	const std::vector<HandleSpec> *list = find_specs(source);
	if (!list)
		return false;
	std::vector<Handle> handles;
	for (const HandleSpec &spec : *list)
		handles.push_back(Handle{&spec, true, QPointF()});

	QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	PositionPickerDialog dialog(source, std::move(handles), parent);
	if (dialog.exec() != QDialog::Accepted)
		dialog.canvas()->restoreOriginal();
	// Refresh the properties so the sliders show the new values.
	return true;
}

extern "C" void spectrum_position_picker_properties(obs_properties_t *props, obs_source_t *source)
{
	if (!source || !find_specs(source))
		return;
	obs_property_t *p = obs_properties_add_button2(props, "spectrum_position_picker",
						       obs_module_text("Picker.Button"), picker_clicked, source);
	obs_property_set_long_description(p, obs_module_text("Picker.Description"));
}
