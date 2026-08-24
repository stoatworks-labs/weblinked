# Notes

Working notes for this repo: status, decisions, and the traps that have actually bitten.
Migrated out of Claude Code's memory on 2026-08-24, so they are written in the first
person and dated by when each thing was learned — that date is usually the useful part.

Cross-cutting notes that are not specific to this repo live in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).

*WebLinked — CEF-based app rendering a URL to SDI (DeckLink/AJA) and NDI/OMT; what is verified vs compile-only*

**WebLinked** — small cross-platform app: a URL in, broadcast video out. Chromium
(CEF 150) rendered offscreen at a real raster/rate → DeckLink + AJA over SDI,
NDI + OMT over network. C++20/CMake. `~/Projects/weblinked`, GitHub **PUBLIC MIT**
(stoatworks-labs/weblinked). **v0.7.0 released 2026-08-01** — installers for macOS
(.dmg/.pkg), Windows (.exe/.zip) and Linux (.tar.gz), built by GitHub Actions.

**A `stream` output (RTMP/SRT) added 2026-08-10, uncommitted** — encodes the
page to H.264/AAC through an `ffmpeg` subprocess fed over two loopback TCP
sockets, so WebLinked can feed a Restreamer/datarhei Core or a YouTube ingest.
`--stream=<url>` + `--bitrate`, or `/api/output/add {"kind":"stream"}`. Built
for atem-overseer's browser source type. **Verified end to end** against a file
target (page renders, 439 Hz tone, A/V within 27 ms over 25 s, 0 dropped) **and
against a real datarhei Core 16.0.0** over the tailnet (0 drop at the Core,
`audio_deficit_ms: 0`). Untested: YouTube/Twitch ingests, SRT, reconnect. Argument building lives in `core/stream_args`
so it is unit-tested; see docs/04-verification.md §28 and
[ffmpeg subprocess traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_ffmpeg_subprocess_traps.md) for the three faults it found.

**Shipped in v0.7.0:**
- **A `screen` output** — fullscreen on a GPU-attached display, `--screen[=n]`
  plus `--scaling fit|fill|stretch`. An `IOutput` like the preview, so it shares
  the frame path; a native AppKit/Metal window, **never** a second CEF browser
  (that crashes the GPU process — see [cef traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_cef_traps.md)). Paced by the
  display, so `presented` != `frames` **by design**. Now verified **across two
  heads** (ASUS PA148 as a second display): 50.1 submitted/s, 60.1 presented/s,
  ratio 1.199 vs a theoretical 1.200. Win/Linux written, never run.
  Traps in [macos native output window](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_macos_native_output_window.md).
