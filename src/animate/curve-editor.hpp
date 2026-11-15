#pragma once

// Keyframe curve editor for Spectrum Animate: a graph of the keyframes
// (time across, % of the From -> To range up) with draggable points and a
// per-segment easing. It edits the filter's "an_keys" text live; Cancel
// restores the previous keyframes.

#include <QDialog>
#include <QWidget>

#include <string>
#include <vector>

#include <obs-module.h>

extern "C" {
#include "keyframes.h"
}

class QComboBox;
class QDoubleSpinBox;
class QPushButton;
class QLabel;

namespace spectrum_animate {

class CurveView : public QWidget {
	Q_OBJECT
public:
	explicit CurveView(std::vector<an_keyframe> &keys, QWidget *parent = nullptr);

	int selected() const { return _selected; }
	void setSelected(int index);
	// Call after editing the keys from outside; keeps them sorted and the
	// selection on the same key.
	void keysEdited();
	void deleteSelected();

	QSize sizeHint() const override;
	QSize minimumSizeHint() const override;

signals:
	void keysChanged();
	void selectionChanged(int index);

protected:
	void paintEvent(QPaintEvent *event) override;
	void mousePressEvent(QMouseEvent *event) override;
	void mouseMoveEvent(QMouseEvent *event) override;
	void mouseReleaseEvent(QMouseEvent *event) override;
	void mouseDoubleClickEvent(QMouseEvent *event) override;
	void keyPressEvent(QKeyEvent *event) override;
	void contextMenuEvent(QContextMenuEvent *event) override;

private:
	QRectF plotRect() const;
	void updateRange();
	QPointF toWidget(double time, double value) const;
	double timeAt(double x) const;
	double valueAt(double y) const;
	int keyAt(const QPointF &p) const;
	void resort();

	std::vector<an_keyframe> &_keys;
	int _selected = 0;
	int _drag = -1;
	double _tMax = 2.0;
	double _vMin = -10.0, _vMax = 110.0;
};

class CurveEditorDialog : public QDialog {
	Q_OBJECT
public:
	CurveEditorDialog(obs_source_t *filter, QWidget *parent = nullptr);
	~CurveEditorDialog() override;

	bool changed() const { return _changed; }
	void restoreOriginal();

private:
	void syncFields();
	void applyFields();
	void apply();

	obs_source_t *_filter;
	std::string _original;
	std::vector<an_keyframe> _keys;
	CurveView *_view;
	QDoubleSpinBox *_time;
	QDoubleSpinBox *_value;
	QComboBox *_ease;
	QPushButton *_delete;
	QLabel *_info;
	bool _syncing = false;
	bool _changed = false;
};

} // namespace spectrum_animate
