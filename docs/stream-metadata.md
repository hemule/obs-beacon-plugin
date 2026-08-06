# Beacon stream metadata — server-side reference

What the Beacon OBS plugin injects into an outgoing RTMP stream, so the server can
find and parse it. There are **two independent channels**:

| Channel | When | Carries | Survives transcode? |
|---|---|---|---|
| **1. RTMP `connect` user_arguments (AMF)** | once, at connect — before any media frame and before the `publishv2` webhook | client identity + session id | n/a (connect-time) |
| **2. H.264 SEI (per frame)** | every video frame | per-frame timing for glass-to-glass | **no** — a re-encode strips it; requires a passthrough path |

Both are added only when the stream is sent through the Beacon output (the plugin's
custom service/output). A stock OBS stream contains neither.

---

## Channel 1 — RTMP connect `user_arguments` (AMF0)

**Where:** the AMF `connect` command's *optional user arguments* (RTMP spec §7.2.1.1) —
a single anonymous **AMF0 object** appended right after the connect command object. It
arrives in the very first RTMP exchange, before `createStream`/`publish` and before any
media. (Stock OBS/librtmp sends no user arguments here; this object is plugin-specific.)

**Object members:**

| Key | AMF type | Value | Meaning |
|---|---|---|---|
| `app` | String (0x02) | `obs-beacon` | client type (OBS plugin) |
| `streamSessionId` | String (0x02) | GUID, e.g. `1725ca6b-1f65-4b29-b306-44d1b4c7e9e7` | id of this streaming session |

**`streamSessionId` semantics (important):** minted once when the user starts streaming
and kept **identical across automatic reconnects** of that session; a **new** GUID is
generated on the next manual start. So the server can tell a *reconnect of an existing
session* (same id) from a *new session* (new id).

**Exact bytes on the wire** (verified; 87 bytes, AMF0):

```
03                                                  object marker
  00 03 61 70 70                                    key "app"
  02 00 13 70 6C 61 73 6D 61 73 74 72 65 61 6D 69 6E 67 2E 6F 62 73   string "obs-beacon"
  00 0F 73 74 72 65 61 6D 53 65 73 73 69 6F 6E 49 64   key "streamSessionId"
  02 00 24 <36 ASCII bytes of the GUID>             string <guid>
00 00 09                                            object end
```

(`00 LL …` = 16-bit-length-prefixed member name; `02 00 LL …` = AMF0 string; the object is
terminated by `00 00 09`.)

---

## Channel 2 — H.264 SEI `user_data_unregistered` (per frame)

**Where:** an SEI NAL unit (H.264 `nal_unit_type = 6`), `payloadType = 5`
(`user_data_unregistered`), inside the video elementary stream. The plugin prepends it at
the start of every access unit. In the RTMP/FLV video payload the NAL is length-prefixed
(AVCC) after muxing; the SEI content bytes are the same. **Codec:** H.264 today (HEVC/AV1
would use their own metadata carriers — not yet implemented).

**Payload = 16-byte UUID + UTF-8 JSON, then a trailing `0x80`:**

- **UUID (identifies our SEI):** `082c968f-775e-49b4-8d55-a429ecd989d5`
  (bytes: `08 2c 96 8f 77 5e 49 b4 8d 55 a4 29 ec d9 89 d5`).
  The stream also contains the encoder's own `user_data_unregistered` SEI — **match this
  UUID** and ignore others.
- **JSON string** (immediately after the 16 UUID bytes):

  ```json
  {"messageId":"encodeTimestamp","payload":<ms>,"sourceLatencyMs":<n>}
  ```

| Field | Type | Meaning |
|---|---|---|
| `messageId` | string | always `"encodeTimestamp"` |
| `payload` | number | frame **encode/composition time**, Unix timestamp in **milliseconds (UTC)** |
| `sourceLatencyMs` | number | time the frame spent **inside the source** (composition → egress), ms — lets you split source vs delivery latency |

**Glass-to-glass** at the player: `t_displayed − payload`. **Delivery-only** (network +
server + player): `(t_displayed − payload) − sourceLatencyMs`. Requires the source and
consumer clocks to share a time reference (NTP/PTP).

**SEI NAL structure:**

```
06                          NAL header (nal_ref_idc=0, type 6 = SEI)
05                          payloadType = 5 (user_data_unregistered)
<payloadSize>               = 16 + byte-length(JSON); ff-encoded if ≥ 255 (typically one byte ~0x5A)
<16-byte UUID><JSON bytes>  the payload
80                          trailing byte (rbsp_trailing_bits; also a transcoder-padding placeholder)
```

**Parsing notes:**
- Standard H.264 **emulation-prevention** (`00 00 03`) may be inserted in the SEI RBSP;
  strip `0x03` after any `00 00` when reading (normal SEI parsing). Our current payload
  (this UUID + ASCII JSON) contains no `00 00`, so in practice none is inserted — but a
  robust parser should handle it.
- The trailing `0x80` is intentional (per the player-team request for transcoder padding);
  real transcoder-padding rules TBD.

---

## Status / caveats

- Field/key names (`streamSessionId`, `sourceLatencyMs`) originate on the plugin side —
  please confirm they match the server's expectations; renaming is trivial.
- SEI is stripped by any server-side re-encode; end-to-end SEI survival needs a passthrough
  path. The connect AMF is unaffected (it's connect-time).
- Values verified on the wire from the plugin; server-side *reading* of both channels is
  the remaining confirmation.
