// Mouse position for Spectrum Animate's "Mouse Position" driver.
//
// QCursor and QScreen may only be used on the UI thread, while the filter
// reads the position from the video thread. So the video thread queues a UI
// task (at most one in flight) that reads the cursor and the screen layout
// into a mutex-protected snapshot, and reads the latest snapshot itself.
// Nothing persistent is created on the Qt side, so there is nothing to tear
// down when the plugin unloads.

#include "animate-ui.h"

#include <QCoreApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QRect>
#include <QScreen>
#include <QThread>

#include <atomic>
#include <mutex>
#include <vector>

namespace {

struct CursorSnapshot {
	bool valid = false;
	QPoint pos;
	QRect primary;
	QRect desktop;
	std::vector<QRect> screens;
};

std::mutex snapshot_mutex;
CursorSnapshot snapshot;
std::atomic<bool> task_pending{false};

QGuiApplication *gui_app()
{
	return qobject_cast<QGuiApplication *>(QCoreApplication::instance());
}

void read_cursor_task(void *)
{
	CursorSnapshot s;
	if (gui_app()) {
		s.pos = QCursor::pos();
		QScreen *primary = QGuiApplication::primaryScreen();
		if (primary)
			s.primary = primary->geometry();
		for (QScreen *screen : QGuiApplication::screens()) {
			s.screens.push_back(screen->geometry());
			s.desktop = s.desktop.united(screen->geometry());
		}
		s.valid = !s.primary.isEmpty();
	}
	{
		std::lock_guard<std::mutex> lock(snapshot_mutex);
		snapshot = std::move(s);
	}
	task_pending = false;
}

} // namespace

extern "C" bool spectrum_cursor_position(int screen, double *x, double *y)
{
	if (!gui_app())
		return false;
	if (!task_pending.exchange(true))
		obs_queue_task(OBS_TASK_UI, read_cursor_task, nullptr, false);

	std::lock_guard<std::mutex> lock(snapshot_mutex);
	if (!snapshot.valid)
		return false;
	QRect r = snapshot.primary;
	if (screen < 0)
		r = snapshot.desktop;
	else if (screen > 0 && (size_t)screen <= snapshot.screens.size())
		r = snapshot.screens[(size_t)screen - 1];
	if (r.width() <= 1 || r.height() <= 1)
		return false;
	const double nx = (snapshot.pos.x() - r.x()) / (double)(r.width() - 1);
	const double ny = (snapshot.pos.y() - r.y()) / (double)(r.height() - 1);
	*x = nx < 0.0 ? 0.0 : (nx > 1.0 ? 1.0 : nx);
	*y = ny < 0.0 ? 0.0 : (ny > 1.0 ? 1.0 : ny);
	return true;
}

extern "C" void spectrum_cursor_list_screens(void (*cb)(void *param, int index, const char *name), void *param)
{
	QGuiApplication *app = gui_app();
	if (!app || QThread::currentThread() != app->thread())
		return;
	int index = 1;
	for (QScreen *screen : QGuiApplication::screens()) {
		const QRect g = screen->geometry();
		const QString label =
			QStringLiteral("%1: %2 (%3x%4)").arg(index).arg(screen->name()).arg(g.width()).arg(g.height());
		cb(param, index, label.toUtf8().constData());
		index++;
	}
}
