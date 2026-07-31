# AGENTS.md — bringing an LLM up to speed on WebLinked

Orientation for an AI assistant (or a new human) picking this project up cold.

`CLAUDE.md` is the working reference — commands and the rules that bite. **Read
it too.** This file gives you the mental model those rules protect, so you can
tell which ones are load-bearing and why.

---

## 1. What this is

A **cross-platform application that renders a URL to broadcast video**: Chromium
offscreen at a real raster and frame rate, out to DeckLink and AJA over SDI and
to NDI and OMT over the network. C++20, CMake, CEF. Public repo, MIT.

The domain in one paragraph: a **page** is rendered offscreen at a **format**
(raster + exact rational rate + scan type). A **clock** ticks at that rate and
asks the browser for exactly one paint per tick. Each tick, the newest frame is
converted at most once per pixel format and handed to every enabled **output**.
The page's audio rides along, sliced into each frame's exact share of samples.

## 2. The five ideas that explain most of the code

**Our clock drives the browser.** Not the other way round. `SendExternalBeginFrame`
means one tick produces one paint. If you find yourself reaching for
`windowless_frame_rate` as the primary path, you are about to reintroduce
irregular pacing that looks fine in a five-minute test and wrong on a mixer.

**Rates are exact rationals, everywhere.** 59.94 is `60000/1001`, never 59.94.
Deadlines are computed from tick zero, never accumulated. Audio per frame is the
difference of two exact positions, which is what makes it alternate 800/801 at
59.94 instead of losing 40 ms a minute. If you see a `double` frame rate or an
accumulated period, that is a bug.

**The frame slot holds one frame, and the clock thread peeks rather than takes.**
A static page must still produce a frame every tick. Repeats are counted, not
hidden. Don't turn this into a queue — the older of two paints between ticks is
stale by definition.

**The clock thread owns its state, and can be parked.** `uyvyPool_`,
`blackPool_`, `uyvyScratch_`, `blackFrame_` are touched by the clock thread only,
or by another thread while it is parked via `pauseClock()`. This is not
defensive habit: mutating them from the HTTP thread caused a real heap overflow
and a hung control API. See §4.

**The preview is an output, not a tap.** It travels the same frame path as SDI
and NDI, so if the preview is right the outputs are right. Don't shortcut it
straight from the browser.

## 3. Layout

```
src/core/     formats, frames, pools, clock, FIFO, JSON, dlopen  ─ NO CEF
src/diag/     logging, crash reports, bundles                    ─ NO CEF
src/browser/  CefApp, CefClient (paint + audio), BrowserSource
src/engine/   the clock loop and everything it owns
src/outputs/  IOutput + preview | ndi | omt | decklink | aja
src/control/  HTTP server, OSC receiver, embedded control page
src/app/      entry points, Info.plists, entitlements
tools/        ndi_probe — an independent receiver, for verification
tests/        unit tests; link core only
```

**`weblinked_core` must not depend on CEF.** That split is what keeps the test
suite building in seconds instead of linking 200 MB of Chromium. If you need
something from core in the browser layer, that is fine; the reverse is not.

Note that `core` is compiled *without* CEF's flags, and `engine` *with* them —
which means `engine` is built `-fno-exceptions -fno-rtti -Werror`. `core` uses
`try`/`catch` in the JSON and format parsers and must stay in `core` for that
reason.

## 4. Traps, all of which have already bitten

**Chromium replaces your signal handlers.** `CefInitialize` installs its own
SIGTERM, SIGINT and crash handlers. Anything registered before it silently does
nothing. Both `installSignalHandlers()` and `diag::installCrashHandler()` are
called *after* `CefInitialize` for exactly this reason — the first symptom was a
crash that produced no crash report.

**Never convert a frame into a buffer sized for a different raster.** During a
format change the slot can hold a pre-resize paint while the pools have already
been rebuilt. Converting one into the other wrote four megabytes past the end of
the destination and surfaced frames later as a jump through a clobbered vtable.
The engine now drops stale-raster frames before conversion *and*
`frameInFormat()` refuses a mismatch. `tests/test_pixel_convert.cpp` pins the
destination-extent contract with a guard band. Don't remove either half.

**A thread that dies holding `Engine::mutex_` hangs the whole control API.** That
is how the above presented: the process stayed alive, the log looked fine, and
every subsequent HTTP request blocked forever. If the control API goes silent,
suspect the clock thread.

**macOS: ad-hoc signature + hardened runtime = will not launch.** Library
validation requires a matching Team ID, which ad-hoc signatures do not have. Error
text blames the framework, not the signature. `cmake/SignMacBundle.cmake` handles
both cases; read the comment before changing it.

**macOS: nested helpers must be signed, inside-out.** An unsigned `.app` bundling
executables gets its helpers SIGKILLed with no dialog and no log entry.

**FramePool must be held by `shared_ptr`.** A frame's deleter reaches back into
its pool, and a format change replaces the pool while frames from the old one are
still alive. The deleter holds a `weak_ptr` and frees the buffer if the pool has
gone. A raw pointer here is a use-after-free that only appears when someone
changes format mid-show.

**macOS needs an NSApplication subclass, and only a window reveals it.**
`src/app/mac_application.mm` implements `CefAppProtocol`. Without it the process
dies with "unrecognized selector -[NSApplication isHandlingSendEvent]" the moment
a real window pumps events. Every `--headless` test passes regardless, which is
exactly why it survived so long — test the window too.

**Shutdown ordering is fragile in two directions.** The window must be closed
before `CefQuitMessageLoop()` (the loop will not return otherwise), but the
*offscreen* browser must NOT be closed before the loop quits (its destruction
then never gets pumped and `CefShutdown()` hangs). And every `CefRefPtr` must be
released before `CefShutdown()` — a global holding the window's client hung it
silently. All three present identically: a process that logs a clean exit and
never exits.

**Never let Chromium reach the OS keyring.** `--password-store=basic` and
`--use-mock-keychain` are in `cef_app.cpp` because without them macOS raises a
Keychain dialog on every launch. A modal dialog on a machine that is live to air
is not acceptable, and it is startling during development too.

**Killing the process without a clean shutdown leaves a stale NDI source** that
receivers keep trying to connect to, and the next run can be undiscoverable for
minutes. The SIGTERM handler exists because of this, not for tidiness.

## 5. Verified vs assumed — read this before claiming anything works

`docs/04-verification.md` is the authority and is kept honest deliberately.

**Verified:** the NDI path end to end, against a real receiver, including colour
correctness against an independent BT.709 reference, audio cadence at 59.94, and
pacing over a three-minute soak with zero dropped ticks. The unit tests. The
control surface, OSC included. Clean shutdown.

**Compiles against a real SDK but has never touched hardware:** DeckLink (SDK
12.2), AJA (libajantv2 18.1), OMT (libomt 1.0.0.16 — never consumed by a
receiver).

**Never built or run:** Windows and Linux.

Do not upgrade an "it compiles" claim to "it works" anywhere in this repo. If you
verify something new, add it to `04-verification.md` with the command and the
output, not just a tick.

## 6. Working on it

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
./build/tests/weblinked_tests
```

Run the app, then check it with the probe rather than with its own counters:

```bash
./build/Release/WebLinked.app/Contents/MacOS/WebLinked \
  --url file://$PWD/tools/testcard.html --format 1080p50 --ndi=Test --headless &
./ndi_probe --source Test --frames 100 --bars
```

The app's own `/api/state` can only tell you it *sent* something. `ndi_probe`
tells you what arrived, which is the claim that matters.
