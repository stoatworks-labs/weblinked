# Verification

What has actually been proven about this code, how, and what has not. Written so
that a claim in the README can be checked rather than believed.

The distinction that matters throughout: **compiles against a real SDK** is not
the same as **works against real hardware**. Three of the four output backends
are in the first category.

Everything below was run on macOS 26.4.1, Apple Silicon (M4 Max), against
CEF 150.0.17 / Chromium 150.0.7871.187.

---

## 1. Unit tests — verified

```bash
cmake --build build && ./build/tests/weblinked_tests
# 49 tests, 24902 checks, 0 failures
```

The two that carry real weight:

**The colour converter is checked against an independent implementation.** The
integer 16.16 fixed-point path in `pixel_convert.cpp` is compared against a
floating-point implementation written straight from the BT.601/BT.709
definitions, across a sweep of 4,096 colours per matrix, requiring agreement
within one LSB. Comparing the shipped code against the same coefficients would
only prove it equals itself; this catches a wrong coefficient, a swapped Cb/Cr,
or a missing studio-swing offset.

A single-colour test cannot tell a right matrix from a wrong one. That failure
mode is not hypothetical — another project in this fleet shipped a broadband
saturator as a "harmonic EQ" through four releases because a single-tone test
could not distinguish them.

**The clock does not drift.** `test_frame_clock.cpp` quantifies it: deadlines are
computed from tick zero as exact rationals, so error stays inside a nanosecond
over ten hours, against ~72 ms (four frames) an hour for the usual
accumulate-a-rounded-period approach at 59.94.

## 2. URL → NDI, end to end — verified

Not verified by reading WebLinked's own counters, which can only say it *sent*
something. Verified with `tools/ndi_probe.cpp`, a separate receiver linked
against the NDI SDK that reports what actually arrived on the network.

```bash
clang++ -std=c++20 -I"/Library/NDI SDK for Apple/include" tools/ndi_probe.cpp \
  "/Library/NDI SDK for Apple/lib/macOS/libndi.dylib" \
  -Wl,-rpath,"/Library/NDI SDK for Apple/lib/macOS" -o ndi_probe

WebLinked --url file://$PWD/tools/testcard.html --format 1080p50 --ndi=Test --headless &
./ndi_probe --source Test --frames 100 --bars
```

Result:

```
raster       1920x1080 progressive
rate         50/1 (50.000 fps)
FourCC       UYVY
stride       3840 bytes (3840 expected for 4:2:2)
aspect       1.7778

bar        sampled Y/Cb/Cr    expected Y/Cb/Cr   verdict
grey        181  128  128       181  128  128       ok
yellow      169   44  136       169   44  136       ok
cyan        146  147   44       146  147   44       ok
green       134   63   51       134   63   51       ok
magenta      63  193  205        63  193  205       ok
red          51  109  212        51  109  212       ok
blue         28  212  120        28  212  120       ok
black        16  128  128        16  128  128       ok

received 100 video frames in 1.97 s — 50.87 fps measured
PASS
```

Every bar matches the probe's own independent BT.709 reference exactly. The
colour path is correct on the wire, not merely internally consistent.

## 3. Audio, at a fractional rate — verified

The hard case. At 59.94, one frame's share of 48 kHz audio is 800.8 samples, so
a correct sender must alternate 800 and 801. A constant 800 loses a sample every
five frames — about 40 ms a minute, an audible lip-sync error within a couple of
minutes.

Rendering a page with a 1 kHz WebAudio oscillator at gain 0.5, at 720p59.94:

```
rate         60000/1001 (59.940 fps)
first audio: 2 ch at 48000 Hz, 801 samples, FourCC FLTp

received 400 video frames and 399 audio frames (319520 samples) in 6.64 s
audio peak 0.5000; samples per frame: 79x800 320x801
  mean 800.8020 samples/frame
```

Three things proven at once: the rate travels as the exact rational 60000/1001;
the sample cadence alternates and averages 800.8020 against a required 800.8;
and the peak is exactly 0.5000, matching the page's gain, so the float audio path
applies no stray scaling.

## 4. Pacing under load — verified

A three-minute soak at 1080p50:

```
ticks in ~180 s : 9001   (50.01 fps)
dropped ticks   : 0
audio under/over: 0 / 0
```

Zero dropped ticks. Separately, over a 1,631-tick run the browser produced 1,610
paints — one paint per tick after the ~21 ticks of startup before the first
paint — which is what clock-driven `SendExternalBeginFrame` pacing is supposed to
deliver, and what Chromium's own timer does not.

## 5. Control surface — verified

- Control page served, 200, `text/html` (13,948 bytes).
- Token enforced: no token → 401 `{"error":"missing or incorrect token"}`;
  `?token=` and `Authorization: Bearer` both accepted.
- `/api/preview` returns exactly 518,400 bytes for a 480×270 BGRA frame, with
  `X-Frame-Width`/`X-Frame-Height`/`X-Frame-Sequence` headers.
