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

#include <obs.h>

#include <cstddef>
#include <cstdint>
#include <mutex>

namespace beacon {

// Last completed 1s window of encoded-packet timing, published for the UI.
struct ProbeStats {
	bool valid = false;
	double inObsMs = 0.0;          // total pir - cts (avg)
	double renderToSubmitMs = 0.0; // fer - cts (avg)
	double encodeMs = 0.0;         // ferc - fer (avg)
	double toInterleaveMs = 0.0;   // pir - ferc (avg)
	double minMs = 0.0;            // min total
	double maxMs = 0.0;            // max total
	double fps = 0.0;
	double kbps = 0.0;
};

/*
 * Read-only probe on the streaming output's encoded-packet stream.
 *
 * Registers via obs_output_add_packet_callback, which fires in send_interleaved()
 * just before packets are handed to the RTMP service. It does NOT modify packets
 * — it only reads the per-frame timing fields (encoder_packet_time: cts/fer/ferc/
 * pir) and logs a once-per-second summary of the OBS-internal latency
 * (composite -> egress) and encode time.
 *
 * This is the groundwork/attach point for later SEI-based glass-to-glass timing;
 * for now it just proves the hook behaves and surfaces the timings.
 *
 * Threading: the callback runs synchronously on the output's send thread (a
 * single thread per output), so the accumulators need no locking. attach/detach
 * happen on the UI thread alongside the output reference lifecycle.
 */
class PacketProbe {
public:
	PacketProbe() = default;
	~PacketProbe() = default;
	PacketProbe(const PacketProbe &) = delete;
	PacketProbe &operator=(const PacketProbe &) = delete;

	void attach(obs_output_t *output);
	void detach(obs_output_t *output);

	// Thread-safe snapshot of the last completed window (for the UI thread).
	ProbeStats stats() const;

private:
	static void thunk(obs_output_t *output, struct encoder_packet *pkt, struct encoder_packet_time *pkt_time,
			  void *param);
	void onPacket(struct encoder_packet *pkt, const struct encoder_packet_time *pkt_time);

	// PoC: prepend the glass-to-glass SEI to an H.264 video AU (in place).
	// tEncodeMs       — frame encode/composition time as a Unix ms timestamp.
	// sourceLatencyMs — time spent inside the source (composition -> egress), ms.
	void injectSei(struct encoder_packet *pkt, uint64_t tEncodeMs, uint64_t sourceLatencyMs);

	void resetWindow(uint64_t now_ns);
	void flush(uint64_t now_ns);

	bool attached_ = false;

	// SEI-injection PoC state.
	bool injectEnabled_ = true; // PoC: on by default; logged at attach
	bool codecIsH264_ = false;  // only H.264 gets our (H.264-specific) SEI
	uint32_t seq_ = 0;          // monotonic frame counter carried in the SEI
	bool loggedInjectThisWindow_ = false;

	// Per-second accumulator window (send-thread only).
	bool haveWindow_ = false;
	uint64_t windowStart_ = 0; // pir ns of first frame in window
	// Read-only NAL structure logging, rate-limited to one line per window.
	bool loggedFrameNal_ = false;
	bool loggedKeyNal_ = false;
	int frames_ = 0;
	int keyframes_ = 0;
	uint64_t bytes_ = 0;

	// Total composite -> egress latency (pir - cts), ns.
	uint64_t c2eSum_ = 0;
	uint64_t c2eMin_ = 0;
	uint64_t c2eMax_ = 0;
	int c2eCount_ = 0;

	// Sub-intervals, ns:
	uint64_t r2sSum_ = 0; // render -> encoder submit   (fer - cts)
	int r2sCount_ = 0;
	uint64_t encSum_ = 0; // encode                     (ferc - fer)
	int encCount_ = 0;
	uint64_t p2iSum_ = 0; // encoded -> interleave/egress (pir - ferc)
	int p2iCount_ = 0;

	// Published snapshot, read from the UI thread.
	mutable std::mutex statsMtx_;
	ProbeStats latest_;
};

} // namespace beacon
