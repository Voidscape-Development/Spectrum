#include "gradient-editor.hpp"

#include <obs-frontend-api.h>
#include <obs-module.h>
#include <util/platform.h>

#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMainWindow>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <string>

namespace {

constexpr double kMargin = 10.0;
constexpr double kMidRow = 14.0;
constexpr double kBarHeight = 40.0;
constexpr double kHandleRow = 24.0;
constexpr double kRemoveDistance = 40.0;

QColor toQColor(uint32_t c)
{
	return QColor(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF, (c >> 24) & 0xFF);
}

uint32_t fromQColor(const QColor &c)
{
	return ((uint32_t)c.alpha() << 24) | ((uint32_t)c.blue() << 16) | ((uint32_t)c.green() << 8) |
	       (uint32_t)c.red();
}

uint32_t hex(const char *rgb, int alpha = 255)
{
	QColor c(rgb);
	c.setAlpha(alpha);
	return fromQColor(c);
}

std::string key(const gradient_t *g, const char *name)
{
	return std::string(g->prefix) + name;
}

struct Preset {
	const char *name;
	enum gradient_interp interp;
	std::vector<gradient_stop> stops;
};

gradient_stop stop(float position, uint32_t color)
{
	return gradient_stop{position, color, 0.5f, GRADIENT_EASE_LINEAR};
}

const std::vector<Preset> &builtinPresets()
{
	static const std::vector<Preset> presets = {
		{"Gradient.Preset.Sunset",
		 GRADIENT_INTERP_OKLAB,
		 {stop(0.0f, hex("#1a1a4e")), stop(0.35f, hex("#6b2d8c")), stop(0.65f, hex("#e2546b")),
		  stop(1.0f, hex("#ffb56b"))}},
		{"Gradient.Preset.Ocean",
		 GRADIENT_INTERP_OKLAB,
		 {stop(0.0f, hex("#03045e")), stop(0.4f, hex("#0077b6")), stop(0.75f, hex("#00b4d8")),
		  stop(1.0f, hex("#90e0ef"))}},
		{"Gradient.Preset.Aurora",
		 GRADIENT_INTERP_OKLAB,
		 {stop(0.0f, hex("#12c2e9")), stop(0.5f, hex("#c471ed")), stop(1.0f, hex("#f64f59"))}},
		{"Gradient.Preset.Fire",
		 GRADIENT_INTERP_OKLAB,
		 {stop(0.0f, hex("#330000")), stop(0.35f, hex("#b31b00")), stop(0.7f, hex("#ff7a00")),
		  stop(1.0f, hex("#ffe066"))}},
		{"Gradient.Preset.Midnight",
		 GRADIENT_INTERP_OKLAB,
		 {stop(0.0f, hex("#0f2027")), stop(0.5f, hex("#203a43")), stop(1.0f, hex("#2c5364"))}},
		{"Gradient.Preset.Neon",
		 GRADIENT_INTERP_OKLCH_SHORTER,
		 {stop(0.0f, hex("#ff00cc")), stop(1.0f, hex("#3333ff"))}},
		{"Gradient.Preset.Pastel",
		 GRADIENT_INTERP_OKLAB,
		 {stop(0.0f, hex("#ffd1dc")), stop(0.5f, hex("#c1e1ff")), stop(1.0f, hex("#d4f8d4"))}},
		{"Gradient.Preset.Rainbow",
		 GRADIENT_INTERP_OKLCH_LONGER,
		 {stop(0.0f, hex("#ff3b3b")), stop(1.0f, hex("#ff3b3b"))}},
		{"Gradient.Preset.FadeBlack",
		 GRADIENT_INTERP_OKLAB,
		 {stop(0.0f, hex("#000000", 255)), stop(1.0f, hex("#000000", 0))}},
		{"Gradient.Preset.FadeWhite",
		 GRADIENT_INTERP_OKLAB,
		 {stop(0.0f, hex("#ffffff", 255)), stop(1.0f, hex("#ffffff", 0))}},
	};
	return presets;
}

std::string presetsPath()
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}
	char *path = obs_module_config_path("gradient-presets.json");
	std::string result = path ? path : "";
	bfree(path);
	return result;
}

