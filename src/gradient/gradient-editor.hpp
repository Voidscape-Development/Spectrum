#pragma once

#include <vector>

#include <QDialog>
#include <QWidget>

extern "C" {
#include "gradient.h"
}

class QComboBox;
class QDoubleSpinBox;
class QPushButton;

// The gradient preview bar with draggable stops (below) and midpoints
// (above).
class GradientBar : public QWidget {
	Q_OBJECT
public:
	explicit GradientBar(std::vector<gradient_stop> &stops, QWidget *parent = nullptr);

	void setInterpolation(enum gradient_interp interp);
	int selected() const { return _selected; }
	void setSelected(int index);
	// The baked color at position t, as an OBS color.
	uint32_t colorAt(float t) const;

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

signals:
	void stopsChanged();
	void selectionChanged(int index);
	void editColorRequested(int index);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;

private:
	QRectF barRect() const;
	float positionAt(double x) const;
	double xAt(float position) const;
	int stopAt(const QPointF &p) const;
	// Returns the stop index whose segment midpoint is under p.
	int midpointAt(const QPointF &p) const;
	// Index of the stop that follows `index` in position order, or -1.
	int nextStop(int index) const;

	std::vector<gradient_stop> &_stops;
	enum gradient_interp _interp = GRADIENT_INTERP_OKLAB;
	int _selected = 0;
	int _dragStop = -1;
	int _dragMidpoint = -1;
	bool _dragRemoving = false;
};

class GradientEditorDialog : public QDialog {
	Q_OBJECT
public:
	GradientEditorDialog(gradient_t *gradient, QWidget *parent = nullptr);

	// Applies the stops (and interpolation) to the source, for live preview.
	void apply();
	void restore();

private:
	void updateStopControls();
	void loadPresetList();
	void editColor(int index);

	gradient_t *_gradient;
	std::vector<gradient_stop> _stops;
	std::vector<gradient_stop> _originalStops;
	long long _originalInterp;

	GradientBar *_bar;
	QComboBox *_interp;
	QPushButton *_colorButton;
	QDoubleSpinBox *_position;
	QDoubleSpinBox *_midpoint;
	QComboBox *_ease;
	QPushButton *_deleteButton;
	QComboBox *_presets;
	bool _updatingControls = false;
};
