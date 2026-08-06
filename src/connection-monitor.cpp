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

#include "connection-monitor.hpp"

#include <plugin-support.h>
#include <util/base.h>

#include <callback/calldata.h>
#include <callback/signal.h>

namespace beacon {

const char *ConnectionMonitor::stateLabel(ConnState state)
{
	switch (state) {
	case ConnState::Idle:
		return "IDLE";
	case ConnState::Connecting:
		return "CONNECTING";
	case ConnState::Live:
		return "LIVE";
	case ConnState::Reconnecting:
		return "RECONNECTING";
	case ConnState::Failed:
		return "FAILED";
	}
	return "UNKNOWN";
}

const char *ConnectionMonitor::stopReasonLabel(int code)
{
	switch (code) {
	case OBS_OUTPUT_SUCCESS:
		return "normal stop";
	case OBS_OUTPUT_BAD_PATH:
		return "bad ingest URL";
	case OBS_OUTPUT_CONNECT_FAILED:
		return "could not reach ingest";
	case OBS_OUTPUT_INVALID_STREAM:
		return "stream key/access rejected";
	case OBS_OUTPUT_ERROR:
		return "output error";
	case OBS_OUTPUT_DISCONNECTED:
		return "unexpected disconnect";
	case OBS_OUTPUT_UNSUPPORTED:
		return "unsupported settings";
	case OBS_OUTPUT_NO_SPACE:
		return "no disk space";
	case OBS_OUTPUT_ENCODE_ERROR:
		return "encoding error";
	case OBS_OUTPUT_HDR_DISABLED:
		return "HDR disabled";
	default:
		return "unknown reason";
	}
}

void ConnectionMonitor::install()
{
	if (installed_)
		return;
	obs_frontend_add_event_callback(&ConnectionMonitor::frontendEventThunk, this);
	installed_ = true;
	obs_log(LOG_INFO, "diagnostics: connection monitor installed");
}

void ConnectionMonitor::uninstall()
{
	if (!installed_)
		return;
	obs_frontend_remove_event_callback(&ConnectionMonitor::frontendEventThunk, this);
	// If the module is unloaded mid-stream, tear down cleanly.
	disconnectSignals();
	releaseOutput();
	installed_ = false;
	obs_log(LOG_INFO, "diagnostics: connection monitor uninstalled");
}

void ConnectionMonitor::frontendEventThunk(enum obs_frontend_event event, void *data)
{
	static_cast<ConnectionMonitor *>(data)->onFrontendEvent(event);
}

void ConnectionMonitor::onFrontendEvent(enum obs_frontend_event event)
{
	switch (event) {
	case OBS_FRONTEND_EVENT_STREAMING_STARTING: {
		acquireOutput();
		connectSignals();
		std::string url = queryIngestUrl();
		{
			std::lock_guard<std::mutex> lk(mtx_);
			status_.reconnectCount = 0;
			status_.stopCode = OBS_OUTPUT_SUCCESS;
			status_.lastError.clear();
			status_.ingestUrl = url;
		}
		if (!url.empty())
			obs_log(LOG_INFO, "diagnostics: streaming to ingest '%s'", url.c_str());
		if (output_)
			obs_log(LOG_INFO, "diagnostics: streaming output id='%s'", obs_output_get_id(output_));
		{
			obs_service_t *svc = obs_frontend_get_streaming_service();
			obs_log(LOG_INFO, "diagnostics: streaming service type='%s'",
				svc ? obs_service_get_type(svc) : "?");
		}
		setState(ConnState::Connecting);
		break;
	}
	case OBS_FRONTEND_EVENT_STREAMING_STOPPED:
		// The "stop" signal has already set the terminal state; just release.
		disconnectSignals();
		releaseOutput();
		break;
	default:
		break;
	}
}

void ConnectionMonitor::onSigStarting(void *data, calldata_t *)
{
	static_cast<ConnectionMonitor *>(data)->setState(ConnState::Connecting);
}

void ConnectionMonitor::onSigStart(void *data, calldata_t *)
{
	static_cast<ConnectionMonitor *>(data)->setState(ConnState::Live);
}

void ConnectionMonitor::onSigReconnect(void *data, calldata_t *)
{
	auto *self = static_cast<ConnectionMonitor *>(data);
	{
		std::lock_guard<std::mutex> lk(self->mtx_);
		++self->status_.reconnectCount;
	}
	self->setState(ConnState::Reconnecting);
}

void ConnectionMonitor::onSigReconnectSuccess(void *data, calldata_t *)
{
	static_cast<ConnectionMonitor *>(data)->setState(ConnState::Live);
}

void ConnectionMonitor::onSigStop(void *data, calldata_t *cd)
{
	auto *self = static_cast<ConnectionMonitor *>(data);

	const int code = static_cast<int>(calldata_int(cd, "code"));
	const char *lastError = calldata_string(cd, "last_error");

	{
		std::lock_guard<std::mutex> lk(self->mtx_);
		self->status_.stopCode = code;
		self->status_.lastError = lastError ? lastError : "";
	}

	if (code == OBS_OUTPUT_SUCCESS) {
		self->setState(ConnState::Idle);
	} else {
		obs_log(LOG_WARNING, "diagnostics: stream stopped, code=%d (%s)%s%s", code, stopReasonLabel(code),
			(lastError && *lastError) ? " last_error: " : "", (lastError && *lastError) ? lastError : "");
		self->setState(ConnState::Failed);
	}
}

void ConnectionMonitor::acquireOutput()
{
	// obs_frontend_get_streaming_output returns a NEW reference (release later).
	output_ = obs_frontend_get_streaming_output();
	signals_ = output_ ? obs_output_get_signal_handler(output_) : nullptr;
	if (output_)
		probe_.attach(output_);
}

void ConnectionMonitor::releaseOutput()
{
	if (output_) {
		probe_.detach(output_);
		obs_output_release(output_);
		output_ = nullptr;
	}
	signals_ = nullptr;
}

void ConnectionMonitor::connectSignals()
{
	if (!signals_)
		return;
	signal_handler_connect(signals_, "starting", &ConnectionMonitor::onSigStarting, this);
	signal_handler_connect(signals_, "start", &ConnectionMonitor::onSigStart, this);
	signal_handler_connect(signals_, "stop", &ConnectionMonitor::onSigStop, this);
	signal_handler_connect(signals_, "reconnect", &ConnectionMonitor::onSigReconnect, this);
	signal_handler_connect(signals_, "reconnect_success", &ConnectionMonitor::onSigReconnectSuccess, this);
}

void ConnectionMonitor::disconnectSignals()
{
	if (!signals_)
		return;
	signal_handler_disconnect(signals_, "starting", &ConnectionMonitor::onSigStarting, this);
	signal_handler_disconnect(signals_, "start", &ConnectionMonitor::onSigStart, this);
	signal_handler_disconnect(signals_, "stop", &ConnectionMonitor::onSigStop, this);
	signal_handler_disconnect(signals_, "reconnect", &ConnectionMonitor::onSigReconnect, this);
	signal_handler_disconnect(signals_, "reconnect_success", &ConnectionMonitor::onSigReconnectSuccess, this);
}

std::string ConnectionMonitor::queryIngestUrl() const
{
	// obs_frontend_get_streaming_service returns a NON-owning reference (no release).
	obs_service_t *svc = obs_frontend_get_streaming_service();
	if (!svc)
		return {};
	const char *url = obs_service_get_connect_info(svc, OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	return url ? std::string(url) : std::string();
}

void ConnectionMonitor::setState(ConnState next)
{
	ConnState prev;
	{
		std::lock_guard<std::mutex> lk(mtx_);
		prev = status_.state;
		status_.state = next;
	}
	if (prev != next)
		obs_log(LOG_INFO, "diagnostics: state %s -> %s", stateLabel(prev), stateLabel(next));
}

ConnStatus ConnectionMonitor::status() const
{
	std::lock_guard<std::mutex> lk(mtx_);
	return status_;
}

ConnStats ConnectionMonitor::pollStats() const
{
	ConnStats s;
	if (!output_)
		return s;
	s.valid = true;
	s.active = obs_output_active(output_);
	s.reconnecting = obs_output_reconnecting(output_);
	s.congestion = obs_output_get_congestion(output_);
	s.framesDropped = obs_output_get_frames_dropped(output_);
	s.totalFrames = obs_output_get_total_frames(output_);
	s.totalBytes = obs_output_get_total_bytes(output_);
	return s;
}

} // namespace beacon
