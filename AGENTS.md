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
converted at most once per pixel format — and, for outputs that composite over a
background colour, at most once per colour — and handed to every enabled
**output**. There is still only ever one paint: a background is a property of an
output, not of the page.
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

**Sources know nothing about each other.** `SourceManager` holds N whole
Engines; no engine ever learns that the others exist, which is what makes one
hung page or one missing card harmless to the rest. The manager is a map and a
lock, nothing more — resist the urge to give it a shared clock, a shared frame
pool or a shared browser. The independence *is* the feature.

## 3. Layout

```
src/core/     formats, frames, pools, clock, FIFO, JSON, dlopen,
              mDNS advertisement (+ _mac/_win/_linux)             ─ NO CEF
src/diag/     logging, crash reports, bundles                    ─ NO CEF
src/browser/  CefApp, CefClient (paint + audio), BrowserSource
src/engine/   the clock loop and everything it owns
src/outputs/  IOutput + preview | ndi | omt | decklink | aja | screen | kms | shared | stream
src/control/  HTTP server, OSC receiver, embedded control page
src/app/      entry points, Info.plists, entitlements, the tray icon
third_party/  cef, ndi, omt headers, and the vendored Syphon server subset
tools/        ndi_probe, syphon_probe, mdns_probe — independent, for verification
tests/        unit tests; link core only
docker/       the headless container image (§7) — packaging only, no source
```

There was a separate Tauri tray launcher here until v1.0.0. It existed to give
this process a desktop presence, and the engine now has one of its own —
`src/app/tray_mac.mm`, `tray_win.cpp`, `tray_linux.cpp` — so a second 140 MB
download carrying a duplicate copy of the engine no longer earns its place.

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

**This process opens no window, and must not.** Every browser in it is
windowless; windowless rendering *always* uses Alloy runtime style, and a
windowed browser defaults to Chrome style. Hosting both means two runtime styles
in one process, and the GPU process segfaults on startup — leaving a correctly
sized, completely empty window. That is exactly what shipped as the "operator
window" up to v0.5.2, and it survived because every test run was headless, where
the GPU process never crashes. The UI is the control page, in a browser; the
menu-bar/tray icon is what puts it one click away. If you ever add a window
back, `CefWindowInfo::runtime_style = CEF_RUNTIME_STYLE_ALLOY` is the thing you
will need, and a menu bar, because CEF sets neither for you.

The cost of having no window is that a double-click looks like a build that does
not start, which is exactly how v0.7.0 was read: a bundle with no icon either, so
Finder drew the blank generic tile, and then nothing appeared. Both halves are
fixed, and neither is a window — the bundle carries `WebLinked.icns`, and a
launch with no arguments hands the control URL to `openInDefaultBrowser()` once
the HTTP server is listening. *Another application's* window is not subject to
the runtime-style trap above, which is the whole reason it is allowed to be a
browser at all. Do not "improve" this into a window of our own.

**A Syphon server must be created on the main thread.** `SyphonServerBase`
registers for the announce-*request* notification from `-init`, on whichever
thread called it, and `NSDistributedNotificationCenter` delivers to that
thread's run loop. Created on the HTTP thread — which is what happens the moment
an operator adds the output from the control page rather than the command line —
the server broadcasts its opening announce and then answers no discovery request
ever again. A consumer already running finds it; one started later never does,
and nothing logs anything. `SyphonSurface::open` marshals to the main thread,
where `CefRunMessageLoop` guarantees a run loop, and `tools/syphon_probe.mm` is
written to start *after* the server so the check cannot pass by accident.

**`root_cache_path` is not optional.** Left unset, CEF picks a default profile
directory shared by *every* CEF application on the machine, and Chromium allows
exactly one browser process per profile directory. The second one to start finds
the lock taken, tries to hand off to a holder that is a headless render host
with no window to raise, times out, and shows "your profile could not be loaded
correctly" — a dialog, on a machine that is live to air. CEF warns about this by
name in the log, and the warning was ignored for five releases. It is now always
set, to `settings::profileDirectory(controlPort)`; `--cache` overrides it and so
must itself be unique per instance. Keying on the control port is what lets
several instances run at once — don't make it per-process, or every launch
leaves a directory behind.

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

