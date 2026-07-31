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

# A keyed graphic: alpha over NDI, and key + fill out of a DeckLink
weblinked --url https://example.com/lower-third --alpha --ndi=LowerThird --key --decklink=0

# Several at once, each with its own browser, clock, raster and outputs
weblinked --config show.json
```

---

[![Watch it running — 63 seconds](docs/video-thumb.png)](https://www.youtube.com/watch?v=3qG3wPPjUjY)

*A 63-second tour of the real app, driven over its own HTTP control API. The output is
NDI, picked up by a separate receiver (`tools/ndi_probe`) — no DeckLink or AJA card
appears, because none has ever been connected. See
[docs/04-verification.md](docs/04-verification.md) for what is verified and what is not.*

![The WebLinked control page, rendering github.com to NDI at 1080p50](docs/images/control-page.png)

*The control page, driving two sources. It is the whole UI — WebLinked serves it
over HTTP and any browser on the network can reach it. The strip along the top carries a live thumbnail per
source and only appears when there is more than one, so a single-source launch
looks exactly as it always did. The preview is fed by the same frame pipeline as
the SDI and NDI outputs, so if the preview is right, the outputs are right.*

| Settings | Diagnostics |
|---|---|
| [![The settings page](docs/images/settings-page.png)](docs/images/settings-page.png) | [![The diagnostics page](docs/images/diagnostics-page.png)](docs/images/diagnostics-page.png) |
| Outputs, with only the fields each backend has. Saved to a file the next launch reads back. | The live log at a level you can change while the fault is happening, and a bundle that downloads. |

Both images below are **real 1920×1080 frames received over NDI** by a separate
receiver (`tools/ndi_probe`), converted back to RGB — not screenshots of a
browser. They are what a vision mixer would have seen.

| A live page | A time-of-day clock |
|---|---|
| ![github.com rendered to 1080p50 and received over NDI](docs/images/output-github.png) | ![A clock page rendered to 1080p50 and received over NDI](docs/images/output-clock.png) |
| source `site` | source `clock` |

Both were on the network **at the same time**, from the one process that served
the control page above — which is what the two chips in its strip are.

`tools/clock.html` ships with the repo. It is a useful thing to point at a feed:
a clock is the quickest way to see at a glance whether a signal is live, and its
sub-second progress bar makes dropped frames visible to the naked eye.

Regenerate all five with [`tools/screenshots.sh`](tools/screenshots.sh).

---

<!-- downloads:start -->

## Download

**[v0.5.2](https://github.com/stoatworks-labs/weblinked/releases/tag/v0.5.2)** — prebuilt for macOS, Windows and Linux. Pick your platform:

<details>
<summary><b>macOS</b> — Apple Silicon</summary>

| Build | Download | Size |
| --- | --- | --- |
| Apple Silicon · .dmg disk image | [`weblinked-0.5.2-macos-arm64.dmg`](https://github.com/stoatworks-labs/weblinked/releases/download/v0.5.2/weblinked-0.5.2-macos-arm64.dmg) | 159 MB |
| Apple Silicon · .pkg installer | [`weblinked-0.5.2-macos-arm64.pkg`](https://github.com/stoatworks-labs/weblinked/releases/download/v0.5.2/weblinked-0.5.2-macos-arm64.pkg) | 142 MB |

</details>

<details>
<summary><b>Windows</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .exe installer | [`weblinked-0.5.2-windows-x86_64-setup.exe`](https://github.com/stoatworks-labs/weblinked/releases/download/v0.5.2/weblinked-0.5.2-windows-x86_64-setup.exe) | 133 MB |
| x64 · .zip archive | [`weblinked-0.5.2-windows-x86_64.zip`](https://github.com/stoatworks-labs/weblinked/releases/download/v0.5.2/weblinked-0.5.2-windows-x86_64.zip) | 178 MB |

</details>

<details>
<summary><b>Linux</b> — x64</summary>

| Build | Download | Size |
| --- | --- | --- |
| x64 · .tar.gz archive | [`weblinked-0.5.2-linux-x86_64.tar.gz`](https://github.com/stoatworks-labs/weblinked/releases/download/v0.5.2/weblinked-0.5.2-linux-x86_64.tar.gz) | 356 MB |

</details>

All builds, checksums and release notes: [github.com/stoatworks-labs/weblinked/releases](https://github.com/stoatworks-labs/weblinked/releases).

<!-- downloads:end -->

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
- **Alpha, properly.** NDI and OMT carry the page's transparency, and a DeckLink
  can output **key + fill** on separate SDI connectors (or key internally over
  its own input). Chromium composites premultiplied and every destination here
  expects straight alpha, so WebLinked un-premultiplies — without that, soft
  edges, drop shadows and fades all render too dark on a keyer.
- **An interactive preview.** The control page's preview forwards clicks,
  scrolling and typing to the live page, which is the only practical way to
  dismiss a cookie banner, close a modal or sign in on a machine whose browser
  you cannot otherwise reach. **On by default** (`--no-interactive` turns it
  off), and always outlined when armed, because it is the on-air output. A link
  that asks for a new tab loads in place rather than opening a window — a
  windowless browser cannot own one.
- **A settings page.** Add, edit, remove, start and stop outputs from the
  browser, with only the fields each backend actually has; change raster,
  colour matrix and pacing; and save the lot to a file the next launch reads
  back. Anything given on the command line still wins over the file.
- **Diagnostics in the app.** The live log with the level changeable while the
  fault is happening, a crash report on demand, and a one-file diagnostics
  bundle that **downloads** rather than naming a path on a machine you are not
  sitting at.
- **Live control**: change the URL, reload, run JavaScript in the page, change
  raster, or stop and start an output mid-show, from the control page, HTTP, or
  OSC.
- **Several sources in one process.** `--config show.json` starts any number of
  independent pipelines — each its own browser, clock, raster and outputs — and
  they share nothing but Chromium's process. A page that hangs, a card that will
  not open or a raster change on one source cannot touch the others. The control
  page grows a strip along the top with a live thumbnail per source; HTTP verbs
  take `?source=<id>` and OSC takes `/weblinked/source/<id>/<verb>`. Leave the id
  off and the request goes to the first source, so everything that drove a
  single-source WebLinked still does.

  How many a machine can carry is a property of that machine, not a setting.
  Three 1080p sources drop frames on an M4 Max; three at 720p25 do not. Measure
  it where the show will run — [docs/04-verification.md](docs/04-verification.md)
  has the numbers and the method.
- **A tray launcher** ([`launcher/`](launcher/)) for a machine where WebLinked
  should come up at login and live in the menu bar rather than in a terminal.

## What it does not do

- No compositing, no layers, no playlist. Each source renders one page. If you
  need two graphics in one picture, that is what the page is for; if you need two
  pictures, that is what a second source is for.
- No capture or input. Output only.
- No genlock. The engine's clock is a software clock; an SDI card's own scheduled
  playback absorbs the difference. See
  [docs/01-architecture.md](docs/01-architecture.md) for where that boundary sits
  and what it costs.

---

## Status, honestly

| Backend | State |
|---|---|
| **NDI** | **Verified end to end**, including alpha. A real receiver confirms raster, rate, pixel format, colour and audio. |
| **Preview** | Verified, and interactive — it is the control page's confidence monitor. |
| **Several sources** | **Verified**: three at once on three rasters, each confirmed by a separate receiver, with one retargeted and a fourth added and removed mid-run without disturbing the others. Frame rates are a capacity question — see below. |
| **Settings + diagnostics pages** | Verified against a running instance: outputs added, renamed and removed, settings saved and read back after a restart, the log and bundle served. |
| **Tray launcher** | Builds; its config is unit-tested against the file it ships. **Not clicked through against a live WebLinked** — see [launcher/README.md](launcher/README.md). |
| **OMT** | Compiles against `libomt.h` 1.0.0.16. **Never tested against an OMT receiver.** |
| **DeckLink** | Compiles against DeckLink SDK 12.2, key + fill included. **Never run against a card.** |
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

WebLinked opens no window of its own. It is a render host and a control server:
the UI is the control page it serves, which you open in a browser, drive over
HTTP or OSC, or reach from the [tray launcher](launcher/) — a menu-bar icon that
starts and stops the process and opens the page for you. That is why there is no
GUI toolkit anywhere in this project.

`--help` lists every option and reports which backends this build contains.

## Control

The control page is served at `http://127.0.0.1:7654/`, so a phone on the same
network is a remote panel.