obs_data_t *loadUserPresets()
{
	std::string path = presetsPath();
	obs_data_t *data = path.empty() ? nullptr : obs_data_create_from_json_file_safe(path.c_str(), "bak");
	return data ? data : obs_data_create();
}

void saveUserPresets(obs_data_t *data)
{
	std::string path = presetsPath();
	if (!path.empty())
		obs_data_save_json_safe(data, path.c_str(), "tmp", "bak");
}

void paintChecker(QPainter &p, const QRectF &rect)
{
	p.save();
	p.setClipRect(rect);
	const int cell = 6;
	p.fillRect(rect, QColor(200, 200, 200));
	for (int y = 0; y * cell < rect.height(); y++) {
		for (int x = 0; x * cell < rect.width(); x++) {
			if ((x + y) % 2 == 0)
				p.fillRect(QRectF(rect.left() + x * cell, rect.top() + y * cell, cell, cell),
					   QColor(255, 255, 255));
		}
	}
	p.restore();
}

} // namespace

/* ------------------------------------------------------------------------
 * GradientBar
 * --------------------------------------------------------------------- */

GradientBar::GradientBar(std::vector<gradient_stop> &stops, QWidget *parent) : QWidget(parent), _stops(stops)
{
	setFocusPolicy(Qt::StrongFocus);
	setMouseTracking(true);
	setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	setToolTip(QString::fromUtf8(obs_module_text("Gradient.Editor.BarTip")));
}

void GradientBar::setInterpolation(enum gradient_interp interp)
{
	_interp = interp;
	update();
}

void GradientBar::setSelected(int index)
{
	if (_stops.empty())
		index = -1;
	else
		index = std::clamp(index, 0, (int)_stops.size() - 1);
	if (index != _selected) {
		_selected = index;
		emit selectionChanged(index);
	}
	update();
}

uint32_t GradientBar::colorAt(float t) const
{
	// Bake a short table and pick the nearest sample.
	const size_t n = 1024;
	std::vector<float> lut(n * 4);
	gradient_bake(_stops.data(), _stops.size(), _interp, lut.data(), n);
	const size_t i = (size_t)std::lround(std::clamp(t, 0.0f, 1.0f) * (float)(n - 1));
	const auto enc = [&](int c) {
		return (int)std::lround(gradient_linear_to_srgb(lut[i * 4 + c]) * 255.0f);
	};
	QColor c(enc(0), enc(1), enc(2), (int)std::lround(lut[i * 4 + 3] * 255.0f));
	return fromQColor(c);
}

QSize GradientBar::sizeHint() const
{
	return QSize(520, (int)(kMidRow + kBarHeight + kHandleRow + 4));
}

QSize GradientBar::minimumSizeHint() const
{
	return QSize(200, (int)(kMidRow + kBarHeight + kHandleRow + 4));
}

QRectF GradientBar::barRect() const
{
	return QRectF(kMargin, kMidRow, width() - 2 * kMargin, kBarHeight);
}

float GradientBar::positionAt(double x) const
{
	const QRectF bar = barRect();
	return (float)std::clamp((x - bar.left()) / bar.width(), 0.0, 1.0);
}

double GradientBar::xAt(float position) const
{
	const QRectF bar = barRect();
	return bar.left() + position * bar.width();
}

int GradientBar::nextStop(int index) const
{
	int best = -1;
	const float p = _stops[index].position;
	for (int i = 0; i < (int)_stops.size(); i++) {
		if (i == index)
			continue;
		const float q = _stops[i].position;
		// Ties are ordered by index, as the (stable enough) bake sort.
		const bool after = q > p || (q == p && i > index);
		if (after && (best < 0 || q < _stops[best].position))
			best = i;
	}
	return best;
}

int GradientBar::stopAt(const QPointF &p) const
{
	const QRectF bar = barRect();
	if (p.y() < bar.bottom() - 4 || p.y() > bar.bottom() + kHandleRow)
		return -1;
	int best = -1;
	double bestDistance = 8.0;
	// Prefer the selected stop when stops overlap.
	for (int i = 0; i < (int)_stops.size(); i++) {
		const double d = std::abs(xAt(_stops[i].position) - p.x());
		if (d < bestDistance || (d <= bestDistance && i == _selected)) {
			best = i;
			bestDistance = d;
		}
	}
	return best;
}