**So does one that waits on a driver while holding it, and drivers do wedge.**
Issue #12: Desktop Video 16.4 deadlocked inside its own allocator, the clock
thread was waiting on that inside `CreateVideoFrame` while holding `mutex_`, and
every `GET /api/state` then waited on `mutex_` for good — 1,410 parked HTTP
threads by the time someone killed it. "DeckLink's scheduled playback cannot
block" was true of the API and false of the driver. Since the fix, no call into an
output is made holding `mutex_` at all, and every wait on an output has a
deadline (§8). The symptom moved accordingly: a wedged output now shows up as
`"stale": true` on that output and `pacing.last_tick_age_ms` climbing, while
`/api/state` keeps answering. If the control API answers but the ticks have
stopped, suspect the clock thread inside a driver.

**A DeckLink frame destroyed on the completion thread can deadlock the SDK.**
Same issue. The old output made a frame per tick and released its reference in
`ScheduledFrameCompleted`; the SDK's own release after the callback then
destroyed the frame on the completion thread, and when that emptied one of the
SDK's buffer allocators while the clock thread was inside `CreateVideoFrame`,
16.4 deadlocked on itself. The output now creates a ring of frames at start and
recycles them, so nothing is allocated or freed during playback (§8). The SDK's
documentation says the same thing in passing: frames you manage yourself
"should be disposed or reused inside the ScheduledFrameCompleted callback".

**Remove the completion callback after `DisableVideoOutput`, never before.**
`DisableVideoOutput` blocks until every scheduled frame is completed or flushed,
and the flushed ones are reported through the callback. The old `stop()` cleared
the callback first, so every frame still queued at a stop was never handed back
and leaked. Found by `tools/dl_fake_card.cpp` run against the old code; the
SDK's samples use the order `stop()` now uses.

**A schedule that only counts up falls behind for good after one long stall.**
Issue #13. Frame N went to the card at N × duration. A late tick only shortened
the queue, which was the design, but a stall longer than the queue left N
behind the card's playhead, and one frame per tick never catches up: every frame
for the next hour was late. `PlayoutSchedule` re-anchors to the playhead plus
the preroll. Display times are also in the display mode's own `GetFrameRate`
units now; the microsecond base before them was an accumulated period at 59.94,
which rule 2 forbids.

**The engine is built `-fno-threadsafe-statics`.** CEF's flags say so. A
function-local `static` in engine code is initialised without a guard, so two
threads reaching it first at once can both construct it. Use namespace scope, as
`gFrameAllocationMutex` in `decklink_output.cpp` does. `core` is not built with
CEF's flags and is not affected.

**The settings page drops any output option it has no field for.** Its editor
rebuilds `options` from its fields on **Apply**, so an option set only on the
command line vanishes the first time anyone edits that output. `--key-premultiplied`
did exactly that until it got a tick box. A new backend option needs a field, or
Apply has to carry the unknown ones across.

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

**Never trust the loop to return once asked.** On Linux `CefRunMessageLoop()`
never returns after `CefQuitMessageLoop()`, and on macOS a run on 2026-09-30
logged the signal and then nothing for over an hour. So `beginShutdown()`
withdraws everything the network can see *before* asking the loop to quit — the
control API and its mDNS record, then every output via `SourceManager::halt()`
(clocks parked, NDI senders destroyed, cards released, browsers untouched) — and
the watchdog `_exit`s 10 s later if the rest has not finished, naming the step.
Each step logs. A second SIGTERM exits at once. Don't move output teardown back
behind the loop, and don't let `halt()` close a browser. See
`docs/04-verification.md` section 37.

**Never let Chromium reach the OS keyring.** `--password-store=basic` and
`--use-mock-keychain` are in `cef_app.cpp` because without them macOS raises a
Keychain dialog on every launch. A modal dialog on a machine that is live to air
is not acceptable, and it is startling during development too.

