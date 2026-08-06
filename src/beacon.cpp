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

#include "beacon.h"
#include "log-capture.hpp"
#include "connection-monitor.hpp"
#include "diagnostics-dock.hpp"
#include "beacon-rtmp.h"

#include <plugin-support.h>
#include <util/base.h>
#include <obs-module.h>
#include <obs-frontend-api.h>

#include <memory>

namespace {
constexpr const char *kDockId = "beacon";

// Owned for the lifetime of the loaded module.
std::unique_ptr<beacon::LogCapture> g_log_capture;
std::unique_ptr<beacon::ConnectionMonitor> g_connection_monitor;

// The dock is owned by OBS once added; we keep a raw pointer only to detach() it
// before the components it references are destroyed.
beacon::DiagnosticsDock *g_dock = nullptr;
} // namespace

void beacon_load(void)
{
	// Register the Beacon RTMP output + service (Phase 0: vendored stock output).
	beacon_rtmp_register();

	// Install log capture first so it also records the components' own logs.
	g_log_capture = std::make_unique<beacon::LogCapture>();
	g_log_capture->install();
	obs_log(LOG_INFO, "diagnostics: log capture installed (ring buffer %zu lines)",
		beacon::LogCapture::kMaxEntries);

	g_connection_monitor = std::make_unique<beacon::ConnectionMonitor>();
	g_connection_monitor->install();

	// Register the dock. OBS wraps the widget in a QDockWidget and takes
	// ownership on success.
	g_dock = new beacon::DiagnosticsDock(g_connection_monitor.get(), g_log_capture.get());
	if (obs_frontend_add_dock_by_id(kDockId, obs_module_text("Beacon"), g_dock)) {
		obs_log(LOG_INFO, "diagnostics: dock registered ('%s')", kDockId);
	} else {
		obs_log(LOG_WARNING, "diagnostics: failed to register dock");
		delete g_dock;
		g_dock = nullptr;
	}
}

void beacon_unload(void)
{
	// Detach the dock from the components first so a late UI refresh cannot
	// touch freed state, then let OBS destroy the widget.
	if (g_dock) {
		g_dock->detach();
		g_dock = nullptr;
	}
	obs_frontend_remove_dock(kDockId);

	if (g_connection_monitor) {
		g_connection_monitor->uninstall();
		g_connection_monitor.reset();
	}

	if (g_log_capture) {
		obs_log(LOG_INFO, "diagnostics: log capture stopping (%zu entries, %zu connection-related)",
			g_log_capture->size(), g_log_capture->connectionCount());
		g_log_capture->uninstall();
		g_log_capture.reset();
	}
}