int GradientBar::midpointAt(const QPointF &p) const
{
	if (p.y() > barRect().top())
		return -1;
	for (int i = 0; i < (int)_stops.size(); i++) {
		const int j = nextStop(i);
		if (j < 0)
			continue;
		const float p0 = _stops[i].position, p1 = _stops[j].position;
		if (p1 - p0 < 1e-4f)
			continue;
		const double x = xAt(p0 + _stops[i].midpoint * (p1 - p0));
		if (std::abs(x - p.x()) <= 6.0)
			return i;
	}
	return -1;
}

void GradientBar::paintEvent(QPaintEvent *)
{
	QPainter p(this);
	p.setRenderHint(QPainter::Antialiasing);
	const QRectF bar = barRect();

	paintChecker(p, bar);

	// Bake one sample per device pixel and ordered-dither it to 8 bits, so
	// the preview itself doesn't band.
	const qreal dpr = devicePixelRatioF();
	const int w = std::max(2, (int)std::lround(bar.width() * dpr));
	const int h = 4;
	std::vector<float> lut((size_t)w * 4);
	gradient_bake(_stops.data(), _stops.size(), _interp, lut.data(), (size_t)w);
	static const float bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
	QImage image(w, h, QImage::Format_RGBA8888);
	for (int y = 0; y < h; y++) {
		uchar *line = image.scanLine(y);
		for (int x = 0; x < w; x++) {
			const float d = (bayer[y & 3][x & 3] + 0.5f) / 16.0f - 0.5f;
			const float *c = &lut[(size_t)x * 4];
			for (int k = 0; k < 3; k++) {
				const float v = gradient_linear_to_srgb(c[k]) * 255.0f + d;
				line[x * 4 + k] = (uchar)std::clamp((int)std::lround(v), 0, 255);
			}
			line[x * 4 + 3] = (uchar)std::clamp((int)std::lround(c[3] * 255.0f + d), 0, 255);
		}
	}
	p.save();
	p.setRenderHint(QPainter::SmoothPixmapTransform, false);
	p.drawImage(bar, image);
	p.restore();

	p.setPen(QPen(palette().color(QPalette::Mid), 1.0));
	p.setBrush(Qt::NoBrush);
	p.drawRect(bar.adjusted(-0.5, -0.5, 0.5, 0.5));

	// Midpoint diamonds.
	for (int i = 0; i < (int)_stops.size(); i++) {
		const int j = nextStop(i);
		if (j < 0)
			continue;
		const float p0 = _stops[i].position, p1 = _stops[j].position;
		if (p1 - p0 < 1e-4f)
			continue;
		const double x = xAt(p0 + _stops[i].midpoint * (p1 - p0));
		const double y = kMidRow / 2.0;
		QPainterPath diamond;
		diamond.moveTo(x, y - 4);
		diamond.lineTo(x + 4, y);
		diamond.lineTo(x, y + 4);
		diamond.lineTo(x - 4, y);
		diamond.closeSubpath();
		const bool active = i == _dragMidpoint || i == _selected;
		p.setPen(QPen(palette().color(QPalette::WindowText), 1.0));
		p.setBrush(active ? palette().color(QPalette::Highlight) : palette().color(QPalette::Window));
		p.drawPath(diamond);
	}

	// Stop handles, the selected one last (on top).
	std::vector<int> order;
	for (int i = 0; i < (int)_stops.size(); i++)
		if (i != _selected)
			order.push_back(i);
	if (_selected >= 0)
		order.push_back(_selected);

	for (int i : order) {
		const double x = xAt(_stops[i].position);
		const double top = bar.bottom() + 2;
		const bool sel = i == _selected;
		const bool removing = sel && _dragRemoving;
		p.setOpacity(removing ? 0.3 : 1.0);

		QPainterPath arrow;
		arrow.moveTo(x, top);
		arrow.lineTo(x + 6, top + 6);
		arrow.lineTo(x - 6, top + 6);
		arrow.closeSubpath();
		const QColor outline = sel ? palette().color(QPalette::Highlight)
					   : palette().color(QPalette::WindowText);
		p.setPen(QPen(outline, sel ? 2.0 : 1.0));
		p.setBrush(outline);
		p.drawPath(arrow);

		const QRectF swatch(x - 7, top + 6, 14, 14);
		paintChecker(p, swatch);
		p.fillRect(swatch, toQColor(_stops[i].color));
		p.setBrush(Qt::NoBrush);
		p.drawRect(swatch);
		p.setOpacity(1.0);
	}
}

