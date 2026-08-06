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

#include "packet-probe.hpp"

#include <plugin-support.h>
#include <util/base.h>
#include <util/bmem.h>
#include <util/platform.h>
#include <util/threading.h>

#include <chrono>
#include <cstring>
#include <string>
#include <vector>

namespace beacon {

namespace {
constexpr uint64_t kWindowNs = 1000000000ULL; // 1 second
inline double ns_to_ms(uint64_t ns)
{
	return static_cast<double>(ns) / 1.0e6;
}

// Short names for common H.264 NAL unit types (type = header_byte & 0x1F).
const char *h264_nal_name(uint8_t type)
{
	switch (type) {
	case 1:
		return "slice";
	case 5:
		return "IDR";
	case 6:
		return "SEI";
	case 7:
		return "SPS";
	case 8:
		return "PPS";
	case 9:
		return "AUD";
	default:
		return nullptr;
	}
}

// Locate the next Annex-B start code (00 00 01 or 00 00 00 01) at or after `from`.
// Returns its offset (or n if none) and writes the start-code length (3 or 4).
size_t find_start_code(const uint8_t *d, size_t n, size_t from, int *sc_len)
{
	for (size_t i = from; i + 3 <= n; ++i) {
		if (d[i] == 0 && d[i + 1] == 0) {
			if (d[i + 2] == 1) {
				*sc_len = 3;
				return i;
			}
			if (i + 4 <= n && d[i + 2] == 0 && d[i + 3] == 1) {
				*sc_len = 4;
				return i;
			}
		}
	}
	return n;
}

// Read-only: describe an Annex-B access unit as "sc=4B nals=[9(AUD)/2 7(SPS)/45 ...]".
std::string describe_annexb(const uint8_t *d, size_t n)
{
	if (!d || n < 4)
		return "empty";

	int sc = 0;
	size_t p = find_start_code(d, n, 0, &sc);
	if (p == n)
		return "no start code (not Annex-B?)";

	char head[24];
	std::snprintf(head, sizeof(head), "sc=%dB nals=[", sc);
	std::string out = head;

	bool has_sei = false;
	int count = 0;
	while (p < n && count < 16) {
		const int this_sc = (p + 3 <= n && d[p] == 0 && d[p + 1] == 0 && d[p + 2] == 1) ? 3 : 4;
		const size_t nal = p + static_cast<size_t>(this_sc);
		if (nal >= n)
			break;
		const uint8_t type = d[nal] & 0x1F;
		if (type == 6)
			has_sei = true;

		int dummy = 0;
		const size_t next = find_start_code(d, n, nal, &dummy);
		const size_t len = next - nal;

		const char *name = h264_nal_name(type);
		char buf[48];
		if (name)
			std::snprintf(buf, sizeof(buf), "%u(%s)/%zu ", type, name, len);
		else
			std::snprintf(buf, sizeof(buf), "%u/%zu ", type, len);
		out += buf;

		p = next;
		++count;
	}
	out += "]";
	if (has_sei)
		out += " (existing SEI present)";
	return out;
}

// Glass-to-glass SEI UUID agreed with the player team (see docs/sei-glass-to-glass.md).
const uint8_t kSeiUuid[16] = {0x08, 0x2c, 0x96, 0x8f, 0x77, 0x5e, 0x49, 0xb4,
			      0x8d, 0x55, 0xa4, 0x29, 0xec, 0xd9, 0x89, 0xd5};

// Insert emulation_prevention_three_byte (0x03) so the RBSP can't contain a
// start-code pattern (00 00 00/01/02/03).
std::vector<uint8_t> epb_escape(const uint8_t *rbsp, size_t n)
{
	std::vector<uint8_t> out;
	out.reserve(n + n / 32 + 4);
	int zeros = 0;
	for (size_t i = 0; i < n; ++i) {
		const uint8_t b = rbsp[i];
		if (zeros >= 2 && b <= 0x03) {
			out.push_back(0x03);
			zeros = 0;
		}
		out.push_back(b);
		zeros = (b == 0) ? zeros + 1 : 0;
	}
	return out;
}
} // namespace

void PacketProbe::attach(obs_output_t *output)
{
	if (attached_ || !output)
		return;
	obs_output_add_packet_callback(output, &PacketProbe::thunk, this);
	attached_ = true;
	haveWindow_ = false;
	seq_ = 0;
	{
		std::lock_guard<std::mutex> lk(statsMtx_);
		latest_ = ProbeStats{};
	}

	// Our SEI is H.264-specific; only inject when the video codec is H.264.
	obs_encoder_t *venc = obs_output_get_video_encoder(output);
	const char *codec = venc ? obs_encoder_get_codec(venc) : nullptr;
	codecIsH264_ = codec && strcmp(codec, "h264") == 0;

	obs_log(LOG_INFO, "probe: packet callback attached (frame timings; codec=%s)", codec ? codec : "?");
	if (injectEnabled_ && codecIsH264_)
		obs_log(LOG_INFO, "probe: SEI glass-to-glass injection ENABLED (PoC)");
	else if (injectEnabled_)
		obs_log(LOG_INFO, "probe: SEI injection skipped (codec not h264)");
}

void PacketProbe::detach(obs_output_t *output)
{
	if (!attached_ || !output)
		return;
	obs_output_remove_packet_callback(output, &PacketProbe::thunk, this);
	attached_ = false;
	{
		std::lock_guard<std::mutex> lk(statsMtx_);
		latest_ = ProbeStats{};
	}
	obs_log(LOG_INFO, "probe: packet callback detached");
}

void PacketProbe::thunk(obs_output_t *, struct encoder_packet *pkt, struct encoder_packet_time *pkt_time, void *param)
{
	static_cast<PacketProbe *>(param)->onPacket(pkt, pkt_time);
}

ProbeStats PacketProbe::stats() const
{
	std::lock_guard<std::mutex> lk(statsMtx_);
	return latest_;
}

void PacketProbe::resetWindow(uint64_t now_ns)
{
	windowStart_ = now_ns;
	loggedFrameNal_ = false;
	loggedKeyNal_ = false;
	loggedInjectThisWindow_ = false;
	frames_ = 0;
	keyframes_ = 0;
	bytes_ = 0;
	c2eSum_ = 0;
	c2eMin_ = 0;
	c2eMax_ = 0;
	c2eCount_ = 0;
	r2sSum_ = 0;
	r2sCount_ = 0;
	encSum_ = 0;
	encCount_ = 0;
	p2iSum_ = 0;
	p2iCount_ = 0;
	haveWindow_ = true;
}

void PacketProbe::onPacket(struct encoder_packet *pkt, const struct encoder_packet_time *pkt_time)
{
	// Only video packets carry the render/encode timing we care about.
	if (!pkt || pkt->type != OBS_ENCODER_VIDEO)
		return;

	// Use PIR (packet-interleave time) as the monotonic window clock; fall back
	// to CTS if PIR is unset.
	uint64_t now = 0;
	if (pkt_time) {
		now = pkt_time->pir ? pkt_time->pir : pkt_time->cts;
	}

	if (!haveWindow_)
		resetWindow(now);

	++frames_;
	if (pkt->keyframe)
		++keyframes_;
	bytes_ += pkt->size;

	if (pkt_time) {
		if (pkt_time->pir && pkt_time->cts && pkt_time->pir >= pkt_time->cts) {
			const uint64_t c2e = pkt_time->pir - pkt_time->cts;
			c2eSum_ += c2e;
			if (c2eCount_ == 0 || c2e < c2eMin_)
				c2eMin_ = c2e;
			if (c2e > c2eMax_)
				c2eMax_ = c2e;
			++c2eCount_;
		}
		// Sub-intervals: render -> submit, encode, encoded -> interleave.
		if (pkt_time->fer && pkt_time->cts && pkt_time->fer >= pkt_time->cts) {
			r2sSum_ += pkt_time->fer - pkt_time->cts;
			++r2sCount_;
		}
		if (pkt_time->ferc && pkt_time->fer && pkt_time->ferc >= pkt_time->fer) {
			encSum_ += pkt_time->ferc - pkt_time->fer;
			++encCount_;
		}
		if (pkt_time->pir && pkt_time->ferc && pkt_time->pir >= pkt_time->ferc) {
			p2iSum_ += pkt_time->pir - pkt_time->ferc;
			++p2iCount_;
		}
	}

	// Read-only NAL structure sample (rate-limited to ~1 line/sec per kind).
	if (pkt->data && pkt->size >= 4) {
		if (pkt->keyframe && !loggedKeyNal_) {
			loggedKeyNal_ = true;
			obs_log(LOG_INFO, "probe/nal keyframe AU %zu B: %s", pkt->size,
				describe_annexb(pkt->data, pkt->size).c_str());
		} else if (!pkt->keyframe && !loggedFrameNal_) {
			loggedFrameNal_ = true;
			obs_log(LOG_INFO, "probe/nal frame AU %zu B: %s", pkt->size,
				describe_annexb(pkt->data, pkt->size).c_str());
		}
	}

	if (now && now - windowStart_ >= kWindowNs)
		flush(now);

	// SEI injection (PoC), last: from here pkt->data/size are replaced.
	if (injectEnabled_ && codecIsH264_ && pkt->data && pkt->size > 0) {
		// Map the frame's monotonic composition/egress times to wall-clock UTC.
		const uint64_t mono_now = os_gettime_ns();
		const uint64_t wall_now =
			static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
						      std::chrono::system_clock::now().time_since_epoch())
						      .count());
		auto to_wall = [&](uint64_t x) -> uint64_t {
			return (x && mono_now >= x) ? wall_now - (mono_now - x) : 0;
		};
		// Frame encode time = composition wall-clock (cts); fall back to now.
		const uint64_t t_capture_ns = (pkt_time && to_wall(pkt_time->cts)) ? to_wall(pkt_time->cts) : wall_now;
		const uint64_t t_encode_ms = t_capture_ns / 1000000ULL;

