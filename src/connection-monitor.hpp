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

#include "packet-probe.hpp"

#include <obs.h>
#include <obs-frontend-api.h>

#include <cstdint>
#include <mutex>
#include <string>

namespace beacon {

// Connection state machine: IDLE -> CONNECTING -> LIVE -> RECONNECTING ->
// (LIVE | FAILED), plus a terminal FAILED(reason) derived from the stop code.
enum class ConnState { Idle, Connecting, Live, Reconnecting, Failed };

// State-machine snapshot (updated from output-signal threads, guarded by mutex).
struct ConnStatus {
	ConnState state = ConnState::Idle;
	int stopCode = OBS_OUTPUT_SUCCESS; // meaningful when state == Failed
	std::string lastError;             // human-readable, when available
	std::string ingestUrl;             // server URL captured at stream start
	int reconnectCount = 0;            // number of reconnect attempts this session
};

// Live counters read on demand from the streaming output (UI thread only).
struct ConnStats {
	bool valid = false;        // false when there is no active output
	bool active = false;       // obs_output_active
	bool reconnecting = false; // obs_output_reconnecting
	float congestion = 0.0f;   // 0..1 network congestion
	int framesDropped = 0;
	int totalFrames = 0;
	uint64_t totalBytes = 0;
};

/*
 * Observes the main streaming output and maintains the connection state machine.
 *
 * Threading:
 *  - Frontend events (obs_frontend_add_event_callback) are delivered on the UI
 *    thread. We acquire/release the streaming output and connect/disconnect its
 *    signals there, so the output reference lifecycle is entirely UI-thread-bound.
 *  - Output signals fire on the output's network/encoder threads. Their handlers
 *    only update the guarded ConnStatus and log; they do no blocking OBS calls
 *    and never touch the reference lifecycle.
 *  - pollStats() reads counters straight from the output and MUST be called on
 *    the UI thread (e.g. from the dock's QTimer).
 */
class ConnectionMonitor {
public:
	ConnectionMonitor() = default;
	~ConnectionMonitor() = default;
	ConnectionMonitor(const ConnectionMonitor &) = delete;
	ConnectionMonitor &operator=(const ConnectionMonitor &) = delete;

	// Register/unregister the frontend event callback. install() from
	// obs_module_load, uninstall() from obs_module_unload.
	void install();
	void uninstall();

	// Thread-safe snapshot of the state machine.
	ConnStatus status() const;

	// Read live counters from the current output. UI thread only.
	ConnStats pollStats() const;

	// Last per-second encoded-packet timing snapshot (OBS-internal latency).
	ProbeStats latencyStats() const { return probe_.stats(); }

	static const char *stateLabel(ConnState state);
	// Short, stable English label for a stop code (localised later in the dock).
	static const char *stopReasonLabel(int code);

private:
	static void frontendEventThunk(enum obs_frontend_event event, void *data);
	void onFrontendEvent(enum obs_frontend_event event);

	// Output signal handlers (run on output threads).
	static void onSigStarting(void *data, calldata_t *cd);
	static void onSigStart(void *data, calldata_t *cd);
	static void onSigStop(void *data, calldata_t *cd);
	static void onSigReconnect(void *data, calldata_t *cd);
	static void onSigReconnectSuccess(void *data, calldata_t *cd);

	// UI-thread helpers.
	void acquireOutput();
	void releaseOutput();
	void connectSignals();
	void disconnectSignals();
	std::string queryIngestUrl() const;

	// Update state + log the transition (logging done outside the lock).
	void setState(ConnState next);

	mutable std::mutex mtx_;
	ConnStatus status_;

	// UI-thread-only members.
	obs_output_t *output_ = nullptr;
	signal_handler_t *signals_ = nullptr;
	bool installed_ = false;

	// Read-only encoded-packet timing probe, attached to the current output.
	PacketProbe probe_;
};

} // namespace beacon
