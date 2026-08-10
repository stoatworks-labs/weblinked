# CLAUDE.md — WebLinked working reference

Commands and the rules that bite. Read `AGENTS.md` for the mental model behind
them.

## Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

With the optional SDKs:

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDECKLINK_SDK_DIR="/path/to/Blackmagic DeckLink SDK 12.9" \
  -DWEBLINKED_WITH_AJA=ON -DNTV2_DIR=/path/to/libajantv2 -DNTV2_BUILD_DIR=/path/to/ajantv2-build
```

CEF is downloaded on first configure into `third_party/cef/current` (gitignored).

## Test

```bash
./build/tests/weblinked_tests          # all (89 tests)
./build/tests/weblinked_tests uyvy     # substring filter
```

Tests link `weblinked_core` only — no CEF — and run in seconds.

## Run

```bash
./build/Release/WebLinked.app/Contents/MacOS/WebLinked \
  --url https://example.com --format 1080p50 --ndi=Test --headless
```

`--help` lists every flag and which backends this build contains.

Control page: <http://127.0.0.1:7654/>. Log:
`~/Library/Logs/WebLinked/WebLinked.log`, or set `WEBLINKED_LOG_DIR`.
`WEBLINKED_LOG=debug` raises the level.

## Verify (do not skip this)

The app's own counters only prove it *sent* something. Check what arrived:

```bash
clang++ -std=c++20 -I"/Library/NDI SDK for Apple/include" tools/ndi_probe.cpp \
  "/Library/NDI SDK for Apple/lib/macOS/libndi.dylib" \
  -Wl,-rpath,"/Library/NDI SDK for Apple/lib/macOS" -o ndi_probe

./ndi_probe --source Test --frames 100 --bars
```

`--bars` checks the rendered colour bars against an independent BT.709 reference.

The shared (Syphon) output has its own receiver, which links **Resolume Arena's
bundled Syphon 5 framework** rather than the Syphon 6 sources this repo vendors
— so a pass is two implementations agreeing, not one agreeing with itself:

```bash
clang++ -std=c++20 -fobjc-arc -Wno-deprecated-declarations tools/syphon_probe.mm \
  -F "/Applications/Resolume Arena/Arena.app/Contents/Frameworks" \
  -framework Syphon -framework Foundation -framework IOSurface \
  -framework OpenGL -framework Cocoa \
  -rpath "/Applications/Resolume Arena/Arena.app/Contents/Frameworks" -o syphon_probe

./syphon_probe --list                             # discovery
./syphon_probe --source WLTest --alphabars        # colour + premultiplied alpha
./syphon_probe --source WLTest --orientation      # needs tools/updown.html
```

Start the probe *after* the server — a server that only answers its opening
announce is the failure this catches. `--alphabars` and `--orientation` are
separate checks and neither covers the other.

DeckLink needs a card, an SDI loopback and the SDK. Unreal's bundled copy works
if you do not have the SDK archive — `FindDeckLinkSDK.cmake` accepts its layout:

```bash
SDK="/path/to/Blackmagic DeckLink SDK/Mac/include"
for t in dl_profile dl_scan sdi_probe; do
  clang++ -std=c++17 -fobjc-arc -I"$SDK" "$SDK/DeckLinkAPIDispatch.cpp" \
    tools/$t.mm -framework CoreFoundation -o $t
done

./dl_profile        # profile, duplex, keying and which rates carry alpha
./dl_scan           # which inputs are receiving, no-source counted separately
./sdi_probe 3       # capture on an index and check the bars; add 1080p25/bands
```

**Run `dl_profile` first and believe nothing you remember.** Desktop Video
profiles persist and change how many sub-devices exist, whether each is half or
full duplex, and whether there is a keyer at all — so a connector map is only
true for the profile it was measured in. Section 19 of `docs/04-verification.md`
records one profile and 19a another, on the same card.

The Windows half (Spout) has its own pair, because WebLinked does not build on
Windows and so cannot be the sender. `tools/spout_send_test.cpp` drives the real
backend directly; run from a PowerShell prompt on the Windows machine:

```powershell
.\tools\build_spout_test.ps1 -Repo C:\wl -Out C:\wl\out
.\out\spout_send_test.exe --pattern alphabars --seconds 60
.\out\spout_probe.exe --source WLTest --pattern alphabars
```

This verifies the *output* and nothing above it. See `docs/04-verification.md`
section 24 for exactly what that does and does not establish.

## Settings and the launcher

```bash
# Settings file (macOS). --settings <file> or $WEBLINKED_SETTINGS overrides it;
# --no-settings ignores it. The command line always beats the file.
~/Library/Application Support/WebLinked/settings.json

