# WebLinked

**A URL in. SDI and video-over-IP out.**

WebLinked renders a web page offscreen at a broadcast raster and frame rate, then
sends it to Blackmagic DeckLink and AJA cards over SDI, and to the network as
[NDI](https://ndi.video) and [OMT](https://openmediatransport.org). Point it at a
scoreboard, a lower-third, a countdown clock, a dashboard or a whole HTML
playback page, and it becomes a source your vision mixer can cut to.

Small on purpose: one binary, no service to install, no framework in the browser.
It is driven from a control page, over HTTP, or over OSC from Companion or a
show-control system.

```bash
weblinked --url https://example.com/scoreboard --format 1080p50 --ndi=Scoreboard
```

---

## What it does

- **Renders any URL** through Chromium (CEF), offscreen, at the exact raster and
  rate you ask for — 1080p50, 720p59.94, 1080i25, 2160p30, or an arbitrary
  raster like `3840x600p60` for an LED strip.
- **Carries the page's audio** — WebAudio, `<video>`, anything Chromium plays —
  as 48 kHz float, embedded in SDI or sent alongside the IP video.
- **Outputs to four kinds of destination at once**: DeckLink, AJA, NDI, OMT.
  Every output takes the same frame, so what NDI sees is what SDI sees.
- **Frame-accurate pacing.** The engine's clock drives Chromium one frame at a
  time (`SendExternalBeginFrame`) rather than letting the browser paint on its
  own timer, so 50 ticks a second means 50 paints a second.
- **Alpha**, where the destination supports it: NDI and OMT can carry the page's
  transparency for a downstream keyer.
- **Live control**: change the URL, reload, run JavaScript in the page, change
  raster, or stop and start an output mid-show, from the control page, HTTP, or
  OSC.

## What it does not do

- No compositing, no layers, no playlist. It renders one page. If you need two
  graphics at once, that is what the page is for.
- No capture or input. Output only.
- No genlock. The engine's clock is a software clock; an SDI card's own scheduled
  playback absorbs the difference. See
  [docs/01-architecture.md](docs/01-architecture.md) for where that boundary sits
  and what it costs.

---

## Status, honestly

| Backend | State |
|---|---|
| **NDI** | **Verified end to end.** A real receiver confirms raster, rate, pixel format, colour and audio. |
| **Preview** | Verified — it is the control page's confidence monitor. |
| **OMT** | Compiles against `libomt.h` 1.0.0.16. **Never tested against an OMT receiver.** |
| **DeckLink** | Compiles against DeckLink SDK 12.2. **Never run against a card.** |
| **AJA** | Compiles against libajantv2 18.1. **Never run against a card.** Off by default. |

Read [docs/04-verification.md](docs/04-verification.md) before trusting any of
this on air. It records exactly what was measured, how, and what was not.

Verified on macOS 26.4 / Apple Silicon. Windows and Linux are written for and
build-configured, but have not been built or run — see
[docs/02-building.md](docs/02-building.md).

---

## Quick start

```bash
git clone https://github.com/stoatworks-labs/weblinked
cd weblinked
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

CMake downloads the CEF binary distribution on first configure (~125 MB) and
caches it in `third_party/cef`. Nothing else is required to build: the NDI and
OMT backends resolve their libraries at run time, and the SDI SDKs are optional.

Then:

```bash
./build/Release/WebLinked.app/Contents/MacOS/WebLinked \
  --url https://example.com/graphic.html \
  --format 1080p50 \
  --ndi=Graphic
```

The operator window opens on the control page. Add `--headless` to run it as a
service and drive it over HTTP or OSC instead.

`--help` lists every option and reports which backends this build contains.

## Control

The control page is served at `http://127.0.0.1:7654/` and is the same page the
operator window shows — so a phone on the same network is a remote panel.

```bash
curl -X POST -d '{"url":"https://example.com/next"}' http://127.0.0.1:7654/api/url
curl -X POST -d '{"ignore_cache":true}'              http://127.0.0.1:7654/api/reload
curl -X POST -d '{"name":"Graphic","enabled":false}' http://127.0.0.1:7654/api/output
```

Over OSC, on port 7655 — the shape a Companion button sends:

```
/weblinked/url          "https://example.com/next"
/weblinked/reload       1
/weblinked/output/Graphic  0
/weblinked/script       "showLowerThird('Anna Kowalski')"
```

`/weblinked/script` is the useful one: a graphic that already exposes a function
can be driven without any integration work at all.

Full reference: [docs/03-control-api.md](docs/03-control-api.md).

**The control surface has no authentication by default** and binds to loopback.
If you bind it to a network interface, set `--token`, and understand that anyone
who can reach the port can change what is on air.

## Requirements

- **NDI**: the NDI runtime or SDK installed. Not redistributable, so WebLinked
  loads it at run time; without it the NDI output reports itself unavailable and
  everything else keeps working.
- **OMT**: `libomt` and `libvmx` beside the binary, from the
  [OMT releases](https://github.com/openmediatransport/libomtnet/releases).
  Published for Windows x64/arm64 and macOS arm64 only — there is no Linux
  binary.
- **DeckLink**: Desktop Video installed, plus the DeckLink SDK at build time
  (`-DDECKLINK_SDK_DIR=...`).
- **AJA**: [libajantv2](https://github.com/aja-video/libajantv2) 18.x at build
  time (`-DNTV2_DIR=... -DNTV2_BUILD_DIR=...`) and `-DWEBLINKED_WITH_AJA=ON`.

## Documentation

- [docs/00-overview.md](docs/00-overview.md) — what this is and who it is for
- [docs/01-architecture.md](docs/01-architecture.md) — the pipeline, and the
  decisions that shaped it
- [docs/02-building.md](docs/02-building.md) — building on each platform, and the
  macOS bundle traps
- [docs/03-control-api.md](docs/03-control-api.md) — HTTP and OSC reference
- [docs/04-verification.md](docs/04-verification.md) — what has actually been
  proven, and how to reproduce it
- [docs/diagnostics.md](docs/diagnostics.md) — logs, crash reports, bundles
- [AGENTS.md](AGENTS.md) — orientation for an AI assistant or a new contributor

## Licence

MIT — see [LICENSE](LICENSE).

Third-party components keep their own licences. `third_party/omt/libomt.h` is MIT
(Open Media Transport contributors). The CEF binary distribution, the NDI SDK,
the DeckLink SDK and libajantv2 are each obtained separately under their own
terms; none of them are redistributed here.

**NDI® is a registered trademark of Vizrt NDI AB.** This project is not
affiliated with or endorsed by Vizrt, Blackmagic Design, or AJA Video Systems.

---

*Built with AI assistance (Claude). The NDI path is verified against real
software; the SDI and OMT paths have never touched hardware or a receiver.
Review it before you trust it with a show.*