- **The single-display bug this feature shipped with**: `-[NSWindow initWith…
  screen:]` takes a rect *relative to that screen's origin* while `-[NSScreen
  frame]` is global, so passing one to the other applied the offset twice and the
  window landed off the desktop on **every display except the main one**. A
  one-display machine could not see it. Every in-process number looked perfect —
  open() succeeded, the CVDisplayLink ticked at the right rate, `presented`
  climbed correctly. Found only by screenshotting both displays. Fixed by
  creating with `screen:nil` then `-setFrame:display:`.
- **The launcher now carries WebLinked inside it**, reversing what
  `launcher/README.md` used to argue. Ships as ONE zip (Tauri's collector dies
  on the CEF framework's symlinks otherwise), unpacked to Application Support on
  first run — **never nested**, which is what dodges the Gatekeeper helper-kill.
  Measured: the ad-hoc signature survives `ditto` intact, no quarantine is
  applied, helpers run — so **no re-signing is needed** and the `codesign
  --deep` step was removed. Launcher .app 3.8 MB → ~142 MB.

**CI artefacts ship OMT only** — hosted runners have no NDI or DeckLink SDK and
`find_package` silently disables them, so a *downloaded* build cannot do NDI at
all. Build locally with the SDKs for the full set. Only the macOS build has ever
been **run**; Windows and Linux compile and nothing more.

**Multi-source shipped 2026-07-31** (unreleased, on `main` after v0.3.0):
`SourceManager` runs N independent Engines from `--config <file>`; HTTP takes
`?source=<id>` and OSC `/weblinked/source/<id>/<verb>`, both defaulting to the
primary so every v0.3.0 client still works. **Capacity is a property of the
machine, not a setting** — 3 sources at 1080p50+720p50+1080p25 drop ~4% on this
M4 Max (receiver measured 46.27 fps against 50, ~900% CPU on 16 cores), while 3
at 720p25 drop 0.26%. Always measure on the show machine.

**Field proven as of 2026-08-02** — Allan has run it on a live event (his own
report, not a session observation). That retires the old "no projector, never
out of the lab" line; the per-subsystem gaps below are unaffected and still hold.

**Verified vs assumed — the distinction to keep:**
- **NDI + preview: verified end to end** on this Mac against `tools/ndi_probe.cpp`
  (an independent receiver). Colour bars match a separate BT.709 reference
  exactly; audio at 59.94 measured at mean 800.8020 samples/frame (79×800 +
  320×801); 3-min soak = 9001 ticks in 180 s, **zero dropped ticks**.
- **DeckLink: verified on real hardware** (Duo 2) — colour via loopback capture
  against an independent BT.709 reference, a 2-min soak holding 6 frames of
  buffer with zero dropped ticks, and key+fill measured as *straight* alpha (the
  premultiply bug that was real on NDI is absent on SDI). Still unmeasured: the
  key channel itself, audio over SDI, genlock over hours.
- **AJA (libajantv2 18.1) and OMT (libomt 1.0.0.16): compile against real SDK
  headers, never touched hardware or a receiver.**
- **Windows/Linux: built by CI, but only macOS has ever been run.**
`docs/04-verification.md` is the authority — never upgrade "compiles" to "works".

**Local SDK sources that made this possible:** DeckLink SDK headers are NOT
installed standalone but exist on this Mac inside the NDI SDK examples
(`/Library/NDI SDK for Apple/examples/C++/NDIlib_Send_BMD/BMDSDK/Mac/include`,
v10.11) and in **Unreal 5.7's BlackmagicMedia ThirdParty (v12.2)** — the latter is
what the build uses via `-DDECKLINK_SDK_DIR`. libajantv2 is MIT on GitHub, shallow
clone is 49 MB.

**Design decisions worth not re-litigating:** our clock drives Chromium via
`SendExternalBeginFrame` (1 tick = 1 paint) rather than `windowless_frame_rate`;
frame rates are exact rationals everywhere (59.94 = 60000/1001), deadlines
computed from tick zero never accumulated; the operator window is just a CEF
window showing the same control page the HTTP server serves, so there is **no GUI
toolkit at all**; the preview is an output so it shares the frame path.

**Cross-platform build traps, all found by the first CI release run:** never
`enable_language(OBJCXX)` (it makes CMake compile CEF's own .mm files as OBJCXX
and CEF only sets CXX flags); Windows needs `NOMINMAX`, `CMAKE_MSVC_RUNTIME_LIBRARY`
= `/MT` to match CEF's wrapper, `dbghelp` for the crash backtrace, and the console
subsystem (`WIN32` demands WinMain); `libomt.h` has a `#pragma comment(lib, ...)`
that needs `/NODEFAULTLIB:libomt.lib`; Linux and Windows must link libcef itself
while macOS must not; `SetAsWindowless` takes `kNullWindowHandle`, not `nullptr`.

**Always `rm -rf build` before tagging.** An incremental build keeps the CEF
wrapper's objects, so anything that breaks CEF's own compilation passes locally
and fails on CI.

**Two silent-failure classes this project keeps producing:**
- **OSC string padding** — `padded()` already counts the mandatory NUL, so
  passing `textLength + 1` over-advances for any string 3 mod 4 and the *whole
  message* is dropped with no log line. Shipped in v0.3.0: `/weblinked/url`
  worked for most URLs and did nothing for a quarter of them. Now covered by
  `tests/test_osc_server.cpp`, and `osc_server.cpp` moved into `weblinked_core`
  because the decoder previously sat in the CEF-linked target where no cheap
  test could reach it.
- **`web_assets.h` has no build step** — one stray byte (a NUL inside
  `join(' ')`) compiles, serves 200, renders the HTML, and kills every line of
  JS after it; the page sits on "connecting" with a black preview. A NUL also
  makes `grep` treat the file as binary and return *nothing*, which sends the
  hunt the wrong way. Diagnose by running the page's own script text through
  `new Function()` in a browser — it names the line at once.

**Two CI traps the embedded launcher introduced (both hit at v0.7.0):**
- **Never let the RPM bundler recompress the embed.** The launcher now carries a
  337 MB WebLinked on Linux, and Tauri's RPM bundler assembles in memory with
  gzip-6 by default — the Linux launcher `Build` step went from **38 s to ~100
  min**. The payload is already-compressed archives, so it bought 14 MB. Fixed
  with `bundle.linux.rpm.compression = {"type":"none"}` → **52 s**. macOS (dmg)
  and Windows (nsis) were unaffected: half the payload, one format each.
- **`download-artifact` with no name takes *every* artifact**, so the
  `weblinked-embed-*` archives that exist only to feed the launcher job got
  published as release assets — v0.7.0 briefly shipped `WebLinked.app.zip`
  (135 MB) and `WebLinked.zip` (339 MB) beside the real installers, and
  `gen-downloads.py` classified them as "Source tarball" and would have offered
  them on the website. The release job now `rm -rf artifacts/weblinked-embed-*`
  before uploading.

See [cef traps](https://github.com/stoatworks-labs/fleet-notes/blob/main/notes/reference_cef_traps.md) for the CEF/macOS traps this project hit.
