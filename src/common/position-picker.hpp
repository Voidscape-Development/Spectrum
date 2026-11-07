#pragma once

// Position Picker: a dialog showing a live snapshot of a source (for a
// filter, its parent with all filters applied) with draggable handles for
// the positional settings of Spectrum's filters and sources: centers, light
// positions, angles and offsets.
//
// OBS has no supported API for drawing handles on its own preview, so the
// handles live in this dialog instead.

#include <obs-module.h>

#ifdef __cplusplus
extern "C" {
#endif

// Adds a "Position Picker..." button if the filter/source type has handles.
void spectrum_position_picker_properties(obs_properties_t *props, obs_source_t *source);

#ifdef __cplusplus
}

#include <QDialog>
#include <QImage>
#include <QTimer>
#include <QWidget>

#include <string>
#include <vector>

namespace spectrum_picker {

enum class HandleType {
	PointPercent, // key_x / key_y: % of the width / height
	PointOffset,  // key_x / key_y: px from the centre
	Angle,        // key_angle: degrees around an anchor
	Polar,        // key_angle + key_distance (px) from the centre
};

struct HandleSpec {
	HandleType type;
	const char *label; // locale key
	const char *key_x;
	const char *key_y;
	const char *key_angle;
	const char *key_distance;
	int anchor; // Angle: index of a point handle, or -1 for the centre
	bool y_up;  // Angle: counter-clockwise (light azimuths)
	double max_distance;
	const char *cond_key; // shown only when this setting is set (bool) or
	int cond_values[4];   // has one of these values (int, 0 terminated)
};

struct Handle {
	const HandleSpec *spec;
	bool visible;
	QPointF pos; // source pixels
};

class PickerCanvas : public QWidget {
	Q_OBJECT
public:
	PickerCanvas(obs_source_t *target, std::vector<Handle> handles, QWidget *parent = nullptr);
	~PickerCanvas() override;

	QSize sizeHint() const override;
	void refreshSnapshot();
	void restoreOriginal();

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;

private:
	QRectF imageRect() const;
	QPointF toWidget(const QPointF &source) const;
	QPointF toSource(const QPointF &widget) const;
	QPointF handleWidgetPos(int index) const;
	QPointF anchorWidgetPos(const Handle &h) const;
	void loadHandles();
	void applyHandle(int index, const QPointF &source_pos);

	obs_source_t *_target;   // the filter/source whose settings we edit
	obs_source_t *_rendered; // what the snapshot shows
	obs_data_t *_original;
	std::vector<Handle> _handles;
	QImage _image;
	int _sourceWidth = 0;
	int _sourceHeight = 0;
	int _drag = -1;

	gs_texrender_t *_texrender = nullptr;
	gs_stagesurf_t *_stage = nullptr;
	uint32_t _stageW = 0, _stageH = 0;
};

class PositionPickerDialog : public QDialog {
	Q_OBJECT
public:
	PositionPickerDialog(obs_source_t *target, std::vector<Handle> handles, QWidget *parent = nullptr);
	PickerCanvas *canvas() const { return _canvas; }

private:
	PickerCanvas *_canvas;
	QTimer _timer;
};

} // namespace spectrum_picker

#endif