**Build clean before you tag.** An incremental build keeps the CEF wrapper's
objects and never recompiles them, so a change that breaks *CEF's* build is
invisible locally and fails on CI. `enable_language(OBJCXX)` did exactly this:
it made CMake compile every `.mm` as OBJCXX including CEF's own, and CEF only
sets the CXX flags, so its `cef_scoped_sandbox_context_mac.mm` failed with
"unknown type name 'constexpr'". Leave OBJCXX alone — CMake compiles `.mm` as
CXX and clang infers Objective-C++ from the extension, which is how CEF builds
its own.

**Windows needs NOMINMAX.** `<windows.h>`'s `max`/`min` macros collide with
`std::numeric_limits<>::max()` inside CEF's `cef_ref_counted.h`, and MSVC blames
the CEF header rather than the macro.

**Killing the process without a clean shutdown leaves a stale NDI source** that
receivers keep trying to connect to, and the next run can be undiscoverable for
minutes. The SIGTERM handler exists because of this, not for tidiness. The mDNS
advertisement has exactly the same failure and is withdrawn on the same path —
`ControlApi::stop()` stops the responder *before* the sockets close, because a
record pointing at a port that has stopped answering is worse than no record.

**An advertisement is a promise about an address, and the default bind cannot
keep it.** `httpBind` defaults to `127.0.0.1`. Advertising that publishes a
record which resolves, on every machine that browses, to *its own* loopback —
so the instance appears in every fleet list on the subnet and every one of them
fails to connect, with nothing logged anywhere. `mdns::bindIsAdvertisable()`
refuses, loudly, and `/api/settings` reports why. Do not "fix" this by
advertising anyway and letting the consumer work it out; the consumer cannot.

**mDNS uses the system responder on macOS and Windows, on purpose.** `dns_sd`
(in libSystem, no link needed) and `DnsServiceRegister` (resolved from
dnsapi.dll, so a pre-1703 Windows degrades instead of failing to load) both add
a record to the responder the machine is already running — the same one the NDI
runtime uses. A second responder bound to UDP 5353 on a machine that is live to
air is not a thing to introduce for a convenience feature. Linux has no system
API of that kind, so avahi is opened with `Dylib` exactly as libndi is: absent
avahi means no advertisement and nothing else.

**A responder can rename you.** Two instances that pick the same name get one
of them renamed, and the renamed value is what consumers show. Read it back
from the callback (`Responder::registeredName()`); never assume the name you
asked for is the name on the network.

**A windowless browser cannot own a popup, and CEF's default tries.** Left
alone, `target="_blank"` and `window.open` create a second browser parented to
the offscreen one. That killed the application — and on the way there it rebound
`RenderClient::browser_` to the popup, so the engine drove a browser nothing was
reading. `OnBeforePopup` now always returns true; the URL is loaded in place or
dropped. `OnAfterCreated` and `OnBeforeClose` also check which browser they were
given. Never make popups "work" — there is no window to put one in.

**Anything the clock thread reads outside `mutex_` must be atomic.** `matrix_`
and `pacing_` were plain members, which was fine while only `start()` wrote
them. The settings page made them writable at run time, and a torn read on the
clock thread is the failure mode described two traps up.

**Pacing cannot be switched on a live browser.** `external_begin_frame_enabled`
is part of `CefWindowInfo` and read only at creation. Setting the field alone
looks like it works and produces no frames at all: `requestFrame()` stops asking
and the browser does not paint on its own. `BrowserSource::setPacing` rebuilds
the browser at the same URL.

**A CSS class with a `display` beats `[hidden]`.** The browser's own rule is at
UA specificity, so `.check { display: flex }` silently outranks it. The settings
page has an explicit `[hidden] { display: none !important }` because of a
checkbox that would not hide. Assert visibility from the DOM, not from a
screenshot.

**Never let the settings page rewrite itself under someone's fingers.** The
state poll runs every second; the editors are built only on arriving at the view
and after a successful mutation.

**Never hold a bare `Engine*` across anything.** Every call reaches an engine
through `SourceManager::withSource`, which holds that source open for the
duration, and a removal waits for the calls inside to come out before it stops
anything. `Engine` reads `browser_` without a lock and `Engine::stop()` destroys
it, so a pointer that outlives the lookup is a use-after-free waiting for an
operator to remove a source mid-show. The expensive halves of add and remove
happen *outside* any lock on purpose — holding one across a browser start would
freeze every other source's control surface.

