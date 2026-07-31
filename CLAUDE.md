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
./build/tests/weblinked_tests          # all
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

## Rules

1. **Never claim a backend works because it compiles.** Only NDI and preview are
   verified. DeckLink, AJA and OMT compile against real SDKs and have never
   touched hardware. `docs/04-verification.md` is the authority — update it with
   commands and output, not ticks.
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
9. **Popups are never allowed to become windows.** A windowless browser cannot
   own one; `OnBeforePopup` always cancels. See the trap in `AGENTS.md`.
10. **Anything the clock thread reads outside `Engine::mutex_` is atomic** —
    `matrix_`, `pacing_`. The settings page can write both at run time.
11. **Build clean (`rm -rf build`) before tagging a release.** An incremental
   build reuses the CEF wrapper's objects, so anything that breaks CEF's own
   compilation passes locally and fails on CI.
12. Commit means commit **and push**.

## Layout

| Path | What |
|---|---|
| `src/core/` | formats, frames, pools, clock, audio FIFO, JSON, dlopen — no CEF |
| `src/diag/` | logging, crash reports, diagnostics bundles |
| `src/browser/` | CefApp, CefClient (paint + audio), BrowserSource |
| `src/engine/` | the clock loop |
| `src/outputs/` | IOutput + preview, ndi, omt, decklink, aja |
| `src/control/` | HTTP server, OSC receiver, embedded control page |
| `src/app/` | entry points, Info.plists, entitlements |
| `launcher/` | av-launcher tray shell (Rust/Tauri); separate build |
| `tools/` | `ndi_probe` — independent receiver for verification |
