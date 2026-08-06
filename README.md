# Beacon

An OBS Studio plugin that (1) surfaces **RTMP/SRT connection
diagnostics** in a dockable panel and (2) optionally routes streaming through a bundled
RTMP output that injects **stream metadata** (client identity, session id, and
glass-to-glass timing) for the server and player to consume.

The diagnostics part is a pure **observer** of the streaming output and the libobs log.
The metadata part is opt-in via a dock toggle — when off, OBS streams exactly as stock.

## Features

- **Connection status dock** — state machine `IDLE → CONNECTING → LIVE → RECONNECTING →
  (LIVE | FAILED)` with the ingest URL, duration, bitrate, network congestion, dropped
  frames, reconnect count, and the classified stop reason / `last_error` on failure.
- **Connection log** — a bounded (2000-line) ring buffer of the libobs log, filtered to
  RTMP/SRT-relevant lines (plus all warnings/errors), in a separate on-demand window
  with a "connection only" toggle and copy/clear.
- **OBS-internal latency probe** — reads per-frame encode timings from the streaming
  output (`obs_output_add_packet_callback`) and reports the composite→egress latency
  broken into `render→submit / encode / →interleave`.
- **Stream metadata injection (opt-in)** — a "Route streaming through Beacon" toggle wraps
  the current service so streaming runs through the bundled Beacon RTMP output, which adds:
  - **RTMP connect `user_arguments` (AMF):** `app` (client type) + `streamSessionId` (a GUID
    stable across OBS reconnects, new on each Stop/Start) — sent at connect, before media.
  - **Per-frame H.264 SEI:** glass-to-glass timing (`encodeTimestamp` + `sourceLatencyMs`).

  Wire formats are documented for the server team in
  [docs/stream-metadata.md](docs/stream-metadata.md) (and the SEI design in
  [docs/sei-glass-to-glass.md](docs/sei-glass-to-glass.md)).
- Localised (en-US, en-GB, ru-RU).

## Requirements

- **OBS Studio 31.1+ / 32.x** to run the prebuilt binary. It is built against Qt 6.8
  (obs-deps), so it loads on OBS with Qt ≥ 6.8 via Qt backward compatibility; on OBS 31.0.x
  or older (Qt 6.6) it fails with `Symbol not found: _qt_version_tag_6_8`. Verified on OBS
  32.2.1 (Qt 6.11). (The dock API `obs_frontend_add_dock_by_id` itself needs OBS 30+.)
- Build tooling: **CMake 3.30+** and **Ninja**; macOS needs **Xcode**.

## Build (macOS)

```bash
cmake --preset macos
cmake --build --preset macos
```

The configure step downloads pinned dependencies (OBS sources, prebuilt obs-deps, Qt6)
into `.deps/` per `buildspec.json`. The built bundle is at
`build_macos/RelWithDebInfo/obs-beacon.plugin`.

### macOS 26 (Tahoe) SDK note

The macOS 26 SDK removed the legacy **AGL** framework that prebuilt Qt6 still references,
which otherwise breaks linking (`ld: framework 'AGL' not found`). This is handled
automatically at configure time by `cmake/macos/patch-qt-agl.cmake` (stripping AGL from
the fetched Qt6 deps) — no manual steps, and it survives `rm -rf .deps` / a fresh clone.
No effect on older SDKs or CI.

## Install (macOS)

```bash
cp -R "build_macos/RelWithDebInfo/obs-beacon.plugin" \
  "$HOME/Library/Application Support/obs-studio/plugins/"
```

Restart OBS, then enable the panel via **Docks → Beacon**. Open the log with
the **Open log…** button in the dock.

## Stream metadata / routing through the Beacon output

The `app` / `streamSessionId` (AMF) and glass-to-glass SEI are only injected when streaming
runs through the plugin's **Beacon RTMP output**. To enable it, tick **Route streaming
through Beacon** in the dock: the plugin wraps the current stream service (keeping its
server/key) in a `beacon_service` whose `get_output_type` points OBS at `beacon_rtmp_output`.

Notes:

- The setting **persists** across restarts (it is saved in the profile's `service.json`), so
  it is a one-time action per profile — not a per-session button.
- OBS's **Settings → Stream** dropdown cannot list a third-party service type, so the Beacon
  service is set programmatically. Editing the service there turns routing **off** (re-tick to
  restore); with routing on, Settings shows the service as "Beacon" but leaves the Server
  field blank (an OBS UI limitation for unknown service types — cosmetic).
- Untick to revert to a plain `rtmp_custom` service (stock output, no injection).

## Testing

Use a local RTMP server such as [MediaMTX](https://github.com/bluenviron/mediamtx) and
exercise the scenarios: successful stream (LIVE + counters), unreachable server
(`CONNECT_FAILED`), server killed mid-stream (`RECONNECTING` → reconnect/disconnect).
The connection log and the OBS log (`~/Library/Application Support/obs-studio/logs/`)
show the state transitions and RTMP-level lines.

> Note: OBS itself can freeze the UI on **Stop Streaming** while reconnecting to a dead
> ingest — it blocks in `obs_output_force_stop` → `pthread_join` on the RTMP thread stuck
> in a blocking `connect()`. This is an OBS behaviour, independent of this plugin
> (confirmed via stack sampling with the plugin removed).

## Architecture

| Component | File | Role |
|---|---|---|
| `LogCapture` | `src/log-capture.*` | Thread-safe libobs log interception + connection filter (chains the previous handler) |
| `ConnectionMonitor` | `src/connection-monitor.*` | Frontend events + output signals → connection state machine; owns the output reference lifecycle |
| `PacketProbe` | `src/packet-probe.*` | Encoded-packet timing probe (OBS-internal latency) **and** per-frame SEI injection |
| `DiagnosticsDock` | `src/diagnostics-dock.*` | Compact Qt status dock + "Route through Beacon" toggle |
| `LogWindow` | `src/log-window.*` | Separate on-demand log window |
| Beacon output + service | `src/beacon-rtmp.c`, `src/rtmp-output/` | Registers `beacon_rtmp_output` (a vendored copy of OBS's stock RTMP output, plain RTMP, that injects the AMF connect metadata) and a `beacon_service` that routes streaming through it |
| bridge / lifecycle | `src/beacon.*`, `src/plugin-main.c` | C module entry point → C++ components + registration |

`src/rtmp-output/` is a vendored copy of OBS's `obs-outputs` RTMP path (rtmp-stream + flv-mux
+ librtmp + happy-eyeballs), built `NO_CRYPTO` (plain RTMP, no TLS/SWF → no external deps),
with `.id` renamed and a small addition to put `app`/`streamSessionId` into the connect
`Link.extras`. OBS's own output is untouched. See the memory note for the maintenance caveat
(the vendored RTMP copy must track OBS security/feature updates).

Threading: frontend events and UI refresh run on the Qt UI thread; the log handler and
output signals run on arbitrary threads and only touch mutex-guarded state.

## Status & roadmap

- Diagnostics prototype (status + log + dock + latency probe) — **done, verified live**.
- Glass-to-glass SEI injection (`encodeTimestamp` + `sourceLatencyMs`) — **done, verified on
  the wire**; server/player-side reading is the remaining confirmation.
- RTMP connect AMF (`app` + `streamSessionId`, stable across reconnects) via the vendored
  Beacon output — **done, verified live**; server-side reading pending on the real ingest.
- Planned: an upstream obs-outputs hook so a service can supply connect AMF without the fork
  (would let us drop `src/rtmp-output/`); connection telemetry upload; account auto-setup
  that provisions the Beacon service/profile programmatically.

## Known limitations / TODO

- **Cross-platform: builds on all three platforms (CI-verified); runtime-tested only
  on macOS.** GitHub CI green-lights **macOS**, **Ubuntu/Linux**, and **Windows**
  builds (the plugin is a frontend plugin using cross-platform OBS + Qt APIs, plus a
  vendored RTMP output built `NO_CRYPTO`). Only macOS (universal arm64 + x86_64) has
  been exercised at runtime — Windows/Linux are confirmed to *compile and link* but
  the diagnostics/routing paths have not been run there yet.

## License

GPLv2 — this plugin links against libobs. See [LICENSE](LICENSE).

Scaffolded from [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate);
its build system, CI workflows, and Codesigning/CI docs still apply.