**And never hold the source map's lock across a call into an engine.** Until
the review of #12's fix, "holds a shared lock for the duration" was literally a
`std::shared_mutex` held across every call. A request wedged in a DeckLink
`stop()` then held it for good; the next add, remove or settings load queued for
the exclusive side; and libc++'s `shared_mutex` queues new readers behind a
waiting writer, so every source's `/api/state`, every OSC message and the tray
(on the UI thread, which stalls every browser) waited behind it. The map is now
a `SourceTable` (`src/core/source_table.h`) whose lock only ever covers copying
a pointer; each source counts the calls inside it. CLAUDE.md rule 12's wording
("holds a shared lock for the call") predates this and means "holds the source
open".

**The control page has no build step, so a stray byte is invisible until
run time.** A single NUL inside a `.join(' ')` compiled fine, served 200, rendered
the HTML, and killed every line of JavaScript after it — the page just sat on
"connecting" with a black preview. A NUL also makes `grep` treat `web_assets.h`
as binary and return *nothing*, which will send you the wrong way for ten
minutes. If the page looks dead, run its own script text through `new Function()`
in a browser: it names the offending line immediately. The earlier TDZ bug had
the same shape.

**"Hidden" does not mean nobody is looking.** `document.hidden` is true for a
kiosk shell, an embedded webview and a screenshot tool. Throttle polling when
hidden; never skip it. Skipping is exactly why every source thumbnail came out
black the first time the strip was built.

## 5. Verified vs assumed — read this before claiming anything works

`docs/04-verification.md` is the authority and is kept honest deliberately.

**Verified:** the NDI path end to end, against a real receiver, including colour
correctness against an independent BT.709 reference, audio cadence at 59.94, and
pacing over a three-minute soak with zero dropped ticks. Per-output background
colours, measured off the wire against a reference computed from the `over`
operator, with two senders on different colours from one browser paint. The unit tests. The
control surface, OSC included. Clean shutdown. The settings page and its file,
including that command-line flags override it across a restart. The diagnostics
endpoints. The popup fix, against both `target="_blank"` and `window.open`, and
both policies.

**Also verified, against real hardware since v0.6.1:** DeckLink. A Duo 2
confirmed output, pre-roll and buffer level (6 frames, zero dropped ticks over a
two-minute soak), colour via an SDI loopback captured by `tools/sdi_probe.mm`,
and that a **keyed fill carries straight alpha** — 50%-alpha green measures 132
off the wire where premultiplied would be ~95, the exact value NDI produced
before `unpremultiplyBgra`. Not measured on DeckLink: the key channel itself,
the internal-keying composite, audio over SDI, genlock over hours.

**On a card, but not here:** the DeckLink frame ring, its application-owned
memory on SDK 14.3 and later, and the stall re-anchoring that fixed #12 and
#13. They were written with no card attached and pass against a simulated card
(`tools/dl_fake_card.cpp`) that the old code fails. Daniel Nyström then ran
them on the rig that found the bugs (UltraStudio Mini Replay 12G, Desktop Video
16.4, an SDK 16.0 build, 1080p25 keyed): the section 40 recipe and an
eight-hour soak with 42 injected faults, no hang, every counter 0 and
`buffer_overlaps` 0 throughout. That is a user's run, reported in #12, not one
made here; keep the two apart. What the real SDK does with application memory
is still unknown, and the pre-14.3 path has met no card. The engine's and the
source map's lock changes are verified against NDI here and ran under a real
driver only on that rig; a driver wedged mid-call has only ever been a fake one,
in `tests/test_output_slot.cpp` and `tests/test_source_table.cpp`.

**Also verified, against Resolume Arena:** the shared (Syphon) output. Arena
7.27.1 lists it, loads it on a layer, reports it at 1920x1080 and labels it
Premultiplied of its own accord. Colour, premultiplied alpha and orientation are
exact through `tools/syphon_probe.mm`, which links *Arena's* Syphon 5 framework
rather than the Syphon 6 sources vendored here — two implementations agreeing,
not one agreeing with itself. The copy costs one dropped tick in 1502.

