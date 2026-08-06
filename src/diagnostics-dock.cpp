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

#include "diagnostics-dock.hpp"
#include "connection-monitor.hpp"
#include "log-capture.hpp"
#include "log-window.hpp"

#include <obs-module.h>

#include <cstring>

#include <QCheckBox>
#include <QClipboard>
#include <QFontDatabase>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollBar>
#include <QString>
#include <QTimer>
#include <QVBoxLayout>

namespace beacon {

namespace {

constexpr int kRefreshMs = 1000;
constexpr const char *kDash = "\xe2\x80\x94"; // em dash

// Localised state name + a colour for the status dot.
struct StateView {
	const char *textKey;
	const char *color;
};

StateView stateView(ConnState state)
{
	switch (state) {
	case ConnState::Idle:
		return {"State.Idle", "#888888"};
	case ConnState::Connecting:
		return {"State.Connecting", "#e0a800"};
	case ConnState::Live:
		return {"State.Live", "#28a745"};
	case ConnState::Reconnecting:
		return {"State.Reconnecting", "#e0a800"};
	case ConnState::Failed:
		return {"State.Failed", "#dc3545"};
	}
	return {"State.Idle", "#888888"};
}

QString durationText(qint64 ms)
{
	const qint64 total = ms / 1000;
	const qint64 h = total / 3600;
	const qint64 m = (total % 3600) / 60;
	const qint64 s = total % 60;
	return QString::asprintf("%02lld:%02lld:%02lld", static_cast<long long>(h), static_cast<long long>(m),
				 static_cast<long long>(s));
}

QLabel *addRow(QGridLayout *grid, int row, const char *captionKey)
{
	auto *caption = new QLabel(QString::fromUtf8(obs_module_text(captionKey)));
	caption->setStyleSheet("color: palette(mid);");
	auto *value = new QLabel(QString::fromUtf8(kDash));
	value->setTextInteractionFlags(Qt::TextSelectableByMouse);
	grid->addWidget(caption, row, 0, Qt::AlignLeft | Qt::AlignTop);
	grid->addWidget(value, row, 1, Qt::AlignLeft | Qt::AlignTop);
	return value;
}

} // namespace

DiagnosticsDock::DiagnosticsDock(ConnectionMonitor *monitor, LogCapture *logCapture, QWidget *parent)
	: QWidget(parent),
	  monitor_(monitor),
	  logCapture_(logCapture)
{
	setObjectName("BeaconDock");
	buildUi();

	// Log lives in a separate, on-demand window so the dock stays compact.
	logWindow_ = new LogWindow(logCapture_, this);

	timer_ = new QTimer(this);
	timer_->setInterval(kRefreshMs);
	connect(timer_, &QTimer::timeout, this, &DiagnosticsDock::refresh);
	timer_->start();

	refresh();
}

DiagnosticsDock::~DiagnosticsDock() = default;

void DiagnosticsDock::detach()
{
	if (timer_)
		timer_->stop();
	if (logWindow_)
		logWindow_->detach();
	monitor_ = nullptr;
	logCapture_ = nullptr;
}

void DiagnosticsDock::buildUi()
{
	auto *root = new QVBoxLayout(this);

	// --- Status group ---
	auto *statusGroup = new QGroupBox(QString::fromUtf8(obs_module_text("Status")));
	auto *grid = new QGridLayout(statusGroup);
	grid->setColumnStretch(1, 1);

	int row = 0;
	stateValue_ = addRow(grid, row++, "State");
	ingestValue_ = addRow(grid, row++, "Ingest");
	durationValue_ = addRow(grid, row++, "Duration");
	bitrateValue_ = addRow(grid, row++, "Bitrate");
	congestionValue_ = addRow(grid, row++, "Congestion");
	droppedValue_ = addRow(grid, row++, "DroppedFrames");
	latencyValue_ = addRow(grid, row++, "InObsLatency");
	reconnectValue_ = addRow(grid, row++, "Reconnects");

	errorCaption_ = new QLabel(QString::fromUtf8(obs_module_text("LastError")));
	errorCaption_->setStyleSheet("color: palette(mid);");
	errorValue_ = new QLabel();
	errorValue_->setWordWrap(true);
	errorValue_->setStyleSheet("color: #dc3545;");
	errorValue_->setTextInteractionFlags(Qt::TextSelectableByMouse);
	grid->addWidget(errorCaption_, row, 0, Qt::AlignLeft | Qt::AlignTop);
	grid->addWidget(errorValue_, row, 1, Qt::AlignLeft | Qt::AlignTop);
	errorCaption_->setVisible(false);
	errorValue_->setVisible(false);

	root->addWidget(statusGroup);

	// Log opens in a separate window (see LogWindow) to keep the dock small.
	auto *logButtons = new QHBoxLayout();
	auto *showLogBtn = new QPushButton(QString::fromUtf8(obs_module_text("ShowLog")));
	connect(showLogBtn, &QPushButton::clicked, this, &DiagnosticsDock::onShowLog);
	logButtons->addWidget(showLogBtn);
	logButtons->addStretch(1);
	root->addLayout(logButtons);

	// Route streaming through our output (which adds app/streamSessionId to the
	// RTMP connect). Persists via the profile's service.json, so once enabled it
	// survives restarts — no need to re-enable each session.
	routeCheck_ = new QCheckBox(QStringLiteral("Route streaming through Beacon"));
	routeCheck_->setToolTip(
		QStringLiteral("Wraps the current stream service so OBS uses the Beacon output, which adds "
			       "app/streamSessionId to the RTMP connect.\nPersists across restarts. "
			       "Note: editing Service in Settings \xe2\x86\x92 Stream turns this off."));
	connect(routeCheck_, &QCheckBox::toggled, this, &DiagnosticsDock::onToggleRoute);
	root->addWidget(routeCheck_);

	// Push everything to the top so the dock can be resized down freely.
	root->addStretch(1);
}

void DiagnosticsDock::onShowLog()
{
	if (!logWindow_)
		return;
	logWindow_->show();
	logWindow_->raise();
	logWindow_->activateWindow();
}

void DiagnosticsDock::onToggleRoute(bool enabled)
{
	// Never switch the streaming service mid-stream.
	if (obs_frontend_streaming_active()) {
		blog(LOG_WARNING, "[obs-beacon] cannot change Beacon routing while streaming");
		if (routeCheck_) {
			routeCheck_->blockSignals(true);
			routeCheck_->setChecked(!enabled);
			routeCheck_->blockSignals(false);
		}
		return;
	}

	// Carry the current server/key across the service swap (non-owning ref).
	obs_service_t *cur = obs_frontend_get_streaming_service();
	const char *server = cur ? obs_service_get_connect_info(cur, OBS_SERVICE_CONNECT_INFO_SERVER_URL) : nullptr;
	const char *key = cur ? obs_service_get_connect_info(cur, OBS_SERVICE_CONNECT_INFO_STREAM_KEY) : nullptr;

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "server", server ? server : "");
	obs_data_set_string(settings, "key", key ? key : "");
	// Display name shown by OBS Settings -> Stream for our custom service type
	// (its else-branch inserts settings["service"] into the combo); without it OBS
	// defaults to the first listed service (Twitch), which is misleading.
	obs_data_set_string(settings, "service", "Beacon");

