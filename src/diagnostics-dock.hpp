/*
Beacon
Copyright (C) 2026 hemule <hemule@mayflower.work>

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License along
with this program. If not, see <https://www.gnu.org/licenses/>
*/

#pragma once

#include <QElapsedTimer>
#include <QWidget>

#include <cstdint>

class QLabel;
class QPlainTextEdit;
class QCheckBox;
class QPushButton;
class QTimer;

namespace beacon {

class ConnectionMonitor;
class LogCapture;

/*
 * Diagnostics dock: a QWidget shown via obs_frontend_add_dock_by_id.
 *
 * Pulls data from ConnectionMonitor and LogCapture on a 1s QTimer. Everything
 * here runs on the Qt UI thread, and the component accessors are internally
 * locked, so no manual cross-thread marshalling is needed.
 *
 * Ownership: OBS takes ownership of this widget. Before the owning components
 * are destroyed at module unload, call detach() to stop the timer and drop the
 * (non-owning) component pointers, so a late refresh can never touch freed state.
 */
class DiagnosticsDock : public QWidget {
	Q_OBJECT

public:
	DiagnosticsDock(ConnectionMonitor *monitor, LogCapture *logCapture, QWidget *parent = nullptr);
	~DiagnosticsDock() override;

	// Stop refreshing and forget the components. Call on the UI thread before
	// the components are freed.
	void detach();

private slots:
	void refresh();
	void onShowLog();
	void onToggleRoute(bool enabled); // route streaming through beacon_rtmp_output (persists)

private:
	void buildUi();

	ConnectionMonitor *monitor_ = nullptr;
	LogCapture *logCapture_ = nullptr;

	QTimer *timer_ = nullptr;
	class LogWindow *logWindow_ = nullptr; // parented to this; shown on demand

	// Status widgets.
	QLabel *stateValue_ = nullptr;
	QLabel *ingestValue_ = nullptr;
	QLabel *durationValue_ = nullptr;
	QLabel *bitrateValue_ = nullptr;
	QLabel *congestionValue_ = nullptr;
	QLabel *droppedValue_ = nullptr;
	QLabel *latencyValue_ = nullptr;
	QLabel *reconnectValue_ = nullptr;
	QLabel *errorCaption_ = nullptr;
	QLabel *errorValue_ = nullptr;

	QCheckBox *routeCheck_ = nullptr; // "route through Beacon" toggle

	// Refresh bookkeeping.
	QElapsedTimer sessionClock_; // measures LIVE/RECONNECTING duration
	bool sessionActive_ = false;
	uint64_t prevBytes_ = 0;
	bool haveBytesBaseline_ = false;
};

} // namespace beacon