- `/api/diagnostics` writes a bundle and returns its path.
- Malformed JSON and unparseable formats are rejected with 400, not guessed at.
- OSC verified over UDP including `#bundle` unwrapping: `/weblinked/url` loaded a
  real external URL, `/weblinked/mute 1` muted, and a bundled
  `/weblinked/output/<name> 0` stopped the NDI sender. Re-enabling it over HTTP
  restarted it cleanly.
- Runtime format changes verified across 720p25, 1080p50, 720p59.94, 1080i25,
  1920x1080p30 and 2160p25, returning to 1080p50 with colour bars still exact.

## 6. Clean shutdown — verified, in both modes

`SIGTERM` produces `shutdown requested by signal` → `audio stream stopped` →
`WebLinked exiting cleanly`, with the NDI sender destroyed rather than abandoned,
**with and without the operator window**.

Both modes matter, and that is the point: for a long time only `--headless` was
ever tested, and the windowed path was badly broken in a way headless could not
show. See the bugs below.

This is not cosmetic. Killing an earlier build without a handler left a stale
source advertised on the network, and the next run was undiscoverable for
several minutes — which looked exactly like a broken sender.

## 7. Alpha over NDI — verified, and it found a bug

Rendering a page with four vertical bands — opaque red, 50% green, 25% blue and
nothing at all — at 1080p50 with `--alpha`, and measuring what a receiver got.

The receiver negotiated **UYVA** (UYVY plus a full-resolution alpha plane) rather
than BGRA under `recv_color_format_fastest`, which is worth knowing: an alpha
check that only looks at BGRA silently reports nothing.

Alpha itself was right first time:

```
x= 240  A=255      x= 720  A=128      x=1200  A= 64      x=1680  A=  0
```

The *colour* was not. The 50% green band came back at **Y=95** where straight
alpha requires **173**:

```
before   x= 720  Y= 95   (premultiplied, sent as if straight)
after    x= 720  Y=173   (expected 172.6)
```

Chromium composites with premultiplied alpha, and NDI, OMT and a DeckLink keyer
all expect straight. Passing Chromium's buffer through unchanged makes every
partially transparent pixel too dark — soft edges, drop shadows and fades render
muddy — and the error is *invisible on fully opaque graphics*, which is exactly
why it survives casual testing. `unpremultiplyBgra` now undoes it for outputs
that key, and all four bands match the reference:

```
        measured    expected
red     Y= 61 A=255  Y= 62.6 A=255
green   Y=173 A=128  Y=172.6 A=128
blue    Y= 31 A= 64  Y= 31.8 A= 64
empty   Y= 16 A=  0  Y= 16.0 A=  0
```

`tests/test_pixel_convert.cpp` pins the conversion, including saturation rather
than wraparound when a channel exceeds its own alpha.

## 8. An interactive preview — verified

Pointer and keyboard input forwarded to the offscreen page, measured by watching
the *output*, not the API's return value.

- **Clicking dismissed a cookie banner** on a test page: the button was hit and
  the banner disappeared from the NDI output.
- **Coordinates are exact.** The page reported the click at `211,165`; the
  request sent `nx 0.11, ny 0.153` against a 1920x1080 raster, which is
  `211.2, 165.2`.
- **Scrolling** registered a wheel delta.
- **Typing** put `WebLinked` into a real `<input>`, with case preserved and the
  caret visible.

Two things this turned up, both now in the API docs:

- An offscreen browser has no focus until told, so keyboard input goes nowhere
  until a `focus` event is sent.
- The `character` has to be on the *keydown*, not just the char event. With only
  a virtual-key code the page sees `e.key` as `"Unidentified"`, so a graphic
  listening for a particular key never fires. The first typing attempt failed
  for exactly this reason.

**DeckLink key + fill is implemented but unverified**, like the rest of the
DeckLink backend — see below.

## 9. The operator window — verified

The window opens, shows the control page, and its preview updates live. Verified
by the window server reporting a top-level window owned by the process, and by
capturing the page.

---

## Bugs this verification actually found

Recorded because they are the argument for doing it at all. All four were found
by running the thing, not by reading it.

**A heap overflow on runtime format change.** After changing raster the engine
could still be holding a 1920×1080 frame while its UYVY pool had been rebuilt at
1280×720, and converted one into the other — writing about four megabytes past
the end of the destination. It did not fault where it happened; it surfaced
frames later as a jump through a clobbered vtable, killing the clock thread while
it held the engine mutex, which in turn hung every later control request. Fixed
by dropping stale-raster frames before conversion, and by making the converter
refuse a raster it was not sized for. `tests/test_pixel_convert.cpp` now pins the
destination-extent contract with a guard band.