		// Time spent inside the source before egress (composition -> egress),
		// a duration in the same monotonic domain (no wall-clock mapping needed).
		uint64_t source_latency_ms = 0;
		if (pkt_time && pkt_time->pir && pkt_time->cts && pkt_time->pir >= pkt_time->cts)
			source_latency_ms = (pkt_time->pir - pkt_time->cts) / 1000000ULL;

		injectSei(pkt, t_encode_ms, source_latency_ms);

		if (!loggedInjectThisWindow_) {
			loggedInjectThisWindow_ = true;
			obs_log(LOG_INFO,
				"probe/inject encodeTimestamp=%llu ms sourceLatency=%llu ms | AU now %zu B: %s",
				static_cast<unsigned long long>(t_encode_ms),
				static_cast<unsigned long long>(source_latency_ms), pkt->size,
				describe_annexb(pkt->data, pkt->size).c_str());
		}
	}
}

void PacketProbe::injectSei(struct encoder_packet *pkt, uint64_t tEncodeMs, uint64_t sourceLatencyMs)
{
	// --- User data = UUID(16) + the agreed JSON string (see docs) ---
	// NOTE: "sourceLatencyMs" key name is our addition — confirm with the player team.
	char json[128];
	const int jlen = std::snprintf(json, sizeof(json),
				       "{\"messageId\":\"encodeTimestamp\",\"payload\":%llu,\"sourceLatencyMs\":%llu}",
				       static_cast<unsigned long long>(tEncodeMs),
				       static_cast<unsigned long long>(sourceLatencyMs));
	if (jlen <= 0)
		return;
	const size_t payload_size = 16 + static_cast<size_t>(jlen);

	// --- SEI RBSP: payloadType(5) + payloadSize + UUID + JSON + rbsp_trailing(0x80) ---
	std::vector<uint8_t> rbsp;
	rbsp.reserve(4 + payload_size + 1);
	rbsp.push_back(0x05); // payloadType = user_data_unregistered
	for (size_t s = payload_size; s >= 255; s -= 255)
		rbsp.push_back(0xff);
	rbsp.push_back(static_cast<uint8_t>(payload_size % 255));
	rbsp.insert(rbsp.end(), kSeiUuid, kSeiUuid + 16);
	rbsp.insert(rbsp.end(), json, json + jlen);
	rbsp.push_back(0x80); // trailing 0x80 (rbsp_trailing_bits; also the transcoder-padding byte)

	const std::vector<uint8_t> esc = epb_escape(rbsp.data(), rbsp.size());

	// --- Assemble Annex-B NAL: [00 00 00 01][0x06][escaped RBSP], prepended to the AU ---
	const size_t sei_len = 4 + 1 + esc.size();
	const size_t new_size = sei_len + pkt->size;

	// Match OBS's packet-data allocation: a `long` refcount immediately precedes
	// the data pointer (see obs_encoder_packet_release), so the later release
	// frees the whole block correctly.
	void *block = bmalloc(sizeof(long) + new_size);
	*reinterpret_cast<long *>(block) = 1;
	uint8_t *nd = reinterpret_cast<uint8_t *>(block) + sizeof(long);

	size_t o = 0;
	nd[o++] = 0x00;
	nd[o++] = 0x00;
	nd[o++] = 0x00;
	nd[o++] = 0x01; // 4-byte start code
	nd[o++] = 0x06; // SEI NAL header (nal_ref_idc=0, type=6)
	std::memcpy(nd + o, esc.data(), esc.size());
	o += esc.size();
	std::memcpy(nd + o, pkt->data, pkt->size);

	// Release our reference to the original data (mirror obs_encoder_packet_release).
	long *orig_refs = reinterpret_cast<long *>(pkt->data) - 1;
	if (os_atomic_dec_long(orig_refs) == 0)
		bfree(orig_refs);

	pkt->data = nd;
	pkt->size = new_size;
}

