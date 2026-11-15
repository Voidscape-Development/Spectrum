#include "curve-editor.hpp"
#include "animate-ui.h"

#include <obs-frontend-api.h>

#include <QComboBox>
#include <QContextMenuEvent>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace spectrum_animate {

static const double kHandleRadius = 5.0;
static const double kHitRadius = 9.0;

/* ------------------------------------------------------------------------
 * CurveView
 * --------------------------------------------------------------------- */

CurveView::CurveView(std::vector<an_keyframe> &keys, QWidget *parent) : QWidget(parent), _keys(keys)
{
	setFocusPolicy(Qt::StrongFocus);
	setMouseTracking(true);
	updateRange();
}

QSize CurveView::sizeHint() const
{
	return QSize(560, 300);
}

QSize CurveView::minimumSizeHint() const
{
	return QSize(240, 140);
}

QRectF CurveView::plotRect() const
{
	return QRectF(48.0, 12.0, std::max(10.0, width() - 60.0), std::max(10.0, height() - 36.0));
}

void CurveView::updateRange()
{
	double tMax = 0.0, vMin = 0.0, vMax = 100.0;
	for (const an_keyframe &k : _keys) {
		tMax = std::max(tMax, k.time);
		vMin = std::min(vMin, k.value);
		vMax = std::max(vMax, k.value);
	}
	// Room for overshooting easings and for dragging past the end.
	_tMax = std::max(1.0, tMax * 1.15);
	const double pad = (vMax - vMin) * 0.12;
	_vMin = vMin - pad;
	_vMax = vMax + pad;
}

QPointF CurveView::toWidget(double time, double value) const
{
	const QRectF r = plotRect();
	return QPointF(r.left() + time / _tMax * r.width(),
		       r.bottom() - (value - _vMin) / (_vMax - _vMin) * r.height());
}

double CurveView::timeAt(double x) const
{
	const QRectF r = plotRect();
	return std::max(0.0, (x - r.left()) / r.width() * _tMax);
}

double CurveView::valueAt(double y) const
{
	const QRectF r = plotRect();
	return _vMin + (r.bottom() - y) / r.height() * (_vMax - _vMin);
}

int CurveView::keyAt(const QPointF &p) const
{
	int best = -1;
	double bestDist = kHitRadius;
	for (size_t i = 0; i < _keys.size(); i++) {
		const QPointF d = toWidget(_keys[i].time, _keys[i].value) - p;
		const double dist = std::hypot(d.x(), d.y());
		if (dist <= bestDist) {
			bestDist = dist;
			best = (int)i;
		}
	}
	return best;
}

void CurveView::setSelected(int index)
{
	if (_keys.empty())
		index = -1;
	else
		index = std::clamp(index, 0, (int)_keys.size() - 1);
	if (index == _selected)
		return;
	_selected = index;
	update();
	emit selectionChanged(_selected);
}

// Moves the selected key to keep the keys sorted by time.
void CurveView::resort()
{
	int s = _selected;
	if (s < 0 || s >= (int)_keys.size())
		return;
	while (s > 0 && _keys[s - 1].time > _keys[s].time) {
		std::swap(_keys[s - 1], _keys[s]);
		s--;
	}
	while (s + 1 < (int)_keys.size() && _keys[s + 1].time < _keys[s].time) {
		std::swap(_keys[s + 1], _keys[s]);
		s++;
	}
	_selected = s;
	if (_drag >= 0)
		_drag = s;
}

void CurveView::keysEdited()
{
	const int before = _selected;
	resort();
	if (_drag < 0)
		updateRange();
	update();
	emit keysChanged();
	if (_selected != before)
		emit selectionChanged(_selected);
}

void CurveView::deleteSelected()
{
	if (_keys.size() <= 1 || _selected < 0 || _selected >= (int)_keys.size())
		return;
	_keys.erase(_keys.begin() + _selected);
	_selected = std::min(_selected, (int)_keys.size() - 1);
	updateRange();
	update();
	emit keysChanged();
	emit selectionChanged(_selected);
}