**The Spout half is the first Windows code here that has been run** — but only
the output, and only through a harness. `tools/spout_send_test.cpp` links the
real backend against the real SDK on a Windows 11 ARM64 VM (x64 binaries), and
`tools/spout_probe.cpp` reads the pixels back: colour, alpha and orientation
exact at tolerance zero. Weaker evidence than the Syphon pass and the doc says
so — there is no second Spout implementation to check against, so it is one
SDK's receiver reading its own sender. Still unproven: the CMake Windows branch
(never configured, because nothing else builds there), and any real consumer —
no Resolume for Windows, TouchDesigner or OBS was involved.

Keying has a ceiling worth knowing: alpha needs RGBA, RGBA costs about twice the
link rate of the same raster in 4:2:2, and a Duo 2 therefore refuses a keyed
1080p50 while accepting 1080p25. The `SupportsExternalKeying` attribute does not
predict this — it passes on such a card. Capability and bandwidth are different
questions.

**Compiles against a real SDK but has never touched hardware:** AJA
(libajantv2 18.1), OMT (libomt 1.0.0.16 — never consumed by a receiver).

**Never built or run:** nothing is in this category any more for the three
desktop platforms — Windows and Linux are both built *and* run now, on the
lab's own VMs. See docs/04-verification.md section 31.

**Builds and is unit-tested, but not driven end to end:** `/api/pacing`, whose
browser rebuild compiles and has not been exercised on air. The tray's
Explorer-restart re-add path on Windows, and the Linux tray's appearance on a
healthy desktop (its protocol conformance *is* verified, against KDE's own
watcher).

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

## 7. The container image (`docker/`)