void PacketProbe::flush(uint64_t now_ns)
{
	const double elapsed_s = ns_to_ms(now_ns - windowStart_) / 1000.0;
	const double kbps = elapsed_s > 0.0 ? (static_cast<double>(bytes_) * 8.0 / 1000.0) / elapsed_s : 0.0;

	if (c2eCount_ > 0) {
		const double r2s = r2sCount_ > 0 ? ns_to_ms(r2sSum_ / static_cast<uint64_t>(r2sCount_)) : 0.0;
		const double enc = encCount_ > 0 ? ns_to_ms(encSum_ / static_cast<uint64_t>(encCount_)) : 0.0;
		const double p2i = p2iCount_ > 0 ? ns_to_ms(p2iSum_ / static_cast<uint64_t>(p2iCount_)) : 0.0;
		const double total = ns_to_ms(c2eSum_ / static_cast<uint64_t>(c2eCount_));
		obs_log(LOG_INFO,
			"probe: %d frames (%d kf) %.0f kb/s | in-OBS avg %.1f ms [render->submit %.1f | encode %.1f | ->interleave %.1f] min %.1f max %.1f ms",
			frames_, keyframes_, kbps, total, r2s, enc, p2i, ns_to_ms(c2eMin_), ns_to_ms(c2eMax_));

		ProbeStats s;
		s.valid = true;
		s.inObsMs = total;
		s.renderToSubmitMs = r2s;
		s.encodeMs = enc;
		s.toInterleaveMs = p2i;
		s.minMs = ns_to_ms(c2eMin_);
		s.maxMs = ns_to_ms(c2eMax_);
		s.fps = elapsed_s > 0.0 ? frames_ / elapsed_s : 0.0;
		s.kbps = kbps;
		{
			std::lock_guard<std::mutex> lk(statsMtx_);
			latest_ = s;
		}
	} else {
		obs_log(LOG_INFO, "probe: %d frames (%d kf) %.0f kb/s | timing fields unavailable", frames_, keyframes_,
			kbps);
	}

	resetWindow(now_ns);
}

} // namespace beacon
