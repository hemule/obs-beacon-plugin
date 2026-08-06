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

#include <util/base.h>

#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace beacon {

struct LogEntry {
	int level;             // OBS log level (LOG_ERROR .. LOG_DEBUG)
	std::string timestamp; // "HH:MM:SS.mmm", captured when the line was logged
	std::string text;      // formatted message, trailing newline stripped
	bool connection;       // line is relevant to the connection view
};

/*
 * Intercepts the libobs log stream, keeps a bounded ring buffer of entries and
 * flags the ones relevant to RTMP/SRT connection diagnostics.
 *
 * Thread-safety: libobs invokes the log handler from arbitrary threads (network,
 * encoder, UI), so every access to the buffer is guarded by a mutex. The handler
 * itself does no OBS API calls and no blocking work beyond formatting + a push.
 * The previously installed handler is always chained so OBS's own logging keeps
 * working.
 */
class LogCapture {
public:
	static constexpr size_t kMaxEntries = 2000;

	LogCapture() = default;
	~LogCapture() = default;
	LogCapture(const LogCapture &) = delete;
	LogCapture &operator=(const LogCapture &) = delete;

	// Install our handler (saving + chaining the previous one) / restore it.
	// Call install() from obs_module_load and uninstall() from obs_module_unload.
	void install();
	void uninstall();

	// Snapshot copy of the buffer, optionally only connection-relevant lines.
	std::vector<LogEntry> snapshot(bool onlyConnection) const;

	// Monotonic count of entries ever appended; lets a UI poller detect changes
	// cheaply without copying the whole buffer.
	uint64_t generation() const;

	void clear();
	size_t size() const;
	size_t connectionCount() const;

private:
	static void handlerThunk(int level, const char *msg, va_list args, void *param);
	void onLog(int level, const char *msg, va_list args);

	mutable std::mutex mtx_;
	std::deque<LogEntry> buffer_;
	uint64_t generation_ = 0;
	size_t connectionCount_ = 0;

	// Set once in install() before base_set_log_handler, treated as immutable
	// until uninstall(); read from the handler without a lock.
	log_handler_t prevHandler_ = nullptr;
	void *prevParam_ = nullptr;
	bool installed_ = false;
};

} // namespace beacon