**Chromium replaces signal handlers.** `CefInitialize` installs its own SIGTERM,
SIGINT and crash handlers, silently replacing anything registered earlier. Both
WebLinked's signal handling and its crash reporting were installed before
`CefInitialize` and therefore did nothing — the first symptom being that the
crash above produced no crash report at all. Both are now installed afterwards.

**Hardened runtime plus an ad-hoc signature will not launch.** With
`--options runtime`, macOS enforces library validation, which requires every
loaded library to share the process's Team ID. An ad-hoc signature has none, so
the app died with *"mapping process and mapped file (non-platform) have different
Team IDs"* — which reads like a corrupt bundle rather than a signing policy.
Ad-hoc builds are now signed without hardened runtime; a Developer ID build gets
hardened runtime plus the library-validation exemption every Chromium app needs.

**The minimal CEF distribution has no sandbox library.** CEF's CMake defaults to
`USE_SANDBOX=ON`, which links a `cef_sandbox.a` the minimal distribution does not
contain.

**macOS needs a custom NSApplication subclass, and nothing says so until a
window exists.** Chromium's message pump calls `-[NSApplication
isHandlingSendEvent]`, a `CrAppProtocol` selector stock `NSApplication` does not
implement, so the process died with

```
*** Terminating app due to uncaught exception 'NSInvalidArgumentException',
reason: '-[NSApplication isHandlingSendEvent]: unrecognized selector sent to
instance ...'
```

This went unnoticed for a long time because every test run used `--headless`,
where no window ever drives `NSApp` — so the application worked perfectly right
up until somebody opened its window, and then failed on the way *out*, which
reads like a shutdown bug rather than a missing application class.
`src/app/mac_application.mm` provides it now.

**Shutdown ordering, twice over.** With the window open, `CefQuitMessageLoop()`
does not end `CefRunMessageLoop()` — the window has to be closed first. And
`CefShutdown()` blocks forever if any `CefRefPtr` is still held, which the
operator window's client was, as a global. Both produced the same symptom: a
process that logged a clean shutdown and then sat there holding the control port
and the NDI source name. An attempted fix that closed the offscreen browser
*before* quitting the loop made it worse, hanging headless too, because the
browser's destruction then never got pumped.

**Chromium asks macOS Keychain for a password-store key on every launch**, which
raises an authorisation dialog — on a machine that may be live to air.
`--password-store=basic` and `--use-mock-keychain` keep it away from the keyring;
a render host has no use for a password store.

**A hidden document must not stop the preview.** The control page skipped its
preview poll entirely when `document.hidden` was true. That covers more than a
backgrounded tab: a kiosk shell, an embedded webview or a screenshot tool can all
report hidden while somebody is looking straight at the screen, and the
confidence monitor showed black. It now polls slowly rather than not at all.

---

## Not verified

Everything in this section is written against a real SDK header set and compiles,
and nothing in it has ever moved a pixel.

**DeckLink.** Compiles against DeckLink SDK 12.2 headers. The scheduled-playback
sequence follows the SDK's documented order — `EnableVideoOutput`, pre-roll,
`StartScheduledPlayback`, top up on completion — but no card has been connected.
Unknowns worth expecting: whether the pre-roll depth is right in practice,
whether the audio and video stream clocks stay aligned over hours, and how the
buffer level behaves when the engine's software clock and the card's genlocked
clock disagree. `buffered_frames` in `/api/state` is the number to watch.

**DeckLink key + fill.** `--key` enables the card's keyer through
`IDeckLinkKeyer`, switching the output to 8-bit BGRA so the alpha survives, and
checks `BMDDeckLinkSupportsExternalKeying`/`SupportsInternalKeying` first so an
unsupported card gets a clear message rather than a bare failure. Compiles
against SDK 12.2, and the alpha it feeds the card is verified correct by the NDI
measurement above — but no card has confirmed that fill and key actually appear
on separate connectors.

**AJA.** Compiles against libajantv2 18.1. AutoCirculate playout follows the
SDK's documented sequence. No card connected. Also untested: whether
`AcquireStreamForApplicationWithReference` cooperates properly with the AJA
Control Panel, and whether the routing is right for anything other than a single
SDI output on the selected channel.

**OMT.** Compiles against `libomt.h` from the v1.0.0.16 binary release, and the
frame construction follows the header's documented contract. No OMT receiver has
consumed its output. The `libomt.dylib` on macOS was observed to be a Swift
binary with no `.NET` runtime dependency despite `libomtnet.dll` shipping beside
it; that is an observation, not documentation, and it may not hold on Windows.

**Windows and Linux.** The CMake, the process model and the platform code paths
are written for both, and the CEF distributions are pinned. Neither has been
built or run. Expect the Windows DeckLink path in particular to need work: it
uses COM rather than the dispatch shim, and that code has never seen a compiler.

**Alpha output.** `--alpha` is implemented for NDI and OMT and has not been
checked against a downstream keyer.

**Anything at 4K over a sustained period.** 2160p25 was exercised only as part of
the format-change sequence, for a few seconds.
