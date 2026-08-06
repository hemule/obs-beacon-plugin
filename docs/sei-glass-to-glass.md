# Glass-to-glass timing over in-stream SEI — format spec (v1)

**Status:** v1, format agreed with the player team. Implemented as a PoC in `PacketProbe`
(H.264, per-frame injection) and verified to keep the stream playable.

Purpose: let a viewer's player compute **glass-to-glass latency** (time from a frame being
formed in OBS to it being displayed to the viewer) by carrying source timestamps *inside
the video bitstream*, so they travel with the exact frame they describe.

## Pipeline and what we timestamp

```
[capture / composite]  frame rendered inside OBS         ← near "glass"   (t_capture)
     │  inside OBS: render → encode → interleave  (~200 ms measured on a test setup)
[egress]               packet leaves OBS toward network  ← OBS↔network     (t_egress)
     │  network → ingest server → CDN → player buffer → decode
[present / display]    frame shown to the viewer         ← far "glass"    (t_present, player)
```

- **t_capture** — wall-clock (UTC) when the frame was composited in OBS. Derived from the
  encoder's composition timestamp (`encoder_packet_time.cts`, a monotonic `os_gettime_ns`)
  mapped to wall-clock (see *Plugin responsibilities*).
- **t_egress** — wall-clock when the encoded packet is interleaved/handed to the RTMP output
  (`encoder_packet_time.pir`), i.e. the moment it leaves OBS for the network.
- **t_present** — measured by the *player* when it displays the frame (its own synced clock).

The player computes:

| Metric | Formula |
|---|---|
| Glass-to-glass (composite → display) | `t_present − t_encode` |

**v1 carries only the encode timestamp** (`t_encode` ≈ `t_capture`), i.e. the total
glass-to-glass number. Splitting it into "inside OBS" vs "delivery" would need a second
timestamp (`t_egress`); that is a possible future extension, not in v1.

## Carrier: SEI user_data_unregistered

- **H.264/AVC:** an SEI NAL unit (`nal_unit_type = 6`), payload `payloadType = 5`
  (`user_data_unregistered`). Per the H.264 spec this payload starts with a mandatory
  16-byte `uuid_iso_iec_11578`, followed by arbitrary user bytes — that is where our schema
  lives.
- **H.265/HEVC:** SEI prefix NAL (`nal_unit_type = 39`), same `payloadType = 5`.
- **AV1:** use a `metadata_obu` of type `METADATA_TYPE_ITUT_T35` or a private metadata type
  instead of SEI (AV1 has no SEI). Out of scope for the first PoC (test encoder is H.264).

At the point the plugin injects (OBS `obs_output_add_packet_callback`, before the FLV muxer),
H.264 packets are in **Annex-B** form (start-code-delimited NAL units), so injection is a
matter of adding one more start-code NAL — no length-prefix rewriting.

## UUID (format tag)

```
082c968f-775e-49b4-8d55-a429ecd989d5
bytes: 08 2c 96 8f 77 5e 49 b4 8d 55 a4 29 ec d9 89 d5
```

- **Fixed constant, identical for every user and every stream.** It is not data — it is a
  magic tag identifying this timing SEI. Agreed with the player team.
- The player MUST match this UUID before parsing, and **ignore SEI with any other UUID**.
  Streams legitimately contain other `user_data_unregistered` SEI (e.g. VideoToolbox and x264
  emit their own); matching this UUID avoids mis-parsing them.

## Payload

The SEI `user_data_unregistered` payload is the 16-byte UUID followed by the **UTF-8 bytes of
this JSON string**:

```
{"messageId":"encodeTimestamp","payload":<ms>,"sourceLatencyMs":<n>}
```

- `<ms>` (`payload`) — the frame encode time as a **Unix timestamp in milliseconds** (a bare
  JSON number, not quoted). In this plugin it is the composition wall-clock
  (`encoder_packet_time.cts` mapped to UTC ms; see *Plugin responsibilities*).
- `<n>` (`sourceLatencyMs`) — time the frame spent **inside the source** (composition → egress
  = `pir − cts`), in ms. Deliberately source-neutral (not OBS-specific). Lets the player
  attribute how much of glass-to-glass is the source's own pipeline vs delivery:
  `delivery = (t_present − payload) − sourceLatencyMs`. It is a duration (same monotonic clock
  domain), so no clock sync is needed to interpret it.
- Example: `{"messageId":"encodeTimestamp","payload":1753894503123,"sourceLatencyMs":198}`.