	// ON  -> beacon_service (get_output_type routes streaming through our output);
	// OFF -> plain rtmp_custom with the same server/key.
	const char *type = enabled ? "beacon_service" : "rtmp_custom";
	obs_service_t *svc = obs_service_create(type, "default_service", settings, nullptr);
	if (svc) {
		obs_frontend_set_streaming_service(svc);
		obs_frontend_save_streaming_service();
		obs_service_release(svc);
		blog(LOG_INFO, "[obs-beacon] Beacon routing %s (service='%s')", enabled ? "ON" : "OFF", type);
	}
	obs_data_release(settings);
}

void DiagnosticsDock::refresh()
{
	if (!monitor_ || !logCapture_)
		return;

	// Keep the "route through Beacon" toggle in sync with the actual service, and
	// lock it while streaming (the service must not change mid-stream).
	if (routeCheck_) {
		obs_service_t *svc = obs_frontend_get_streaming_service();
		const bool routed = svc && strcmp(obs_service_get_type(svc), "beacon_service") == 0;
		if (routeCheck_->isChecked() != routed) {
			routeCheck_->blockSignals(true);
			routeCheck_->setChecked(routed);
			routeCheck_->blockSignals(false);
		}
		routeCheck_->setEnabled(!obs_frontend_streaming_active());
	}

	const ConnStatus st = monitor_->status();
	const StateView sv = stateView(st.state);

	stateValue_->setText(QString::fromUtf8("\xe2\x97\x8f ") + QString::fromUtf8(obs_module_text(sv.textKey)));
	stateValue_->setStyleSheet(QString("color: %1; font-weight: bold;").arg(sv.color));

	const QString ingest = st.ingestUrl.empty() ? QString::fromUtf8(kDash) : QString::fromStdString(st.ingestUrl);
	ingestValue_->setText(ingest);
	// Full URL on hover in case a long one is clipped by the column width.
	ingestValue_->setToolTip(st.ingestUrl.empty() ? QString() : ingest);
	reconnectValue_->setText(QString::number(st.reconnectCount));

	const bool failed = st.state == ConnState::Failed;
	errorCaption_->setVisible(failed);
	errorValue_->setVisible(failed);
	if (failed) {
		QString reason = QString::fromUtf8(ConnectionMonitor::stopReasonLabel(st.stopCode));
		QString detail = st.lastError.empty() ? QString() : (" — " + QString::fromStdString(st.lastError));
		errorValue_->setText(QString("[%1] %2%3").arg(st.stopCode).arg(reason).arg(detail));
	}

	// Session duration + live counters.
	const bool live = st.state == ConnState::Live || st.state == ConnState::Reconnecting;
	if (live && !sessionActive_) {
		sessionActive_ = true;
		sessionClock_.start();
		haveBytesBaseline_ = false;
	} else if (!live && sessionActive_) {
		sessionActive_ = false;
	}

	if (live) {
		durationValue_->setText(durationText(sessionClock_.elapsed()));
		const ConnStats s = monitor_->pollStats();
		if (s.valid) {
			if (haveBytesBaseline_) {
				const double kbps = static_cast<double>(s.totalBytes - prevBytes_) * 8.0 / 1000.0;
				bitrateValue_->setText(QString::asprintf("%.0f kb/s", kbps));
			} else {
				bitrateValue_->setText(QString::fromUtf8(kDash));
			}
			prevBytes_ = s.totalBytes;
			haveBytesBaseline_ = true;

			congestionValue_->setText(QString::asprintf("%.0f%%", s.congestion * 100.0));
			const double dropPct =
				s.totalFrames > 0 ? 100.0 * static_cast<double>(s.framesDropped) / s.totalFrames : 0.0;
			droppedValue_->setText(
				QString::asprintf("%d / %d (%.1f%%)", s.framesDropped, s.totalFrames, dropPct));
		}

		// OBS-internal latency (composite -> egress) from the packet probe.
		const ProbeStats lat = monitor_->latencyStats();
		if (lat.valid) {
			latencyValue_->setText(
				QString::asprintf("%.0f ms  (render %.0f \xc2\xb7 enc %.0f \xc2\xb7 mux %.0f)",
						  lat.inObsMs, lat.renderToSubmitMs, lat.encodeMs, lat.toInterleaveMs));
			latencyValue_->setToolTip(QString::asprintf(
				"composite \xe2\x86\x92 egress: avg %.1f ms (min %.1f / max %.1f)\n"
				"render\xe2\x86\x92submit %.1f \xc2\xb7 encode %.1f \xc2\xb7 \xe2\x86\x92interleave %.1f ms",
				lat.inObsMs, lat.minMs, lat.maxMs, lat.renderToSubmitMs, lat.encodeMs,
				lat.toInterleaveMs));
		} else {
			latencyValue_->setText(QString::fromUtf8(kDash));
			latencyValue_->setToolTip(QString());
		}
	} else {
		durationValue_->setText(QString::fromUtf8(kDash));
		bitrateValue_->setText(QString::fromUtf8(kDash));
		congestionValue_->setText(QString::fromUtf8(kDash));
		droppedValue_->setText(QString::fromUtf8(kDash));
		latencyValue_->setText(QString::fromUtf8(kDash));
		latencyValue_->setToolTip(QString());
		haveBytesBaseline_ = false;
	}
}

} // namespace beacon