void CurveView::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing, true);
	const QPalette pal = palette();
	const QRectF r = plotRect();
	const QColor text = pal.color(QPalette::Text);
	QColor grid = text;
	grid.setAlphaF(0.15f);
	QColor faint = text;
	faint.setAlphaF(0.5f);

	p.fillRect(rect(), pal.color(QPalette::Base));

	// Time grid: one line per "nice" step.
	const double steps[] = {0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 15.0, 30.0, 60.0};
	double step = steps[0];
	for (double s : steps) {
		step = s;
		if (_tMax / s <= 10.0)
			break;
	}
	QFont small = font();
	small.setPointSizeF(std::max(6.0, small.pointSizeF() * 0.85));
	p.setFont(small);
	for (double t = 0.0; t <= _tMax + 1e-9; t += step) {
		const double x = toWidget(t, 0.0).x();
		p.setPen(QPen(grid, 1.0));
		p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
		p.setPen(faint);
		p.drawText(QRectF(x - 30.0, r.bottom() + 2.0, 60.0, 16.0), Qt::AlignHCenter | Qt::AlignTop,
			   QString::number(t, 'g', 3) + "s");
	}

	// The From (0%) and To (100%) lines.
	for (double v : {0.0, 100.0}) {
		const double y = toWidget(0.0, v).y();
		p.setPen(QPen(faint, 1.0, Qt::DashLine));
		p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
		p.setPen(faint);
		p.drawText(QRectF(0.0, y - 8.0, r.left() - 6.0, 16.0), Qt::AlignRight | Qt::AlignVCenter,
			   QString::fromUtf8(obs_module_text(v == 0.0 ? "Animate.Min" : "Animate.Max")));
	}
	p.setPen(QPen(grid, 1.0));
	p.setBrush(Qt::NoBrush);
	p.drawRect(r);

	if (_keys.empty())
		return;

	// The curve, sampled per pixel.
	QPainterPath path;
	const double duration = an_keys_duration(_keys.data(), _keys.size());
	for (int i = 0; i <= (int)r.width(); i++) {
		const double t = timeAt(r.left() + i);
		const double v = an_keys_eval(_keys.data(), _keys.size(), std::min(t, duration));
		const QPointF pt = toWidget(t, v);
		if (i == 0)
			path.moveTo(pt);
		else
			path.lineTo(pt);
	}
	const QColor accent = pal.color(QPalette::Highlight);
	p.save();
	p.setClipRect(r.adjusted(-1, -1, 1, 1));
	p.setPen(QPen(accent, 2.0));
	p.drawPath(path);
	p.restore();

	// End of the animation.
	const double xe = toWidget(duration, 0.0).x();
	p.setPen(QPen(accent, 1.0, Qt::DotLine));
	p.drawLine(QPointF(xe, r.top()), QPointF(xe, r.bottom()));

	for (size_t i = 0; i < _keys.size(); i++) {
		const QPointF c = toWidget(_keys[i].time, _keys[i].value);
		const bool sel = (int)i == _selected;
		p.setPen(QPen(sel ? text : accent, 1.5));
		p.setBrush(sel ? accent : pal.color(QPalette::Base));
		p.drawEllipse(c, kHandleRadius + (sel ? 1.0 : 0.0), kHandleRadius + (sel ? 1.0 : 0.0));
	}
}

void CurveView::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;
	const int k = keyAt(event->position());
	if (k >= 0) {
		setSelected(k);
		_drag = k;
	}
}

void CurveView::mouseMoveEvent(QMouseEvent *event)
{
	const QPointF pos = event->position();
	if (_drag < 0) {
		setCursor(keyAt(pos) >= 0 ? Qt::SizeAllCursor : Qt::CrossCursor);
		return;
	}
	an_keyframe &k = _keys[_drag];
	// Snap to 0.01 s and 0.1 %; hold Shift to keep the time, Ctrl the value.
	if (!(event->modifiers() & Qt::ShiftModifier))
		k.time = std::round(std::min(timeAt(pos.x()), _tMax) * 100.0) / 100.0;
	if (!(event->modifiers() & Qt::ControlModifier))
		k.value = std::round(valueAt(pos.y()) * 10.0) / 10.0;
	keysEdited();
}

void CurveView::mouseReleaseEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton || _drag < 0)
		return;
	_drag = -1;
	updateRange();
	update();
}

void CurveView::mouseDoubleClickEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;
	const QPointF pos = event->position();
	const int existing = keyAt(pos);
	if (existing >= 0) {
		setSelected(existing);
		return;
	}
	if ((int)_keys.size() >= AN_MAX_KEYS)
		return;
	an_keyframe k;
	k.time = std::round(timeAt(pos.x()) * 100.0) / 100.0;
	k.value = std::round(valueAt(pos.y()) * 10.0) / 10.0;
	k.ease = AN_EASE_SMOOTH;
	_keys.push_back(k);
	_selected = (int)_keys.size() - 1;
	keysEdited();
	emit selectionChanged(_selected);
}

void CurveView::keyPressEvent(QKeyEvent *event)
{
	if (event->key() == Qt::Key_Delete || event->key() == Qt::Key_Backspace)
		deleteSelected();
	else
		QWidget::keyPressEvent(event);
}

void CurveView::contextMenuEvent(QContextMenuEvent *event)
{
	const int k = keyAt(event->pos());
	if (k < 0)
		return;
	setSelected(k);
	QMenu menu(this);
	if (k > 0) {
		for (int e = 0; e < AN_EASE_COUNT; e++) {
			QAction *a = menu.addAction(QString::fromUtf8(obs_module_text(an_ease_text_key(e))));
			a->setCheckable(true);
			a->setChecked(_keys[k].ease == e);
			connect(a, &QAction::triggered, this, [this, k, e]() {
				if (k < (int)_keys.size()) {
					_keys[k].ease = e;
					keysEdited();
					emit selectionChanged(_selected);
				}
			});
		}
		menu.addSeparator();
	}
	QAction *del = menu.addAction(QString::fromUtf8(obs_module_text("Animate.Curve.Delete")));
	del->setEnabled(_keys.size() > 1);
	connect(del, &QAction::triggered, this, [this]() { deleteSelected(); });
	menu.exec(event->globalPos());
}