> `sourceLatencyMs` is the Beacon plugin's addition; the exact key name is pending confirmation
> with the player team. Parsers that only read `messageId`/`payload` ignore it (extra JSON
> keys are non-breaking).

### Full SEI NAL unit (H.264, Annex-B)

```
00 00 00 01                 4-byte start code
06                          NAL header (nal_ref_idc=0, nal_unit_type=6 = SEI)
05                          payloadType = 5 (user_data_unregistered)
<payloadSize>               = 16 + len(JSON), ff-encoded if ≥ 255
<16-byte UUID><JSON bytes>  the payload
80                          trailing byte
```

- **Trailing `0x80`** doubles as the H.264 `rbsp_trailing_bits` and the transcoder-padding
  byte the player team asked for (static for now; they will provide real transcoder padding
  rules later).
- Standard **emulation-prevention** (`00 00 03`) is applied over the RBSP; the UUID and ASCII
  JSON contain no `00 00` sequences, so in practice nothing is escaped.
- Overhead ≈ 75 bytes/frame (~2.2 KB/s at 30 fps) — negligible.

## Player-side requirements

1. **Extract** the SEI `user_data_unregistered` NAL (H.264 NAL type 6 / HEVC prefix SEI 39),
   match the UUID, then parse the remaining payload bytes as the JSON string above and read
   `payload` (ms). Ignore SEI with other UUIDs.
2. **Timestamp display**: record `t_present` as close to on-screen as the pipeline allows, and
   **document which stage** was measured (decode-complete / submitted-to-render / vsync) — the
   semantics change how the number is read.
3. **Clock sync**: the device clock and the OBS host must share a time reference (NTP minimum,
   PTP better). The absolute latency error equals the clock offset between them.
4. **Passthrough delivery**: a transcoding server/CDN re-encodes video and **strips SEI**. This
   scheme requires a passthrough path (RTMP→remux, WebRTC passthrough, …). If HLS/DASH with
   repackaging is used, verify SEI survives; otherwise consider `prft` (Producer Reference
   Time) or ID3 timed metadata inserted at packaging instead.
5. **Tolerate gaps**: not every frame need carry SEI (sampling, or a transcode dropped it) —
   associate by `seq` / `pts`.

## Plugin-side responsibilities

- **Wall-clock mapping**: `cts`/`pir` are `os_gettime_ns()` (monotonic, not UTC). The plugin
  maps them once per packet: `wall_ns(x) = wall_now_ns − (mono_now_ns − x)`, so the numbers are
  comparable across machines.
- **Injection cadence**: per-frame is fine (overhead negligible); sampling at 1–10 Hz is also
  acceptable for monitoring. `seq`/`pts` let the player associate regardless.
- **Codec**: first PoC targets H.264 (Annex-B). HEVC/AV1 carriers to follow.

## Caveats

- SEI is stripped by any re-encode; end-to-end survival requires a passthrough path.
- Absolute numbers are only as good as clock sync.
- `t_capture` is the OBS composition time, not the true camera-sensor time; and `t_present` is
  the player's display time, not the true panel photons. Calibrate those two physical ends once
  via a visual burn-in test if absolute accuracy matters.

## Appendix: observed encoder structure (read-only probe)

Measured live on the test setup (macOS, VideoToolbox H.264, ~30 fps), for reference when
implementing injection:

- Format is **Annex-B**. Start-code width is **mixed**: keyframe AUs use 4-byte
  (`00 00 00 01`), P-frame AUs use 3-byte (`00 00 01`). The injector should emit a 4-byte
  start code (always valid) regardless.
- **Keyframe AU:** `SPS(7) · PPS(8) · SEI(6) · IDR(5)` — no AUD(9). **VideoToolbox already
  emits its own SEI**, so the stream carries other `user_data_unregistered` SEI; the player's
  UUID match is what disambiguates ours.
- **P-frame AU:** `slice(1)` optionally followed by `filler_data(12)` (CBR padding).
- **Insertion point:** prepend our SEI NAL at the **start of the access unit** (before SPS on
  keyframes / before the slice on P-frames), with a 4-byte start code. SEI then correctly
  precedes the primary coded picture. Injecting on every frame reaches P-frames too.
- In-OBS latency stayed ~200 ms (render ~37 / encode ~1 / interleave ~165) even at ~10 Mbps,
  i.e. the interleave buffering is bitrate-independent (a fixed delay, not encode cost).