void GradientBar::mousePressEvent(QMouseEvent *event)
{
	if (event->button() != Qt::LeftButton)
		return;
	const QPointF pos = event->position();
	_dragRemoving = false;

	const int s = stopAt(pos);
	if (s >= 0) {
		setSelected(s);
		_dragStop = s;
		return;
	}

	const int m = midpointAt(pos);
	if (m >= 0) {
		setSelected(m);
		_dragMidpoint = m;
		update();
		return;
	}

	const QRectF bar = barRect();
	if (pos.y() >= bar.top() && pos.y() <= bar.bottom() + kHandleRow && _stops.size() < GRADIENT_MAX_STOPS) {
		const float t = positionAt(pos.x());
		gradient_stop stop{t, colorAt(t), 0.5f, GRADIENT_EASE_LINEAR};
		_stops.push_back(stop);
		_dragStop = (int)_stops.size() - 1;
		setSelected(_dragStop);
		emit stopsChanged();
	}
}

void GradientBar::mouseMoveEvent(QMouseEvent *event)
{
	const QPointF pos = event->position();
	if (_dragStop >= 0) {
		const bool removing = _stops.size() > 2 && pos.y() > barRect().bottom() + kHandleRow + kRemoveDistance;
		if (removing != _dragRemoving) {
			_dragRemoving = removing;
			update();
		}
		if (!removing) {
			_stops[_dragStop].position = positionAt(pos.x());
			emit stopsChanged();
			emit selectionChanged(_selected);
			update();
		}
	} else if (_dragMidpoint >= 0) {
		const int j = nextStop(_dragMidpoint);
		if (j >= 0) {
			const float p0 = _stops[_dragMidpoint].position, p1 = _stops[j].position;
			if (p1 - p0 > 1e-4f) {
				const float m = (positionAt(pos.x()) - p0) / (p1 - p0);
				_stops[_dragMidpoint].midpoint = std::clamp(m, 0.01f, 0.99f);
				emit stopsChanged();
				emit selectionChanged(_selected);
				update();
			}
		}
	} else {
		const bool handle = stopAt(pos) >= 0 || midpointAt(pos) >= 0;
		setCursor(handle ? Qt::SizeHorCursor : Qt::ArrowCursor);
	}
}

void GradientBar::mouseReleaseEvent(QMouseEvent *)
{
	if (_dragStop >= 0 && _dragRemoving && _stops.size() > 2) {
		_stops.erase(_stops.begin() + _dragStop);
		_selected = -1;
		setSelected(std::min(_dragStop, (int)_stops.size() - 1));
		emit stopsChanged();
	}
	_dragStop = -1;
	_dragMidpoint = -1;
	_dragRemoving = false;
	update();
}

void GradientBar::mouseDoubleClickEvent(QMouseEvent *event)
{
	const int s = stopAt(event->position());
	if (s >= 0) {
		_dragStop = -1;
		emit editColorRequested(s);
	}
}

void GradientBar::keyPressEvent(QKeyEvent *event)
{
	if (_selected < 0) {
		QWidget::keyPressEvent(event);
		return;
	}
	const float step = (event->modifiers() & Qt::ShiftModifier) ? 0.001f : 0.01f;
	switch (event->key()) {
	case Qt::Key_Delete:
	case Qt::Key_Backspace:
		if (_stops.size() > 2) {
			const int index = _selected;
			_stops.erase(_stops.begin() + index);
			_selected = -1;
			setSelected(std::min(index, (int)_stops.size() - 1));
			emit stopsChanged();
		}
		return;
	case Qt::Key_Left:
	case Qt::Key_Right: {
		float &p = _stops[_selected].position;
		p = std::clamp(p + (event->key() == Qt::Key_Left ? -step : step), 0.0f, 1.0f);
		emit stopsChanged();
		emit selectionChanged(_selected);
		update();
		return;
	}
	default:
		QWidget::keyPressEvent(event);
	}
}

