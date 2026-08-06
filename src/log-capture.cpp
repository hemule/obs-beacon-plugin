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

#include "log-capture.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>

namespace beacon {

namespace {

// Lowercased substrings that mark a log line as connection-relevant. Matched
// against a lowercased copy of the message, so this also catches "RTMP",
// "SRT", "RTMP_Connect0", "WriteN", "Output '...'", etc.
constexpr std::array<const char *, 8> kNeedles = {
	"rtmp", "srt", "connect", "socket", "reconnect", "writen", "output '", "connecting to",
};

bool matches_connection(const std::string &lower)
{
	for (const char *needle : kNeedles) {
		if (lower.find(needle) != std::string::npos)
			return true;
	}
	return false;
}

std::string now_timestamp()
{
	using namespace std::chrono;
	const auto now = system_clock::now();
	const auto t = system_clock::to_time_t(now);
	const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;

	std::tm tmv{};
#ifdef _WIN32
	localtime_s(&tmv, &t); // Windows: (tm*, time_t*) — note reversed argument order
#else
	localtime_r(&t, &tmv);
#endif

	char buf[16];
	std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d.%03d", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
		      static_cast<int>(ms.count()));
	return std::string(buf);
}

void rstrip_newlines(std::string &s)
{
	while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
		s.pop_back();
}

} // namespace

void LogCapture::install()
{
	if (installed_)
		return;
	// Order matters: record the current handler before installing ours, so the
	// handler (which may fire immediately from another thread) sees a valid chain.
	base_get_log_handler(&prevHandler_, &prevParam_);
	base_set_log_handler(&LogCapture::handlerThunk, this);
	installed_ = true;
}

void LogCapture::uninstall()
{
	if (!installed_)
		return;
	base_set_log_handler(prevHandler_, prevParam_);
	installed_ = false;
}

void LogCapture::handlerThunk(int level, const char *msg, va_list args, void *param)
{
	auto *self = static_cast<LogCapture *>(param);
	if (self)
		self->onLog(level, msg, args);
	// Always forward the ORIGINAL, untouched args to the previous handler so
	// OBS's own log file and console keep working.
	if (self && self->prevHandler_)
		self->prevHandler_(level, msg, args, self->prevParam_);
}

void LogCapture::onLog(int level, const char *msg, va_list args)
{
	if (!msg)
		return;

	// Format into a local buffer using a COPY of args; the original must stay
	// intact for the chained handler.
	std::string text;
	char stack_buf[1024];
	va_list ap;
	va_copy(ap, args);
	const int n = std::vsnprintf(stack_buf, sizeof(stack_buf), msg, ap);
	va_end(ap);

	if (n < 0) {
		text = msg; // formatting error: fall back to the raw format string
	} else if (static_cast<size_t>(n) < sizeof(stack_buf)) {
		text.assign(stack_buf, static_cast<size_t>(n));
	} else {
		// Rare oversized line: reformat with an exact-size buffer.
		text.resize(static_cast<size_t>(n));
		va_list ap2;
		va_copy(ap2, args);
		std::vsnprintf(text.data(), static_cast<size_t>(n) + 1, msg, ap2);
		va_end(ap2);
	}

	rstrip_newlines(text);

	std::string lower = text;
	std::transform(lower.begin(), lower.end(), lower.begin(),
		       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

	// Connection view keeps matched lines plus everything at WARNING/ERROR
	// (lower numeric level == more severe).
	const bool conn = level <= LOG_WARNING || matches_connection(lower);

	LogEntry entry{level, now_timestamp(), std::move(text), conn};

	std::lock_guard<std::mutex> lk(mtx_);
	buffer_.push_back(std::move(entry));
	++generation_;
	if (buffer_.back().connection)
		++connectionCount_;
	if (buffer_.size() > kMaxEntries) {
		if (buffer_.front().connection && connectionCount_ > 0)
			--connectionCount_;
		buffer_.pop_front();
	}
}

std::vector<LogEntry> LogCapture::snapshot(bool onlyConnection) const
{
	std::lock_guard<std::mutex> lk(mtx_);
	std::vector<LogEntry> out;
	out.reserve(onlyConnection ? connectionCount_ : buffer_.size());
	for (const auto &e : buffer_) {
		if (!onlyConnection || e.connection)
			out.push_back(e);
	}
	return out;
}

uint64_t LogCapture::generation() const
{
	std::lock_guard<std::mutex> lk(mtx_);
	return generation_;
}

void LogCapture::clear()
{
	std::lock_guard<std::mutex> lk(mtx_);
	buffer_.clear();
	connectionCount_ = 0;
}

size_t LogCapture::size() const
{
	std::lock_guard<std::mutex> lk(mtx_);
	return buffer_.size();
}

size_t LogCapture::connectionCount() const
{
	std::lock_guard<std::mutex> lk(mtx_);
	return connectionCount_;
}

} // namespace beacon
