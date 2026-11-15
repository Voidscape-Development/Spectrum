// "Colors from Source...": picks a source, extracts its dominant colors and
// writes them into the gradient as stops (and as the mesh / point colors
// when the gradient is a mesh or free points).

extern "C" {
#include "gradient.h"
}

#include <obs-frontend-api.h>
#include <obs-module.h>

#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>
#include <QWidget>

#include <cmath>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kCaptureSize = 256;

QString T(const char *id)
{
	return QString::fromUtf8(obs_module_text(id));
}

// The palette as swatches over the gradient it makes.
class PaletteView : public QWidget {
public:
	explicit PaletteView(QWidget *parent = nullptr) : QWidget(parent) { setMinimumSize(360, 72); }

	void setColors(const std::vector<uint32_t> &colors, const std::vector<float> &weights)
	{
		_colors = colors;
		_weights = weights;
		_strip = QImage();
		if (!_colors.empty()) {
			gradient_stop stops[GRADIENT_MAX_STOPS];
			const size_t n =
				gradient_palette_to_stops(_colors.data(), _colors.size(), stops, GRADIENT_MAX_STOPS);
			const int w = 512;
			std::vector<float> baked((size_t)w * 4);
			gradient_bake(stops, n, GRADIENT_INTERP_OKLAB, baked.data(), (size_t)w);
			_strip = QImage(w, 1, QImage::Format_RGBA8888);
			uchar *line = _strip.scanLine(0);
			for (int i = 0; i < w; i++) {
				for (int k = 0; k < 3; k++)
					line[i * 4 + k] = (uchar)std::lround(
						gradient_linear_to_srgb(baked[(size_t)i * 4 + k]) * 255.0f);
				line[i * 4 + 3] = (uchar)std::lround(baked[(size_t)i * 4 + 3] * 255.0f);
			}
		}
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter p(this);
		const QRectF r = QRectF(rect()).adjusted(1, 1, -1, -1);
		p.fillRect(r, palette().color(QPalette::Base));
		if (_colors.empty()) {
			p.setPen(palette().color(QPalette::PlaceholderText));
			p.drawText(r, Qt::AlignCenter, T("Gradient.Palette.Empty"));
			return;
		}
		const double swatch_h = r.height() * 0.55;
		const double cell = r.width() / (double)_colors.size();
		for (size_t i = 0; i < _colors.size(); i++) {
			const uint32_t c = _colors[i];
			const QRectF cr(r.left() + cell * (double)i, r.top(), cell, swatch_h);
			p.fillRect(cr, QColor(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF));
			if (i < _weights.size()) {
				const QString share = QString::number(std::lround(_weights[i] * 100.0f)) + "%";
				const QColor col(c & 0xFF, (c >> 8) & 0xFF, (c >> 16) & 0xFF);
				p.setPen(col.lightnessF() > 0.55 ? Qt::black : Qt::white);
				p.drawText(cr, Qt::AlignCenter, share);
			}
		}
		p.setRenderHint(QPainter::SmoothPixmapTransform);
		p.drawImage(QRectF(r.left(), r.top() + swatch_h + 4.0, r.width(), r.height() - swatch_h - 4.0), _strip);
	}

private:
	std::vector<uint32_t> _colors;
	std::vector<float> _weights;
	QImage _strip;
};

struct SourceList {
	obs_source_t *owner;
	std::vector<std::string> names;
};