/* ------------------------------------------------------------------------
 * GradientEditorDialog
 * --------------------------------------------------------------------- */

static QString T(const char *id)
{
	return QString::fromUtf8(obs_module_text(id));
}

GradientEditorDialog::GradientEditorDialog(gradient_t *gradient, QWidget *parent) : QDialog(parent), _gradient(gradient)
{
	setWindowTitle(T("Gradient.Editor.Title"));
	setModal(true);

	obs_data_t *settings = obs_source_get_settings(gradient->owner);
	gradient_stop buffer[GRADIENT_MAX_STOPS];
	const size_t count = gradient_read_stops(settings, gradient->prefix, buffer, GRADIENT_MAX_STOPS);
	_stops.assign(buffer, buffer + count);
	_originalStops = _stops;
	_originalInterp = obs_data_get_int(settings, key(gradient, "interp").c_str());
	obs_data_release(settings);

	auto *layout = new QVBoxLayout(this);

	_bar = new GradientBar(_stops, this);
	_bar->setInterpolation((enum gradient_interp)_originalInterp);
	layout->addWidget(_bar);

	auto *hint = new QLabel(T("Gradient.Editor.Hint"), this);
	hint->setWordWrap(true);
	QFont small = hint->font();
	small.setPointSizeF(small.pointSizeF() * 0.9);
	hint->setFont(small);
	layout->addWidget(hint);

	/* Selected stop */
	auto *form = new QFormLayout();

	auto *colorRow = new QHBoxLayout();
	_colorButton = new QPushButton(this);
	_colorButton->setMinimumWidth(120);
	colorRow->addWidget(_colorButton);
	_deleteButton = new QPushButton(T("Gradient.Editor.Delete"), this);
	colorRow->addWidget(_deleteButton);
	colorRow->addStretch();
	form->addRow(T("Gradient.Editor.Color"), colorRow);

	_position = new QDoubleSpinBox(this);
	_position->setRange(0.0, 100.0);
	_position->setDecimals(1);
	_position->setSuffix("%");
	form->addRow(T("Gradient.Editor.Position"), _position);

	_midpoint = new QDoubleSpinBox(this);
	_midpoint->setRange(1.0, 99.0);
	_midpoint->setDecimals(1);
	_midpoint->setSuffix("%");
	_midpoint->setToolTip(T("Gradient.Editor.MidpointTip"));
	form->addRow(T("Gradient.Editor.Midpoint"), _midpoint);

	_ease = new QComboBox(this);
	_ease->addItem(T("Gradient.Ease.Linear"), GRADIENT_EASE_LINEAR);
	_ease->addItem(T("Gradient.Ease.Smooth"), GRADIENT_EASE_SMOOTH);
	_ease->addItem(T("Gradient.Ease.In"), GRADIENT_EASE_IN);
	_ease->addItem(T("Gradient.Ease.Out"), GRADIENT_EASE_OUT);
	_ease->addItem(T("Gradient.Ease.InOut"), GRADIENT_EASE_IN_OUT);
	_ease->addItem(T("Gradient.Ease.Step"), GRADIENT_EASE_STEP);
	_ease->setToolTip(T("Gradient.Editor.EaseTip"));
	form->addRow(T("Gradient.Editor.Ease"), _ease);

	_interp = new QComboBox(this);
	_interp->addItem(T("Gradient.Interpolation.Oklab"), GRADIENT_INTERP_OKLAB);
	_interp->addItem(T("Gradient.Interpolation.OklchShorter"), GRADIENT_INTERP_OKLCH_SHORTER);
	_interp->addItem(T("Gradient.Interpolation.OklchLonger"), GRADIENT_INTERP_OKLCH_LONGER);
	_interp->addItem(T("Gradient.Interpolation.OklchIncreasing"), GRADIENT_INTERP_OKLCH_INCREASING);
	_interp->addItem(T("Gradient.Interpolation.OklchDecreasing"), GRADIENT_INTERP_OKLCH_DECREASING);
	_interp->addItem(T("Gradient.Interpolation.LinearRgb"), GRADIENT_INTERP_LINEAR_RGB);
	_interp->addItem(T("Gradient.Interpolation.Srgb"), GRADIENT_INTERP_SRGB);
	_interp->setCurrentIndex(std::max(0, _interp->findData((int)_originalInterp)));
	form->addRow(T("Gradient.Interpolation"), _interp);

	layout->addLayout(form);

	/* Tools */
	auto *tools = new QHBoxLayout();
	auto *reverse = new QPushButton(T("Gradient.Editor.Reverse"), this);
	auto *distribute = new QPushButton(T("Gradient.Editor.Distribute"), this);
	tools->addWidget(reverse);
	tools->addWidget(distribute);
	tools->addStretch();
	layout->addLayout(tools);

	auto *presetRow = new QHBoxLayout();
	_presets = new QComboBox(this);
	_presets->setMinimumWidth(200);
	auto *savePreset = new QPushButton(T("Gradient.Editor.SavePreset"), this);
	auto *deletePreset = new QPushButton(T("Gradient.Editor.DeletePreset"), this);
	deletePreset->setEnabled(false);
	presetRow->addWidget(new QLabel(T("Gradient.Editor.Presets"), this));
	presetRow->addWidget(_presets, 1);
	presetRow->addWidget(savePreset);
	presetRow->addWidget(deletePreset);
	layout->addLayout(presetRow);
	loadPresetList();

	auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
	layout->addStretch(1);
	layout->addWidget(buttons);

	/* Wiring */
	connect(_bar, &GradientBar::stopsChanged, this, [this]() { apply(); });
	connect(_bar, &GradientBar::selectionChanged, this, [this](int) { updateStopControls(); });
	connect(_bar, &GradientBar::editColorRequested, this, [this](int index) { editColor(index); });
	connect(_colorButton, &QPushButton::clicked, this, [this]() { editColor(_bar->selected()); });

	connect(_deleteButton, &QPushButton::clicked, this, [this]() {
		const int index = _bar->selected();
		if (index < 0 || _stops.size() <= 2)
			return;
		_stops.erase(_stops.begin() + index);
		_bar->setSelected(std::min(index, (int)_stops.size() - 1));
		updateStopControls();
		apply();
	});

	connect(_position, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		const int index = _bar->selected();
		if (_updatingControls || index < 0)
			return;
		_stops[index].position = (float)(v / 100.0);
		apply();
	});
	connect(_midpoint, &QDoubleSpinBox::valueChanged, this, [this](double v) {
		const int index = _bar->selected();
		if (_updatingControls || index < 0)
			return;
		_stops[index].midpoint = (float)(v / 100.0);
		apply();
	});
	connect(_ease, &QComboBox::currentIndexChanged, this, [this](int) {
		const int index = _bar->selected();
		if (_updatingControls || index < 0)
			return;
		_stops[index].ease = (enum gradient_ease)_ease->currentData().toInt();
		apply();
	});
	connect(_interp, &QComboBox::currentIndexChanged, this, [this](int) {
		_bar->setInterpolation((enum gradient_interp)_interp->currentData().toInt());
		apply();
	});

	connect(reverse, &QPushButton::clicked, this, [this]() {
		// Reverse positions, and move each segment's midpoint and easing
		// to the stop that now starts it.
		std::vector<gradient_stop> sorted = _stops;
		std::stable_sort(sorted.begin(), sorted.end(), [](const gradient_stop &a, const gradient_stop &b) {
			return a.position < b.position;
		});
		std::vector<gradient_stop> reversed;
		for (size_t i = sorted.size(); i-- > 0;) {
			gradient_stop s = sorted[i];
			s.position = 1.0f - s.position;
			if (i > 0) {
				s.midpoint = 1.0f - sorted[i - 1].midpoint;
				switch (sorted[i - 1].ease) {
				case GRADIENT_EASE_IN:
					s.ease = GRADIENT_EASE_OUT;
					break;
				case GRADIENT_EASE_OUT:
					s.ease = GRADIENT_EASE_IN;
					break;
				default:
					s.ease = sorted[i - 1].ease;
				}
			} else {
				s.midpoint = 0.5f;
				s.ease = GRADIENT_EASE_LINEAR;
			}
			reversed.push_back(s);
		}
		_stops = reversed;
		_bar->setSelected(0);
		updateStopControls();
		_bar->update();
		apply();
	});

	connect(distribute, &QPushButton::clicked, this, [this]() {
		std::vector<int> order(_stops.size());
		for (size_t i = 0; i < order.size(); i++)
			order[i] = (int)i;
		std::stable_sort(order.begin(), order.end(),
				 [this](int a, int b) { return _stops[a].position < _stops[b].position; });
		for (size_t i = 0; i < order.size(); i++)
			_stops[order[i]].position = order.size() > 1 ? (float)i / (float)(order.size() - 1) : 0.0f;
		updateStopControls();
		_bar->update();
		apply();
	});

	connect(_presets, &QComboBox::activated, this, [this, deletePreset](int row) {
		const QString id = _presets->itemData(row).toString();
		deletePreset->setEnabled(id.startsWith("user:"));
		if (id.startsWith("builtin:")) {
			const Preset &preset = builtinPresets()[(size_t)id.mid(8).toInt()];
			_stops = preset.stops;
			_interp->setCurrentIndex(std::max(0, _interp->findData((int)preset.interp)));
		} else if (id.startsWith("user:")) {
			const std::string name = id.mid(5).toStdString();
			obs_data_t *root = loadUserPresets();
			obs_data_array_t *list = obs_data_get_array(root, "presets");
			for (size_t i = 0; list && i < obs_data_array_count(list); i++) {
				obs_data_t *item = obs_data_array_item(list, i);
				if (name == obs_data_get_string(item, "name")) {
					gradient_stop buf[GRADIENT_MAX_STOPS];
					const size_t n = gradient_read_stops(item, "", buf, GRADIENT_MAX_STOPS);
					_stops.assign(buf, buf + n);
					_interp->setCurrentIndex(
						std::max(0, _interp->findData((int)obs_data_get_int(item, "interp"))));
				}
				obs_data_release(item);
			}
			obs_data_array_release(list);
			obs_data_release(root);
		} else {
			return;
		}
		_bar->setSelected(0);
		updateStopControls();
		_bar->update();
		apply();
	});

	connect(savePreset, &QPushButton::clicked, this, [this]() {
		bool ok = false;
		const QString name = QInputDialog::getText(this, T("Gradient.Editor.SavePreset"),
							   T("Gradient.Editor.PresetName"), QLineEdit::Normal,
							   QString(), &ok)
					     .trimmed();
		if (!ok || name.isEmpty())
			return;
		obs_data_t *root = loadUserPresets();
		obs_data_array_t *list = obs_data_get_array(root, "presets");
		if (!list)
			list = obs_data_array_create();
		// Replace a preset with the same name.
		for (size_t i = obs_data_array_count(list); i-- > 0;) {
			obs_data_t *item = obs_data_array_item(list, i);
			if (name.toStdString() == obs_data_get_string(item, "name"))
				obs_data_array_erase(list, i);
			obs_data_release(item);
		}
		obs_data_t *item = obs_data_create();
		obs_data_set_string(item, "name", name.toUtf8().constData());
		obs_data_set_int(item, "interp", _interp->currentData().toInt());
		gradient_write_stops(item, "", _stops.data(), _stops.size());
		obs_data_array_push_back(list, item);
		obs_data_release(item);
		obs_data_set_array(root, "presets", list);
		obs_data_array_release(list);
		saveUserPresets(root);
		obs_data_release(root);
		loadPresetList();
		_presets->setCurrentIndex(_presets->findData("user:" + name));
	});

	connect(deletePreset, &QPushButton::clicked, this, [this, deletePreset]() {
		const QString id = _presets->currentData().toString();
		if (!id.startsWith("user:"))
			return;
		const std::string name = id.mid(5).toStdString();
		obs_data_t *root = loadUserPresets();
		obs_data_array_t *list = obs_data_get_array(root, "presets");
		for (size_t i = list ? obs_data_array_count(list) : 0; i-- > 0;) {
			obs_data_t *item = obs_data_array_item(list, i);
			if (name == obs_data_get_string(item, "name"))
				obs_data_array_erase(list, i);
			obs_data_release(item);
		}
		obs_data_array_release(list);
		saveUserPresets(root);
		obs_data_release(root);
		loadPresetList();
		deletePreset->setEnabled(false);
	});

	connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

	_bar->setSelected(0);
	updateStopControls();
}