`ghcr.io/stoatworks-labs/weblinked`, built by `.github/workflows/docker.yml` from
this checkout (context = repo root, `docker/Dockerfile`). It lived in
stoatworks-labs/weblinked-docker until October 2026, which `git clone`d main at
build time — and that layer was a cache hit on every run, so its `:edge` sat on
22 August's main through six releases while every build went green. **Never
fetch the source inside the Dockerfile again**; COPY from the context is what
keys the cache on the files. The old name is not pushed any more and its `:edge`
stays frozen (no write grant on that package; Allan's call, 2026-10-11). User
docs: `docker/README.md`.

The decisions in it that look arbitrary and are not:

- **libndi is never in the image.** The NDI licence needs the distributor's terms
  to forbid modification and reverse engineering; the release installers carry
  an EULA that does, a public MIT image has none. It is mounted at
  `/opt/ndi/lib` (on `LD_LIBRARY_PATH`). A change that "conveniently" bakes it
  in is the one change this packaging exists to prevent.
- **KMS and STREAM are built OFF.** KMS needs `/dev/dri`, DRM master and a
  monitor on the server — a board driving its own screen, which is the tarball's
  job. STREAM needs an ffmpeg binary, and shipping one puts an H.264/AAC encoder
  in a public image (see the codec section of `docs/06-ndi-distribution.md`).
  `docker/smoke_assert.py` fails if either turns up in `compiled_backends`, so
  changing this is a decision, not an accident.
- **No avahi in the image**, so it does not advertise. avahi-client reaches the
  daemon over the host's D-Bus system bus, and mounting that into a container is
  a bigger grant than discovery is worth. The weblinked-docker docs once said
  mounting the avahi socket worked; it never could have.
- **The three Chromium switches in the entrypoint** (`--ozone-platform=headless
  --use-gl=angle --use-angle=swiftshader`) are the whole headless story. Drop
  any one and the container exits at startup.
- **Host networking is a correctness flag**: NDI discovery is mDNS, and a bridge
  network hides the sender silently.
- **The tailnet warning is stronger than boilerplate**: `POST /api/script` makes
  an exposed control port remote code execution in a browser. Do not weaken it.
- **The builder's `-dev` packages are libcef.so's DT_NEEDED entries**, not ours;
  without them the link fails with `undefined reference to dbus_/snd_/atk_/NSS_`.
  CEF links X11 even with the screen output off.
- **CEF's version and linux64 sha1 are read from `cmake/FetchCEF.cmake`.** A CEF
  bump there reaches the image, and one that forgets the linux64 sha1 fails the
  image build — correctly.
- **`.dockerignore` is an allow-list**, kept in step with the Dockerfile's COPY
  lines and the workflow's path filter.
- **amd64 only**: arm64 would be a QEMU-emulated Chromium-wrapper compile.
- **The Unraid template is `unraid/weblinked.xml`, here**, and stoatworks-unraid
  copies it into the CA feed (`hasOwnTemplate`). Change a port, mount or variable
  and it has to change in that file too.

## 8. Calling into outputs, and the DeckLink ring

Issues #12 and #13 (§4) are why this section exists. The design in one line:
**a vendor driver may never return from a call, so nothing waits on one for
ever and nothing that the control API needs is held while waiting.**

How it is built:

- Each output lives in an `OutputSlot` (`src/outputs/output_slot.h`), held by
  `shared_ptr` from `Engine::Entry`, with a `CallGate` (`src/core/call_gate.h`):
  a timed lock every call into the output goes through. `call()` is for the
  clock and for reads; `exclusive()` starts or stops the object already there
  and leaves the generation alone; `replace()` swaps or destroys it, always
  moves the generation on, and is the only place the pointer is handed out.
  All header-only and CEF-free, so the tests drive them with a fake output
  that hangs.
- The clock plans each tick under `mutex_` from `Entry::desc` (pixel format,
  alpha, audio, and a generation), lets go, converts, then calls each output
  through its gate. `call()` checks the generation, so an output replaced
  between the plan and the call is skipped rather than handed a payload sized
  for something else.
- `state()` copies the entries under `mutex_` (waiting at most 100 ms, else the
  last report marked stale), then asks each slot for `status()` with one 100 ms
  budget for all of them and a 10 ms floor each (`OutputSlot::atLeast`). A slot
  whose gate is held answers from its last status, marked `"stale": true` with
  `driver_busy_ms` only once the call has run past the wedge threshold (1 s);
  before that it is merely busy.
- Control paths (add, remove, update, enable, format) are serialised by the
  engine's `ControlQueue` (`src/core/control_queue.h`), whose mutex neither the
  clock nor `state()` ever takes. A change waits up to 30 s for the one in
  front, but gives up at once if four are already waiting, or if the one in
  front is inside an output gate that counts as stuck — each control path marks
  the output it is calling with `ControlCall` so a waiter can tell. Every waiter
  is an HTTP thread, and there are 64.
- Above the engine, `SourceManager` keeps its sources in a `SourceTable`: the
  table's lock covers copying a pointer and nothing else, each source counts the
  calls inside it, and a removal closes the source to new calls and waits for
  the ones inside, giving up (and reopening it) after 40 s.
- `setFormat` stops the outputs with the clock running, parks the clock only
  for the swap of the raster, epoch and pools, and reopens them with the clock
  running. `halt()` waits 2 s for the clock thread, closes the outputs whether
  or not it came out, and only then joins it.
- The DeckLink output makes `2 × preroll + 4` frames at start into a
  `RecycleRing` (`src/core/recycle_ring.h`), hands one out per tick, and gets it
  back in the completion callback. On SDK 14.3 and later each frame wraps an
  `AppVideoBuffer` — WebLinked's own page-aligned memory, given to the SDK with
  `CreateVideoFrameWithBuffer` — so the SDK's allocator pool is never used, and
  the buffers outlive a stop/start at the same raster. `PlayoutSchedule`
  (`src/core/playout_schedule.h`) turns the frame index into display times in
  the mode's own units and re-anchors it to the card's playhead.

The rules that keep it true:

1. **No call into an output while holding `Engine::mutex_`.** Not submit, not
   start or stop, not `status()`, not a destructor. Copy the slot out under the
   lock and call after letting go. `mutex_` guards the list and `format_`, and
   is held only for copying.
2. **Every call into an output goes through its slot**, and the output object
   is replaced or destroyed only inside `replace()`. That is what makes "no
   object destroyed with a call in flight" true without anyone having to think
   about it. Starting and stopping use `exclusive()`, which does not move the
   generation, so re-enabling a running output costs no tick.
3. **Every wait on an output has a deadline**, and every shared budget has a
   floor per output: the clock 5 ms a tick with 2 ms each, `state()` 100 ms
   with 10 ms each, a control path 2 s for a gate and 30 s queued in the
   control queue (four waiters at most, and none behind a stuck change), a
   source removal 40 s for the calls inside, `halt()` 2 s
   for the clock before closing outputs anyway, the DeckLink frame allocator
   5 s. A gate held for over a second is tried once and given up on. A new
   wait without a deadline is a bug; the one exception is `halt()`'s final
   join of the clock thread, which uses the engine and its browser and so
   cannot be abandoned.
4. **After every `replace()`, copy `slot->description()` back into the entry
   under `mutex_`.** Forget, and the clock skips that output for ever — the
   safe way to be wrong, but wrong.
5. **Stop feeding before stopping.** A path that stops or replaces an output
   clears `entry.enabled` under `mutex_` first, so the clock is not queued on
   the gate behind a `stop()`. On a refused gate, put `enabled` back: the
   contract is "nothing changed".
6. **Lock order is the control queue → gate → `mutex_`, and `mutex_` only
   briefly.** Never wait on a gate holding `mutex_`. The clock thread never
   takes the control queue. Above all of these, the `SourceTable` lock is held
   only to copy a pointer: never call into an engine holding it. A control
   path's call into an output goes inside a `ControlCall`, or a change stuck
   in it looks merely slow to the requests queued behind it.
7. **Clock-thread state is still rebuilt only while parked** (CLAUDE.md rule
   6), and only that: never park the clock across a driver call, or one wedged
   output freezes the source's others. `setFormat` refuses up front, changing
   nothing, when the clock has not ticked for a second; and if the park still
   fails (a race), it reopens the outputs at the old raster. A clock that would
   not stop is inside a driver, and its pools must not be swapped underneath
   it; and a refusal should cost nothing — not NDI senders re-advertised, not
   cards prerolled again.
8. **A DeckLink ring frame is written only between its completion and its next
   `ScheduleVideoFrame`**, inside `FrameBytes`, which on SDK 14.3 and later is
   what brackets the write with `StartAccess`/`EndAccess`. The ring never hands
   out a frame the card has.
9. **Never `Release()` a ring frame except in `releaseRing()`**, and call that
   only after `DisableVideoOutput` has run with the callback still installed.
   The completion callback hands frames back to the ring and never releases
   them; ring frames are made and released under `gFrameAllocationMutex`,
   waited for 5 s at most. Before SDK 14.3 a release that cannot get it leaks
   the frames: they are the SDK's memory, and another card's driver is wedged
   in the allocator they would go back to.
10. **`submit()` never waits for a free frame.** No frame free means drop the
    tick and count it (`ring_misses`). Waiting would stall every other output
    on the same clock.
11. **The schedule index only increases**, and display times stay in the
    mode's own `GetFrameRate` units (CLAUDE.md rule 2). Re-anchoring may skip
    slots; it never reuses one.
12. **On SDK 14.3 and later, ring memory is ours.** Make frames with
    `CreateVideoFrameWithBuffer` over `AppVideoBuffer`s, write through our own
    buffer (still inside `FrameBytes`), and keep the buffers until the raster,
    pixel format or ring size changes — or until a stop finds the card still
    holding frames, whose buffers must then not be reused, because those frames
    still wrap them. Do not go back to `CreateVideoFrame` on that path: it is
    the SDK's pool, which is where #12 deadlocked. `buffer_overlaps` counts a
    write starting while a read is open on the same buffer; it should stay 0,
    unless the SDK turns out to hold reads for a frame's whole life (see the
    caveat in section 40's recipe). Desktop Video 16.4 does not: it stayed 0
    through Daniel Nyström's eight hours on an UltraStudio (section 40).
13. **At shutdown, an engine with a request still inside is kept, not
    dropped.** Dropping it would leave the straggling thread to run `~Engine`,
    which closes a browser, possibly after `CefShutdown`. `SourceManager::stop`
    keeps such engines for the life of the process.

## Notes

`docs/NOTES.md` carries this repo's working notes — current status, decisions
already made, and the traps that have actually bitten. Read it before changing
anything non-obvious. Cross-cutting fleet knowledge lives in
[fleet-notes](https://github.com/stoatworks-labs/fleet-notes).