```bash
curl -X POST -d '{"url":"https://example.com/next"}' http://127.0.0.1:7654/api/url
curl -X POST -d '{"ignore_cache":true}'              http://127.0.0.1:7654/api/reload
curl -X POST -d '{"name":"Graphic","enabled":false}' http://127.0.0.1:7654/api/output
# Click at the centre of the page, in normalised coordinates
curl -X POST -d '{"type":"down","nx":0.5,"ny":0.5}'  http://127.0.0.1:7654/api/input

# With several sources, ?source= picks one; without it you get the first
curl http://127.0.0.1:7654/api/sources
curl -X POST -d '{"url":"https://example.com/next"}' \
     'http://127.0.0.1:7654/api/url?source=lower-third'
```

Over OSC, on port 7655 — the shape a Companion button sends:

```
/weblinked/url          "https://example.com/next"
/weblinked/reload       1
/weblinked/output/Graphic  0
/weblinked/script       "showLowerThird('Anna Kowalski')"

# One feed by name, when several are running
/weblinked/source/lower-third/url  "https://example.com/next"
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
- [docs/05-settings.md](docs/05-settings.md) — the settings page and the file it
  writes
- [docs/06-ndi-distribution.md](docs/06-ndi-distribution.md) — why NDI is loaded
  at run time, what the licence permits, and the attribution every project owes
- [launcher/README.md](launcher/README.md) — the menu-bar tray launcher
- [AGENTS.md](AGENTS.md) — orientation for an AI assistant or a new contributor

## Licence

MIT — see [LICENSE](LICENSE).

Third-party components keep their own licences. `third_party/omt/libomt.h` is MIT
(Open Media Transport contributors). The CEF binary distribution, the NDI SDK,
the DeckLink SDK and libajantv2 are each obtained separately under their own
terms; none of them are redistributed here.

**NDI® is a registered trademark of Vizrt NDI AB.** See <https://ndi.video>. The
NDI runtime is loaded at run time and is not redistributed here — see
[docs/06-ndi-distribution.md](docs/06-ndi-distribution.md) for why, and for what
the licence does and does not permit. NDI Tools are not redistributed either;
get them from <https://ndi.video/tools>.

This project is not affiliated with or endorsed by Vizrt, Blackmagic Design, or
AJA Video Systems.

---

*Built with AI assistance (Claude). The NDI path is verified against real
software; the SDI and OMT paths have never touched hardware or a receiver.
Review it before you trust it with a show.*
