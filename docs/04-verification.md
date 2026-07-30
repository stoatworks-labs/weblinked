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
# 46 tests, 24882 checks, 0 failures
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

## 6. Clean shutdown — verified

`SIGTERM` produces `shutdown requested by signal` → `audio stream stopped` →
`WebLinked exiting cleanly`, with the NDI sender destroyed rather than abandoned.

This is not cosmetic. Killing an earlier build without a handler left a stale
source advertised on the network, and the next run was undiscoverable for
several minutes — which looked exactly like a broken sender.

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