/* ------------------------------------------------------------------------
 * CurveEditorDialog
 * --------------------------------------------------------------------- */

CurveEditorDialog::CurveEditorDialog(obs_source_t *filter, QWidget *parent) : QDialog(parent), _filter(filter)
{
	setWindowTitle(QString::fromUtf8(obs_module_text("Animate.CurveEditor.Title")));

	obs_data_t *settings = obs_source_get_settings(filter);
	_original = obs_data_get_string(settings, "an_keys");
	obs_data_release(settings);

	_keys.resize(AN_MAX_KEYS);
	_keys.resize(an_keys_parse(_original.c_str(), _keys.data(), AN_MAX_KEYS));
	if (_keys.empty()) {
		_keys.push_back({0.0, 0.0, AN_EASE_LINEAR});
		_keys.push_back({1.0, 100.0, AN_EASE_SMOOTH});
	}

	_view = new CurveView(_keys, this);

	_time = new QDoubleSpinBox(this);
	_time->setRange(0.0, 3600.0);
	_time->setDecimals(2);
	_time->setSingleStep(0.05);
	_time->setSuffix(" s");
	_value = new QDoubleSpinBox(this);
	_value->setRange(-10000.0, 10000.0);
	_value->setDecimals(1);
	_value->setSuffix(" %");
	_ease = new QComboBox(this);
	for (int e = 0; e < AN_EASE_COUNT; e++)
		_ease->addItem(QString::fromUtf8(obs_module_text(an_ease_text_key(e))), e);
	_delete = new QPushButton(QString::fromUtf8(obs_module_text("Animate.Curve.Delete")), this);

	QHBoxLayout *fields = new QHBoxLayout();
	fields->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Animate.Curve.Time")), this));
	fields->addWidget(_time);
	fields->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Animate.Curve.Value")), this));
	fields->addWidget(_value);
	fields->addWidget(new QLabel(QString::fromUtf8(obs_module_text("Animate.Curve.Ease")), this));
	fields->addWidget(_ease, 1);
	fields->addWidget(_delete);

	_info = new QLabel(QString::fromUtf8(obs_module_text("Animate.Curve.Help")), this);
	_info->setWordWrap(true);

	QDialogButtonBox *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->addWidget(_view, 1);
	layout->addLayout(fields);
	layout->addWidget(_info);
	layout->addWidget(buttons);

	connect(_view, &CurveView::keysChanged, this, [this]() {
		syncFields();
		apply();
	});
	connect(_view, &CurveView::selectionChanged, this, [this](int) { syncFields(); });
	connect(_time, &QDoubleSpinBox::valueChanged, this, [this](double) { applyFields(); });
	connect(_value, &QDoubleSpinBox::valueChanged, this, [this](double) { applyFields(); });
	connect(_ease, &QComboBox::currentIndexChanged, this, [this](int) { applyFields(); });
	connect(_delete, &QPushButton::clicked, this, [this]() { _view->deleteSelected(); });

	syncFields();
	resize(640, 440);
}

CurveEditorDialog::~CurveEditorDialog() {}

void CurveEditorDialog::syncFields()
{
	const int s = _view->selected();
	const bool valid = s >= 0 && s < (int)_keys.size();
	_syncing = true;
	_time->setEnabled(valid);
	_value->setEnabled(valid);
	_ease->setEnabled(valid && s > 0); // the first key has no incoming segment
	_delete->setEnabled(valid && _keys.size() > 1);
	if (valid) {
		_time->setValue(_keys[s].time);
		_value->setValue(_keys[s].value);
		_ease->setCurrentIndex(_ease->findData(_keys[s].ease));
	}
	_syncing = false;
}

void CurveEditorDialog::applyFields()
{
	if (_syncing)
		return;
	const int s = _view->selected();
	if (s < 0 || s >= (int)_keys.size())
		return;
	_keys[s].time = _time->value();
	_keys[s].value = _value->value();
	_keys[s].ease = _ease->currentData().toInt();
	_view->keysEdited();
}

// Writes the keys to the filter so the animation updates while editing.
void CurveEditorDialog::apply()
{
	std::string text(an_keys_format(_keys.data(), _keys.size(), nullptr, 0) + 1, '\0');
	an_keys_format(_keys.data(), _keys.size(), text.data(), text.size());
	text.resize(strlen(text.c_str()));
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "an_keys", text.c_str());
	obs_source_update(_filter, settings);
	obs_data_release(settings);
	_changed = true;
}

void CurveEditorDialog::restoreOriginal()
{
	if (!_changed)
		return;
	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "an_keys", _original.c_str());
	obs_source_update(_filter, settings);
	obs_data_release(settings);
	_changed = false;
}

} // namespace spectrum_animate

extern "C" bool spectrum_curve_editor_open(obs_source_t *filter)
{
	if (!filter)
		return false;
	QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	spectrum_animate::CurveEditorDialog dialog(filter, parent);
	if (dialog.exec() != QDialog::Accepted)
		dialog.restoreOriginal();
	// Refresh the properties so the keyframe text shows the result.
	return true;
}