void GradientEditorDialog::loadPresetList()
{
	_presets->clear();
	_presets->addItem(T("Gradient.Editor.ChoosePreset"), QString());
	const auto &builtins = builtinPresets();
	for (size_t i = 0; i < builtins.size(); i++)
		_presets->addItem(T(builtins[i].name), QString("builtin:%1").arg(i));

	obs_data_t *root = loadUserPresets();
	obs_data_array_t *list = obs_data_get_array(root, "presets");
	const size_t count = list ? obs_data_array_count(list) : 0;
	if (count > 0)
		_presets->insertSeparator(_presets->count());
	for (size_t i = 0; i < count; i++) {
		obs_data_t *item = obs_data_array_item(list, i);
		const QString name = QString::fromUtf8(obs_data_get_string(item, "name"));
		_presets->addItem(name, "user:" + name);
		obs_data_release(item);
	}
	obs_data_array_release(list);
	obs_data_release(root);
}

void GradientEditorDialog::updateStopControls()
{
	const int index = _bar->selected();
	const bool valid = index >= 0 && index < (int)_stops.size();
	_updatingControls = true;
	_colorButton->setEnabled(valid);
	_position->setEnabled(valid);
	_midpoint->setEnabled(valid);
	_ease->setEnabled(valid);
	_deleteButton->setEnabled(valid && _stops.size() > 2);
	if (valid) {
		const gradient_stop &s = _stops[index];
		const QColor c = toQColor(s.color);
		QString label = c.name(QColor::HexRgb).toUpper();
		if (c.alpha() < 255)
			label += QString("  %1%").arg(std::lround(c.alphaF() * 100.0));
		_colorButton->setText(label);
		const QColor text = c.lightnessF() > 0.55 || c.alphaF() < 0.5 ? Qt::black : Qt::white;
		_colorButton->setStyleSheet(QString("QPushButton { background-color: %1; color: %2; }")
						    .arg(c.name(QColor::HexArgb), text.name()));
		_position->setValue(s.position * 100.0);
		_midpoint->setValue(s.midpoint * 100.0);
		_ease->setCurrentIndex(std::max(0, _ease->findData((int)s.ease)));
	}
	_updatingControls = false;
}

