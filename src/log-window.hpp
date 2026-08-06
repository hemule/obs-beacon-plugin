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

#include <QWidget>

#include <cstdint>

class QPlainTextEdit;
class QCheckBox;
class QTimer;

namespace beacon {

class LogCapture;

/*
 * Separate floating window for the connection log, opened from the dock so the
 * dock itself stays compact and does not eat into the stream preview.
 *
 * Parented to the dock (destroyed with it) but shown as a top-level window.
 * Refreshes on its own timer that runs ONLY while the window is visible, so a
 * hidden log window costs nothing. All access is on the UI thread; LogCapture's
 * accessors are internally locked.
 */
class LogWindow : public QWidget {
	Q_OBJECT

public:
	explicit LogWindow(LogCapture *logCapture, QWidget *parent = nullptr);
	~LogWindow() override;

	// Stop refreshing and drop the component pointer before it is freed.
	void detach();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private slots:
	void refresh();
	void onFilterToggled(bool onlyConnection);
	void onCopy();
	void onClear();

private:
	void buildUi();
	void refreshLogs(bool force);

	LogCapture *logCapture_ = nullptr;
	QTimer *timer_ = nullptr;
	QPlainTextEdit *logView_ = nullptr;
	QCheckBox *filterCheck_ = nullptr;

	uint64_t lastLogGeneration_ = 0;
	bool onlyConnection_ = true;
};

} // namespace beacon