# The tray launcher is a separate Cargo project and does NOT build with the rest
cd launcher && cargo test --manifest-path src-tauri/Cargo.toml
cd launcher && npm install && npm run tauri dev     # needs the Tauri CLI
AV_LAUNCHER_CONFIG=launchers/dev.toml npm run tauri dev   # against ./build
```

## Release

Two products come out of this repo and they are not interchangeable: the tray
launcher, which carries the engine inside it and is what a person should
download, and the engine on its own for a machine driven from a command line.

Published engine artefacts are named from `scripts/release-file-slug`
(`weblinked-engine`), which `rl_init` reads — do **not** also export
`RL_FILE_SLUG`, because the variable beats the file. `RL_SLUG` stays `weblinked`
and must: it is the Windows uninstall registry key, so changing it gives
everyone who already installed a second Add/Remove Programs entry.

## Rules

1. **Never claim a backend works because it compiles.** NDI, preview, DeckLink
   and now the shared/Syphon output are verified against real receivers,
   hardware or Resolume Arena itself. AJA and OMT compile against real SDKs and
   have never touched either. `docs/04-verification.md` is the authority —
   update it with commands and output, not ticks. There are three independent
   receivers for exactly this: `tools/ndi_probe.cpp`, `tools/sdi_probe.mm` and
   `tools/syphon_probe.mm` — the first two carrying their own BT.709 reference,
   the third linking a *different vendor's* Syphon, so a check is not a
   restatement of the code under test.
2. **Frame rates stay exact rationals.** No `double` rates, no accumulated
   periods. 59.94 is `60000/1001`.
3. **`weblinked_core` must not depend on CEF.** It is also compiled without CEF's
   `-fno-exceptions`, which is why the parsers that use `try`/`catch` live there.
4. **Signal and crash handlers go after `CefInitialize`.** Chromium replaces
   anything registered earlier.
5. **Never convert a frame into a buffer sized for a different raster.** Both
   guards — the stale-frame drop in `clockLoop` and the refusal in
   `frameInFormat` — must stay.
6. **Clock-thread state is rebuilt only while the clock is parked** via
   `pauseClock()`.
7. **`FramePool` is always held by `shared_ptr`** (`FramePool::create`).
8. **macOS bundles are signed inside-out, ad-hoc without hardened runtime.** See
   `cmake/SignMacBundle.cmake` before changing anything there.
9. **No browser in this process may own a window.** A windowless browser cannot
   own a popup; `OnBeforePopup` always cancels. Every browser here is windowless,
   which forces Alloy runtime style, and a windowed browser would default to
   Chrome style and segfault the GPU process. See `docs/04-verification.md`
   section 9. The `screen` output *is* a real platform window, and is not an
   exception to this: it is a video destination, not a UI and not a browser —
   it draws frames the engine already produced. Keep it that way.
10. **Anything the clock thread reads outside `Engine::mutex_` is atomic** —
    `matrix_`, `pacing_`. The settings page can write both at run time.
11. **Build clean (`rm -rf build`) before tagging a release.** An incremental
   build reuses the CEF wrapper's objects, so anything that breaks CEF's own
   compilation passes locally and fails on CI.
12. **An engine is only ever reached through `SourceManager::withSource`.** It
    holds a shared lock for the call, which is what stops a removal destroying
    an engine while an HTTP or OSC request is inside it. `Engine` reads
    `browser_` without a lock, so a bare `Engine*` from a lookup is a
    use-after-free waiting for an operator to remove a source mid-show.
13. **The control page has no build step**, so a syntax error anywhere in
    `web_assets.h` kills every line after it and the page just sits on
    "connecting". If it looks dead, run its own script text through
    `new Function()` in a browser — it names the line at once.
14. **An advertisement must be true, or absent.** WebLinked refuses to publish
    an mDNS record while the control API is bound to loopback, and withdraws
    the record before the sockets close. A record that outlives its port, or
    points at an address a browser cannot reach, is worse than never having
    advertised — it fails on someone else's machine, silently. See
    `mdns::bindIsAdvertisable` and `ControlApi::stop`.
15. **The mDNS TXT record is identity and reachability, never live state.**
    Adding a source count or a URL to it means rewriting the record at poll
    rate, multicast to the whole subnet. State belongs on HTTP.
16. Commit means commit **and push**.

## Layout

| Path | What |
|---|---|
| `src/core/` | formats, frames, pools, clock, audio FIFO, JSON, dlopen, mDNS (`mdns_service*`) — no CEF |
| `src/diag/` | logging, crash reports, diagnostics bundles |
| `src/browser/` | CefApp, CefClient (paint + audio), BrowserSource |
| `src/engine/` | the clock loop |
| `src/outputs/` | IOutput + preview, ndi, omt, decklink, aja, screen, shared, stream |
| `third_party/syphon/` | vendored Syphon server subset (BSD-3) — see its README |
| `third_party/spout/` | vendored Spout DirectX sender subset (BSD-2) — see its README |
| `src/control/` | HTTP server, OSC receiver, embedded control page |
| `src/app/` | entry points, Info.plists, entitlements |
| `launcher/` | av-launcher tray shell (Rust/Tauri); separate build |
| `tools/` | independent receivers (`ndi_probe`, `sdi_probe`, `syphon_probe`, `spout_probe`, `mdns_probe`) plus the DeckLink diagnostics `dl_profile` / `dl_scan`, and the HTML test pages |