void GradientEditorDialog::editColor(int index)
{
	if (index < 0 || index >= (int)_stops.size())
		return;
	const uint32_t previous = _stops[index].color;

	QColorDialog dialog(toQColor(previous), this);
	dialog.setOption(QColorDialog::ShowAlphaChannel);
	dialog.setWindowTitle(T("Gradient.Editor.Color"));
	connect(&dialog, &QColorDialog::currentColorChanged, this, [this, index](const QColor &c) {
		if (!c.isValid())
			return;
		_stops[index].color = fromQColor(c);
		updateStopControls();
		_bar->update();
		apply();
	});

	if (dialog.exec() == QDialog::Accepted)
		_stops[index].color = fromQColor(dialog.selectedColor());
	else
		_stops[index].color = previous;
	updateStopControls();
	_bar->update();
	apply();
}

void GradientEditorDialog::apply()
{
	obs_data_t *data = obs_data_create();
	gradient_write_stops(data, _gradient->prefix, _stops.data(), _stops.size());
	obs_data_set_int(data, key(_gradient, "interp").c_str(), _interp->currentData().toInt());
	obs_source_update(_gradient->owner, data);
	obs_data_release(data);
}

void GradientEditorDialog::restore()
{
	obs_data_t *data = obs_data_create();
	gradient_write_stops(data, _gradient->prefix, _originalStops.data(), _originalStops.size());
	obs_data_set_int(data, key(_gradient, "interp").c_str(), _originalInterp);
	obs_source_update(_gradient->owner, data);
	obs_data_release(data);
}

extern "C" void gradient_editor_open(gradient_t *g)
{
	if (!g || !g->owner)
		return;
	QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	GradientEditorDialog dialog(g, parent);
	if (dialog.exec() == QDialog::Accepted)
		dialog.apply();
	else
		dialog.restore();
}