bool add_source(void *param, obs_source_t *source)
{
	auto *list = static_cast<SourceList *>(param);
	if (source == list->owner)
		return true;
	if (!(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
		return true;
	const char *name = obs_source_get_name(source);
	if (name && *name)
		list->names.emplace_back(name);
	return true;
}

class PaletteDialog : public QDialog {
public:
	PaletteDialog(gradient_t *g, QWidget *parent) : QDialog(parent), _gradient(g)
	{
		setWindowTitle(T("Gradient.Palette.Title"));
		auto *layout = new QVBoxLayout(this);
		auto *form = new QFormLayout();
		layout->addLayout(form);

		_source = new QComboBox(this);
		SourceList list{g->owner, {}};
		obs_enum_scenes(add_source, &list);
		obs_enum_sources(add_source, &list);
		for (const std::string &name : list.names)
			_source->addItem(QString::fromStdString(name));
		form->addRow(T("Gradient.Palette.Source"), _source);

		_count = new QSpinBox(this);
		_count->setRange(2, 12);
		_count->setValue(5);
		form->addRow(T("Gradient.Palette.Count"), _count);

		_order = new QComboBox(this);
		_order->addItem(T("Gradient.Palette.Order.Lightness"), (int)GRADIENT_PALETTE_LIGHTNESS);
		_order->addItem(T("Gradient.Palette.Order.Hue"), (int)GRADIENT_PALETTE_HUE);
		_order->addItem(T("Gradient.Palette.Order.Prominence"), (int)GRADIENT_PALETTE_PROMINENCE);
		form->addRow(T("Gradient.Palette.Order"), _order);

		_view = new PaletteView(this);
		layout->addWidget(_view, 1);

		auto *hint = new QLabel(T("Gradient.Palette.Hint"), this);
		hint->setWordWrap(true);
		layout->addWidget(hint);

		auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
		auto *refresh = buttons->addButton(T("Gradient.Palette.Refresh"), QDialogButtonBox::ActionRole);
		layout->addWidget(buttons);
		_ok = buttons->button(QDialogButtonBox::Ok);
		connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
		connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
		connect(refresh, &QPushButton::clicked, this, [this]() { capture(); });

		connect(_source, &QComboBox::currentIndexChanged, this, [this](int) { capture(); });
		connect(_count, &QSpinBox::valueChanged, this, [this](int) { extract(); });
		connect(_order, &QComboBox::currentIndexChanged, this, [this](int) { extract(); });
		capture();
	}

	~PaletteDialog() override { bfree(_pixels); }

	const std::vector<uint32_t> &colors() const { return _colors; }

private:
	// Reads a fresh frame of the selected source.
	void capture()
	{
		bfree(_pixels);
		_pixels = nullptr;
		_width = _height = 0;
		const QByteArray name = _source->currentText().toUtf8();
		obs_source_t *source = name.isEmpty() ? nullptr : obs_get_source_by_name(name.constData());
		if (source) {
			_pixels = gradient_palette_capture(source, kCaptureSize, &_width, &_height);
			obs_source_release(source);
		}
		extract();
	}

	void extract()
	{
		_colors.clear();
		std::vector<float> weights;
		if (_pixels) {
			uint32_t colors[16];
			float shares[16];
			const size_t n = gradient_palette_extract(
				_pixels, _width, _height, _width * 4, (size_t)_count->value(),
				(enum gradient_palette_order)_order->currentData().toInt(), colors, shares);
			_colors.assign(colors, colors + n);
			weights.assign(shares, shares + n);
		}
		_view->setColors(_colors, weights);
		_ok->setEnabled(!_colors.empty());
	}

	gradient_t *_gradient;
	QComboBox *_source;
	QSpinBox *_count;
	QComboBox *_order;
	PaletteView *_view;
	QPushButton *_ok;
	uint8_t *_pixels = nullptr;
	uint32_t _width = 0, _height = 0;
	std::vector<uint32_t> _colors;
};

} // namespace

extern "C" void gradient_palette_dialog_open(gradient_t *g)
{
	if (!g || !g->owner)
		return;
	QWidget *parent = static_cast<QWidget *>(obs_frontend_get_main_window());
	PaletteDialog dialog(g, parent);
	if (dialog.exec() != QDialog::Accepted || dialog.colors().empty())
		return;

	// Edit the source's own settings (so the shape and mesh size are
	// known), then let it re-read them.
	obs_data_t *settings = obs_source_get_settings(g->owner);
	gradient_apply_palette(settings, g->prefix, dialog.colors().data(), dialog.colors().size());
	obs_source_update(g->owner, nullptr);
	obs_data_release(settings);
}
