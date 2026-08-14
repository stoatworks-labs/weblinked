# Verification

What has actually been proven about this code, how, and what has not. Written so
that a claim in the README can be checked rather than believed.

The distinction that matters throughout: **compiles against a real SDK** is not
the same as **works against real hardware**. NDI and DeckLink have now been
measured against real hardware (sections 2, 19 and 19a); OMT and AJA remain in
the first category.

Everything below was run on macOS 26.4.1, Apple Silicon (M4 Max), against
CEF 150.0.17 / Chromium 150.0.7871.187.

---

## 1. Unit tests — verified

```bash
cmake --build build && ./build/tests/weblinked_tests
# 89 tests, 25225 checks, 0 failures
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
`WebLinked exiting cleanly`, with the NDI sender destroyed rather than abandoned.

There is only one mode now that the operator window is gone (section 9), and the
shutdown path is correspondingly simpler: `beginShutdown()` quits the message
loop directly rather than closing a window first. Re-verified after that change.

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

## 9. The operator window — removed in v0.5.3

This section used to claim the window opened and showed the control page. That
claim did not survive contact with a released build: on macOS the window came up
correctly sized, titled `WebLinked - Chromium`, and **completely empty**, with
the GPU process segfaulting on startup every time.

The cause is structural rather than a bug to patch. Every source browser here is
windowless, windowless rendering *always* uses Alloy runtime style, and a
windowed browser defaults to Chrome style — so the process ran two runtime styles
at once. Measured, not assumed: a headless run crashes the GPU process zero
times, a windowed run crashes it every time.

```
grep -c "GPU process exited unexpectedly" headless.log   # 0
grep -c "GPU process exited unexpectedly" windowed.log   # 1
```

It also never had a menu bar — `NSApp.mainMenu` was never set, so the
application menu had zero items, ⌘Q did nothing, and there was no way to quit it
from the UI.

Forcing `CEF_RUNTIME_STYLE_ALLOY` on the window would likely have fixed the
compositor, but the window was the wrong shape for the product regardless: this
process is a render host and a control server, and the operator's view is a
browser. It is now removed outright, and the tray launcher in `launcher/` is what
puts that view on a desktop. `--headless` is accepted and ignored.

Verified after removal: no top-level window exists (the window server reports
zero for the process), the GPU process no longer crashes, the control page still
serves, and `SIGTERM` still produces `shutdown requested by signal` →
`WebLinked exiting cleanly`.

## 10. Popups and new tabs — verified

The bug this fixes was reported from testing as "clicking a link that opened a
new tab crashed the application". Reproduced and measured against a test page
carrying both routes: an `<a target="_blank">` and an `onclick` calling
`window.open`.

The click was delivered through `/api/input` at normalised coordinates, so the
test drives exactly the path an operator uses.

**Default policy (`--popups navigate`)**, clicking the `target="_blank"` link:

```
before:  loaded_url  file:///.../popup.html      popups 0
after:   loaded_url  https://example.com/        popups 1
         last_popup  https://example.com/
log:     browser: popup to https://example.com/ redirected into the main frame
process: alive
```

And the source was still the source afterwards, which is the part that matters —
the old failure left the engine driving a browser nothing was reading:

```
preview frames: 1095 -> 1197 over ~2s   (~51/s at 1080p50)
```

**`--popups block`**, both routes on the same page:

```
window.open      -> popups 1, last_popup https://example.org/, url unchanged
target=_blank    -> popups 2,                                  url unchanged
log:  browser: blocked a popup to https://example.org/
      browser: blocked a popup to https://example.com/
process: alive
```

## 11. Settings — verified

Round-tripped through a real file and a real restart.

```
POST /api/output/add     {"kind":"ndi","name":"SettingsTest","options":{"alpha":true}}
POST /api/output/update  rename to RenamedTest, drop alpha
  -> preview (preview) running, RenamedTest (ndi) running
POST /api/settings/save  -> settings.json written
```

Killed and restarted **with no `--url` and no `--ndi`**:

```
url      https://example.com          <- from the file
outputs  [preview, RenamedTest]       <- from the file
```

Restarted again **with** `--url https://iana.org --no-interactive`:

```
url                     https://iana.org   <- command line wins
outputs                 [preview, RenamedTest]  <- file still fills in the rest
interactive_by_default  false               <- command line wins
```

`/api/settings/apply` applied a 720p59.94 / BT.601 / block-popups configuration
and both outputs stayed running through it; `/api/settings/reload` put all three
back from the file. Rejections are rejections: an unparseable format answers 400
naming the format, a duplicate output name answers with the clash.

Six unit tests cover the store itself, including that the atomic write leaves no
`.tmp` behind and that a corrupt file is reported as such rather than silently
ignored.

## 12. Diagnostics in the app — verified

```
GET  /api/log?lines=3        level, path, and the last three lines
POST /api/log/level          {"ok":true,"level":"debug"}, and /api/log agrees
POST /api/diagnostics/report -> WebLinked-crash-20260731-043449.json
GET  /api/diagnostics/bundle -> 200, application/json,
     Content-Disposition: attachment; filename="WebLinked-diagnostics-...json"
     schema stoatworks.diagnostics/1, 16 log lines, config present
```

The level is served lower case and unpadded — `diag` pads its names to five
characters for the log file, and `"INFO "` would never have matched the control
page's selector.

## 13. The control page's three views — verified

Loaded in a browser against a running instance. **No console errors.** The
preview showed the live page; the interactive toggle came up **armed**, with the
canvas outlined, from `settings.interactive_by_default`.

Per-backend field visibility was asserted from the live DOM rather than by
looking at it:

```
preview      -> factor
ndi          -> alpha
decklink     -> device, keying
```

That check found a real bug: `.check { display: flex }` outranks the browser's
own `[hidden] { display: none }`, so the alpha checkbox was showing on the
preview output — an option that does nothing there. Fixed with an explicit
`[hidden]` rule and re-asserted.

The diagnostics view showed the log with levels coloured, the log paths, the
settings path, and the popup counters.

## 14. The tray launcher — partially verified (RETIRED in v1.0.0)

> The separate Tauri launcher this section describes no longer exists. The
> engine puts its own icon in the menu bar and system tray — section 31. This
> and sections 18 and 21 are kept because they record what was true when they
> were written, not because any of it still ships.


- Builds; `cargo test` passes 7 tests, one of which parses the `launcher.toml`
  this repo actually ships and asserts the argv it produces, `--headless`
  included.
- The release binary starts and logs to its own directory
  (`~/Library/Logs/WebLinked Launcher`), confirming it is a separate diagnostics
  identity rather than av-launcher's.
- The panel renders in WebLinked's palette, from the real HTML and CSS.

**Not verified:** Start / Stop / Launch GUI driven against a live WebLinked, and
the built `.app` on a machine that has never seen the source. See
[../launcher/README.md](../launcher/README.md).

---

## 15. Several sources in one process — verified, with a capacity limit

Three independent pipelines from one `--config` file, each with its own browser,
clock, raster and NDI sender:

```bash
weblinked --config three.json --headless
```

| Source | Raster | NDI name |
|---|---|---|
| `clock-a` | 1920x1080p50 | WL-A |
| `clock-b` | 1280x720p50 | WL-B |
| `bars` | 1920x1080p25 | WL-C |

All three came up, and `tools/ndi_probe` — a separate receiver — confirmed each
one independently rather than trusting the app's own counters:

```
WL-A  received 60 video frames in 1.17 s — 51.27 fps   stride 3840 (1920 wide)
WL-B  received 60 video frames in 1.16 s — 51.63 fps   stride 2560 (1280 wide)
WL-C  received 60 video frames in 2.33 s — 25.78 fps   stride 3840 (1920 wide)
```

Three different rasters and two different rates, on the network at the same time.

**Isolation is the claim worth testing, and it holds.** Retargeting `clock-b`
over `POST /api/url?source=clock-b` left `clock-a` and `bars` untouched. Adding
a fourth source at runtime and removing it again left the other three delivering
— the receiver still passed on all three afterwards, and `WL-D` was gone from
the network. A duplicate id is refused (`409`), and an unknown one is a `404`
rather than a silent no-op on the wrong feed.

**OSC is addressed the same way.** `/weblinked/source/clock-b/url` reached only
`clock-b`; a bare `/weblinked/url` went to the primary; `/weblinked/source/ghost/url`
logged `no source called 'ghost'` and changed nothing.

### The capacity limit, measured rather than assumed

Three sources at **1080p50 + 720p50 + 1080p25 do not run clean on this Mac.**
Measured over 60 s, with an independent receiver confirming it from outside:

| Source | Ticks / 60 s | Dropped | Receiver measured |
|---|---|---|---|
| clock-a (1080p50) | 3012 | 126 (4.2%) | **46.27 fps** against a nominal 50 |
| clock-b (720p50) | 3012 | 98 (3.3%) | — |
| bars (1080p25) | 1506 | 45 (3.0%) | — |

`ticks − dropped == frames sent` exactly, so a dropped tick here means a frame
that never went out, not a counter artefact.

**This is the machine running out of work capacity, not the pacing breaking.**
The same three sources at 720p25 drop **3 ticks in 1134 over 45 s (0.26%)**, and
the receiver measures 25.15, 25.15 and 25.14 fps against a nominal 25 — three
concurrent sources, clean. What changes between the two runs is only how much
pixel work there is. During the 1080p run, WebLinked's eight processes were
using ~900% CPU on a 16-core machine with a load average above 25.

So: the number of sources one process can carry is a property of the machine and
the rasters, and there is no configuration in WebLinked that will tell you what
it is. **Measure it on the machine that will run the show**, with
`tools/ndi_probe` or a real receiver, before trusting a count. The single-source
soak in section 4 remains the reference for what clean looks like: 9001 ticks in
180 s, zero dropped.

### The control page

Verified in a browser against the three-source instance: the strip shows one chip
per source with its own live thumbnail, clicking one switches every panel to it,
and the strip hides itself entirely when there is only one source, so a
command-line launch is unchanged. Two bugs found doing it — see below.

---

## 16. Two instances at once — verified

Section 15 covers several *sources* inside one process. This is the other axis:
two separate WebLinked processes, which until v0.5.2 was not reliably possible
at all.

The bug was that no Chromium profile directory was ever set, so every instance
fell back to CEF's default — one directory shared by every CEF application on
the machine. Chromium permits exactly one browser process per profile directory,
so the second instance found the lock held and showed *"Your profile could not be
loaded correctly"* instead of starting. Reproduced from the shipped v0.5.1 app:
the lock in `~/Library/Application Support/CEF/User Data` was held by one
WebLinked while a second refused to come up, and CEF's own log said so —
`Please customize CefSettings.root_cache_path for your application. Use of the
default value may lead to unintended process singleton behavior.`

Verified after the fix, on macOS arm64:

```bash
WebLinked --headless --no-preview --no-osc --port 7801 &
WebLinked --headless --no-preview --no-osc --port 7802 &
```

Both come up and both answer on their own control port. Each holds its own lock,
in its own directory, from its own process:

```
…/WebLinked/profiles/7801/SingletonLock -> Mac-60106
…/WebLinked/profiles/7802/SingletonLock -> Mac-60153
```

`grep root_cache_path` over both logs returns nothing — the CEF warning is gone,
which is the check that the path is actually being applied rather than merely
computed.

A third instance on a port already taken exits 1 before Chromium starts:

```
control port 7801 on 127.0.0.1 is already in use — another WebLinked is
probably running. Quit it, or give this one --port.
```

That ordering is the point. The port clash used to reach the profile lock first
and surface as the dialog above, which says nothing about ports and sends you
looking in the wrong place.

**Not verified:** the same on Windows and Linux, which share the code path but
have not been run (see below); and whether two instances driving real SDI cards
contend for anything at the driver level, which is a hardware question and no
card has ever been connected.

---

## 17. Tabs, and per-tab output routing — verified

Driven in a real browser against a running instance, through the page's own
handlers rather than by calling the API behind it.

Starting from a default launch (one source), clicking `+ tab`, filling the inline
form and submitting it:

```
sources: main   1920x1080p50  running=True
         lower3 1280x720p50   running=True
```

Then an NDI output added to `lower3` alone:

```
[main]   preview  preview                 running=True
[lower3] preview  preview                 running=True
         ndi      Lower3-NDI              running=True
```

That is the property that matters: the outputs belong to the tab, not to the
process. Also verified in the same session — the new tab is auto-selected, the
form closes on success, a duplicate id is refused client-side with the form left
open (`a tab called 'lower3' already exists`) and no request sent, and the
console is free of errors throughout.

**The bug this found is the feature itself.** Several sources have worked in the
API and in `--config` since v0.4.0, and section 15 verified three at once. None
of that was reachable from the page: the source strip hid itself below two
sources, and the `+` button lived inside the strip — so a default launch could
never get to a second one. Verifying the engine is not the same as verifying the
route an operator takes to it.

The three output modes — **Fill only**, **Key + fill**, **Overlay** — were
checked in the DOM against a DeckLink output on a build with the DeckLink SDK
present, confirming the labels map to `""`, `"external"` and `"internal"` and
that they appear only for backends that have a keyer. **Nothing here has been
near a card**: the modes are a renaming of options `decklink_output.cpp` already
read, and the keying path remains unverified against hardware exactly as before.
AJA no longer offers the control at all, because `aja_output.cpp` implements no
keying.

---

## 18. The tray launcher — built, not yet driven (RETIRED, see 14)

Improved from section 14 but still not end-to-end.

- Builds on this Mac into `WebLinked Launcher.app` and
  `WebLinked Launcher_0.6.0_aarch64.dmg`, with `works.stoat.weblinked.launcher`
  as its identifier — so it no longer collides with WebLinked's own
  `/Applications/WebLinked.app`, which the previous `productName` did.
- The bundled `launcher.toml` is confirmed present inside the built `.app` and
  points at the installed WebLinked.
- `cargo test` passes 8 tests. The new one parses all three platform configs and
  asserts the argv each produces — which is how the Windows one was caught being
  **unparseable**: a Windows path in a TOML basic string makes `C:\Program
  Files\...` an invalid escape, so the whole file failed to load. That would
  have shipped a launcher that starts and cannot find its own configuration.

**Not verified:** Start / Stop / Launch GUI clicked against a live WebLinked; the
Windows and Linux bundles, which only CI builds; and the `.app` on a machine that
has never seen the source.

---

## 19. DeckLink, against a real card — verified, and it found two things

The first hardware this project has ever been connected to. Everything above
about SDI was "compiles against the SDK"; this is not.

**The card:** a DeckLink Duo 2 over Thunderbolt, Desktop Video 16.0.1, on the
same M4 Max. It presents four sub-devices, and the profile it is in matters:

```
index 0  DeckLink Duo (1)   full duplex   ext key: yes  int key: yes
index 1  DeckLink Duo (2)   full duplex   ext key: yes  int key: yes
index 2  DeckLink Duo (3)   INACTIVE      ext key: no   int key: no
index 3  DeckLink Duo (4)   INACTIVE      ext key: no   int key: no
```

Indices 2 and 3 support **no display modes at all** in this profile. A
`--decklink=2` is not a missing card, it is a card whose profile has that
sub-device switched off — worth knowing before reading the error.

### Output, and a loopback that proves it

Verified the way NDI was, with an independent receiver: `sdi_probe`, written
against the DeckLink SDK with its own BT.709 reference, capturing what actually
came back down a BNC cable rather than reading WebLinked's counters.

### Which connector is which — partly, and honestly

An earlier revision of this document published a confident connector map. Later
cable positions contradicted it, so it is withdrawn: what the physical BNC labels
correspond to could not be pinned down from software, and guessing produced two
wrong asks in a row.

What is solid, because each line was measured:

| Cable position (as labelled on the card) | Transmit | Capture | Result |
|---|---|---|---|
| 1 to 3 | index 0 | index 0 | locks, all bars exact |
| 1 to 2 | index 0 | index 0 and 1 | dark on both |
| 1 to 4 | index 0 | index 0 and 1 | dark on both |
| 1 to 4 | **index 1** | **index 0** | locks, `no-source=0`, bars exact |

The operational facts that follow, which is what actually matters:

- Each active sub-device has one output and one input, and they are **not** the
  connector pair you would guess from the numbering.
- A loop can be same-sub-device or cross-sub-device depending on which pair is
  patched, and only a cross one can capture externally keyed **fill** — keying
  consumes the second connector of the transmitting pair for the key.
- **Diagnose it rather than reason about it.** `tools/sdi_probe.mm` reports
  `no-source` frames separately from good ones, so "input locked, nothing
  arriving" is distinguishable from "not the input you think". A free-running
  input with nothing plugged in reports mode `ntsc` and a mix of both, which is
  the signature of no lock rather than of a signal.

With SDI out (connector 1) looped to SDI in (connector 3) — the same sub-device,
index 0:

```
bar        sampled Y/Cb/Cr    expected Y/Cb/Cr   verdict
grey        181  128  128       181  128  128       ok
yellow      169   44  136       169   44  136       ok
cyan        146  147   44       146  147   44       ok
green       134   63   51       134   63   51       ok
magenta      63  193  205        63  193  205       ok
red          51  109  212        51  109  212       ok
blue         28  212  120        28  212  120       ok
black        16  128  128        16  128  128       ok

frames received: 29  (1920x1080)   PASS
```

Every bar exact, at **1080p50 and again at 1080p25**. The colour path is correct
on an SDI wire, not merely internally consistent.

### The buffer question, answered

`docs` has asked since v0.1.0 "whether the pre-roll depth is right in practice"
and "how the buffer level behaves when the engine's software clock and the card's
genlocked clock disagree". Over a two-minute soak at 1080p50:

```
  t     dl_frames  buffered   ticks  dropped  repeated
 10s       1546         6     1542        0         0
 60s       4053         6     4049        0         2
120s       7061         6     7057        0         2
```

`buffered_frames` sat at 6 the entire time (one sample of 5, immediately back),
zero dropped ticks, 50.2 fps sustained. The scheduled-playback sequence and
pre-roll depth are right on this card.

### Keying — engages, with a bandwidth ceiling nobody had hit

**Both modes start on real hardware** at 1080p25:

```
keying=external   running=True   frames 371   buffered 5
keying=internal   running=True   frames 370   buffered 5
```

At **1080p50 both fail**, and the reason is the finding:

```
'DeckLink Duo (1)' reports 1920x1080p50 as unsupported for 8-bit BGRA output
```

Asked of the hardware directly, this card supports **no** alpha-carrying pixel
format at 1080p50 — but does at every lower rate:

```
1080p50 :  8BitYUV=yes  8BitBGRA=no   8BitARGB=no
1080i50 :  8BitYUV=yes  8BitBGRA=yes  8BitARGB=yes
1080p25 :  8BitYUV=yes  8BitBGRA=yes  8BitARGB=yes
720p50  :  8BitYUV=yes  8BitBGRA=yes  8BitARGB=yes
```

RGBA costs roughly twice the link rate of the 4:2:2 that fits the same raster, so
a keyed 1080p50 is over what the card will carry. Nothing in the code was wrong,
but the message stopped at "unsupported" — and the guard it is documented as
having (`SupportsExternalKeying`/`SupportsInternalKeying`) *passes* on this card,
because the card really does have a keyer. The capability check and the
bandwidth limit are different questions. The error now asks the card which rates
it would accept and names them:

```
... Keying carries alpha, which needs about twice the link rate of the same
raster without it; this card accepts a keyed 1920x1080 at 24000/1001,
24000/1000, 25000/1000, 30000/1001, 30000/1000, 25000/1000 (interlaced), ...
```

### Key + fill, measured — the fill carries straight alpha

The measurement worth having. With external keying on index 1 and a
cross-sub-device capture on index 0, the **fill** off the wire, against four
bands of known alpha:

```
bar            sampled Y/Cb/Cr    expected Y/Cb/Cr   verdict
red a=1.0       51  108  212        51  109  212       ok
green a=0.5    132   63   52       134   63   51       ok
blue a=0.25     27  211  120        28  212  120       ok
empty a=0       16  128  128        16  128  128       ok
PASS
```

The 50%-alpha green reads **132**. Premultiplied it would read about **95** —
which is exactly the value the NDI path produced before `unpremultiplyBgra`
existed (section 7). So the fill of a keyed SDI signal carries *straight*
(unassociated) colour, correctly, and the bug that was real on NDI is not present
here. This is invisible on opaque graphics, so nothing short of this measurement
would have shown it either way.

Also confirmed: with external keying on, the second connector of the
transmitting pair stops being an input and becomes the **key** output — a
same-sub-device loopback goes dark exactly when keying is enabled and captures
perfectly with it off, at an otherwise identical raster and rate.

### Internal keying, over a live input

Overlay composites over whatever the card is receiving, so keying over our own
output is a feedback path rather than a test. Driven properly instead, with two
WebLinked processes at once — the thing v0.5.2 made possible — one transmitting a
background into the other's input:

```
port 7801  device 1  keying off       running  frames 694  buffered 5
port 7802  device 0  keying internal  running  frames 371  buffered 6
```

The keyer engages against a genuine incoming signal and both sub-devices run
from separate processes without interfering. **The composite itself is not
verified**: with the cable feeding the background in, there is no spare input
left to capture the keyed result on.

**Still not verified, and now for concrete reasons:**

- **The key channel itself.** The fill is measured above; that the *key* output
  carries alpha as luma is not. It needs the key connector of the transmitting
  pair patched to a free input.
- **The internal-keying composite.** The keyer engages over a live input, but
  capturing the composited result needs a third connection this cabling has no
  input left for.
- **Audio over SDI.** Not exercised at all.
- **Genlock over hours**, rather than two minutes.
- **AJA.** Still no card. Unchanged from "compiles and nothing more".

---

## 19a. The same card in a different profile — re-verified, and the map above is withdrawn

Run at v0.7.0 on the same Duo 2, and the first thing it established is that
**section 19's connector table describes a profile the card is no longer in**.
Desktop Video profiles are persistent and change which sub-devices exist, so a
connector map is only true for the profile it was measured in. This one was
measured; do not carry either table across a profile change.

`tools/dl_profile.mm` asks the card what it is rather than assuming:

```
index 0  DeckLink Duo (1)  half duplex  in:yes out:yes  ext key:no  int key:no
index 1  DeckLink Duo (2)  half duplex  in:yes out:yes  ext key:no  int key:no
index 2  DeckLink Duo (3)  half duplex  in:yes out:yes  ext key:no  int key:no
index 3  DeckLink Duo (4)  half duplex  in:yes out:yes  ext key:no  int key:no
           output modes: 1080p50/YUV=yes 1080p25/YUV=yes
                         1080p50/BGRA=no 1080p25/BGRA=yes
```

Against section 19's two full-duplex sub-devices with two inactive, this profile
has **all four active, half duplex, and no keyer at all**. Half duplex is what
makes the map unambiguous: each sub-device owns exactly one BNC, so the physical
labels line up with the indices, which the loopback then confirmed.

| Cable position | Transmit | Capture | Result |
|---|---|---|---|
| 1 to 4 | index 0 | index 3 | locks, all bars exact |

### The round trip, at both rates

Transmitted `tools/testcard.html`, captured with `sdi_probe` and its own BT.709
reference — the receiver is not reading WebLinked's counters:

```
bar        sampled Y/Cb/Cr    expected Y/Cb/Cr   verdict
grey        181  128  128       181  128  128       ok
yellow      169   44  136       169   44  136       ok
cyan        146  147   44       146  147   44       ok
green       134   63   51       134   63   51       ok
magenta      63  193  205        63  193  205       ok
red          51  109  212        51  109  212       ok
blue         28  212  120        28  212  120       ok
black        16  128  128        16  128  128       ok

frames received: 26  (1920x1080)   PASS
```

Identical at **1080p50 and 1080p25**, and identical to the values section 19
recorded on the other profile — the colour path does not depend on which
sub-device carries it.

### The buffer, again

Ninety seconds at 1080p50, sampling `/api/state`:

```
  t      frames   completed  buffered  late  dropped
  0s      2635      2629         6       0       0
 30s      4137      4131         6       0       0
 60s      5638      5632         6       0       0
 90s      7139      7133         6       0       0
```

4504 frames in 90s = **50.04 fps**, `buffered_frames` pinned at 6 with no
excursion at all, nothing late, nothing dropped. Section 19 saw one dip to 5;
this run saw none.

### Keying refused correctly — the case section 19 could not reach

Section 19 noted that the `SupportsExternalKeying`/`SupportsInternalKeying`
guard *passed* on that card and so proved nothing. This profile has no keyer, so
the guard is finally exercised in the direction that matters, and it fires:

```
--key=external --format 1080p25 -> 'DeckLink Duo (1)' does not support external keying
--key=internal --format 1080p25 -> 'DeckLink Duo (1)' does not support internal keying
```

In both cases the output reports `running: false` and the process stays up
rather than dying — a refused output is not a crashed one.

### A real defect this found: the wrong remedy at 1080p50

Asking for keyed **1080p50** on this profile produces the *bandwidth* message
instead:

```
'DeckLink Duo (1)' reports 1920x1080p50 as unsupported for 8-bit BGRA output.
Keying carries alpha, which needs about twice the link rate ...; this card
accepts a keyed 1920x1080 at 24000/1001, 24000/1000, 25000/1000, ...
```

Every rate it names is wrong **for this profile**, because the card has no keyer
at any rate here. `configureOutput`'s mode check runs before `configureKeyer`
(`decklink_output.cpp`), so the bandwidth explanation wins and sends the operator
to 1080p25 — where they hit "does not support external keying" instead. The
message's own code comment assumes the card "reports keying support and every
rate as available", which was true of the full-duplex profile section 19 measured
and is not true generally.

Not a wrong refusal — the output correctly fails either way — but the advice
attached to it is misleading, and only a profile without a keyer exposes it.

### What this run did and did not settle

Settled, on hardware, and now on **two different profiles**: the output path,
the scheduled-playback sequence, pre-roll depth, buffer behaviour, the colour on
the wire at 1080p50 and 1080p25, and the keyer capability guard in both
directions.

Still open, and now for a **profile** reason rather than a cabling one — key +
fill content, the key channel, and the internal-keying composite all need a
keying-capable (full-duplex) profile, which this card is not currently in.
Audio over SDI and genlock over hours remain untouched.

---

## 20. The screen output — verified on this Mac

`--screen` puts the rendered frame fullscreen on a GPU-attached display. It is
an `IOutput` like any other, which is the point: the frames it shows are the
same objects the SDI and NDI outputs are handed, so it cannot drift from them.

It is **not** a second Chromium window. Every browser here is windowless, which
forces Alloy runtime style; a windowed browser defaults to Chrome style, and one
process running both crashed the GPU process every time (section 9). This takes
frames the engine has already produced and puts them on the glass with Metal.

Run against a local bars page at 1080p50 on the built-in display:

```
--url file://…/bars.html --screen=0 --format 1080p50
```

**Picture — correct.** Fullscreen, right way up, all four corner markers and the
full magenta border visible, letterboxed top and bottom. That last part is the
expected result rather than a fault: 1920x1080 is 1.78:1 and this display is
4112x2658, which is 1.55:1, so `fit` bars the vertical axis.

**All three scaling modes — correct**, checked by screenshot:

| Mode | Expected | Observed |
|---|---|---|
| `fit` | whole frame, bars where the aspects differ | bars top and bottom, all four corners visible |
| `fill` | fills the display, crops the long axis | no bars, corners and part of the HUD cropped away |
| `stretch` | fills, ignores aspect | no bars, all corners visible, geometry distorted |

**Pacing is the display's, not the engine's — measured, and this is the part
worth reading.** The counters at first looked wrong: at 1080p50 the output
reported 50 submitted and 50 presented per second, which is exactly what a bug
that only draws on a new frame would produce. It is not that. Re-run at 1080p25:

```
at 1080p25 over 10s: submitted=25/s  presented=50/s  dropped=0
```

Each frame is presented twice, with nothing dropped, because the panel is
running at 50 Hz — confirmed independently by `/api/state`, which now reports
`displays[].refresh_hz: 50`. The 50p case matching so exactly was two
independent ~50 Hz clocks, not one clock driving both. At 50p the same run shows
roughly 8 dropped frames a second, which is those two clocks beating against
each other and is expected: `dropped` counts a frame overwritten before a
refresh could show it, and a source and a head at the same nominal rate but
unsynchronised will do that continually.

So `presented` and `frames` are *supposed* to differ. That is what makes
repetition on a faster head distinguishable from a genuine drop.

**Live add and remove — verified, after fixing a crash it found.** Four full
remove/add cycles through `/api/output/remove` and `/api/output/add`, plus a
fifth driven through the settings page itself (named `Projector`, scaling
`fill`), with the process surviving all of them and the counters resetting each
time. This exercises the path where `start()` arrives on the **HTTP thread**
rather than the main thread, which on macOS is the one that cannot create a
window; the backend marshals with `dispatch_sync` to the main queue.

The first attempt at this killed the process on the second cycle. See the bug
list below — it was an over-release, and it is exactly the kind of thing a
single happy-path run does not find.

**A display index that does not exist — handled.** `--screen=5` with one display
attached does not take the process down: the output fails to start, the rest of
the app carries on, and the reason reaches both the log and `outputs[].error`:

```
ERROR output 'screen5' (screen) failed to start: display 5 does not exist (1 attached)
```

**A second display — verified, and it found the bug that mattered most.** An
ASUS PA148 (1920x1080, 60 Hz) was attached after the first pass and `--screen=1`
run against it. See the bug list: the window was landing off-screen on every
display except the main one, while every counter claimed success.

After the fix, on the external head at 1080p50:

```
submitted 50.1/s  presented 60.1/s  dropped 5 (in 15s)  ratio 1.199
```

1.199 against a theoretical 60/50 = 1.200. This is the claim the whole design
rests on — that presentation follows the *display* and a slower source is
repeated rather than torn — measured on a real second head rather than inferred
from one panel. Picture confirmed by screenshotting that display: 1920x1080
content on a 1920x1080 head, so edge to edge with no bars, all four corner
markers and the full border present.

Worth recording that the built-in panel reported 50 Hz during the first pass and
120 Hz during this one. It is a ProMotion display and its refresh genuinely
varies, which is exactly why `displays[].refresh_hz` is read live rather than
cached, and why the first pass's numbers looked suspicious.

**Still not verified.** Colour was checked against the same bars page rather than
against `ndi_probe`'s independent BT.709 reference; the layer's colour space is
pinned to sRGB precisely so it *can* be compared that way, but that comparison
has not been made. Nothing has been shown on a projector. Three or more displays
are untested.

**Windows and Linux: never run.** `screen_window_win.cpp` (D3D11) and
`screen_window_linux.cpp` (X11 + EGL) are written and compile-targeted only.
Linux additionally disables the whole backend when X11 or EGL headers are
absent, the same way a missing NDI SDK disables NDI.

---

## 21. The launcher carrying WebLinked — mostly verified (RETIRED, see 14)

The launcher now ships WebLinked inside itself, so the macOS download is one
install rather than two applied in the right order. `launcher/README.md`
previously argued against this. Both of its reasons were real, and both are
addressed rather than ignored.

**The Tauri resource walk — solved, verified.** The collector follows the CEF
framework's `Versions/Current` and `Resources` symlinks and dies on
`.../Resources/Resources`. Shipping one `.zip` gives it a single regular file.
`npm run tauri build` completes and produces a 142 MB `.app` and a 144 MB
`.dmg`, from a 130 MB archive.

**`ditto` round-trips the framework — verified.** Packed with
`ditto -c -k --sequesterRsrc --keepParent` and expanded again, the three
framework symlinks are still symlinks and all five helper `.app`s are present.

**Gatekeeper — verified for this path, and it removed a step.** The unpacked
copy was run directly from Application Support:

- it starts, serves, paints and drives the screen output;
- the renderer and GPU helpers stay alive at the new path — the specific thing
  that dies when the nested-bundle failure mode bites;
- the ad-hoc signature survives the archive intact: `flags=0x2(adhoc)` with the
  original `works.stoat.weblinked` identifier;
- the extracted files carry no `com.apple.quarantine`.

All of that with **no post-processing at all**. An earlier version of
`embedded.rs` re-signed with `codesign --force --deep --sign -` on the
assumption the helper signatures needed re-establishing. They do not, the
verification of that signature was itself failing, and `--deep` contradicts this
project's own inside-out signing rule (`cmake/SignMacBundle.cmake`). It was
removed. The quarantine clear stays as one cheap line, because the case it
defends — a launcher `.dmg` downloaded from GitHub, whose contents *are*
quarantined — cannot be reproduced from a source checkout.

**The unpack logic — covered by tests.** `cargo test` is 15 tests, up from 9,
including a full unpack → short-circuit-on-stamp → re-unpack-on-upgrade cycle
against a real `ditto` archive, and a corrupt-archive case asserting that a
failed unpack leaves no stamp behind to be trusted later.

**Not verified: the tray click-through.** Still true, and still for the reason
section 18 gives. The panel renders correctly from the shipped bundle and the
window is there, but the Start button lives inside a WKWebView whose contents
are not exposed in the accessibility tree — only the window's three traffic
lights are — so it could not be driven from a script. The sequence that button
triggers was verified by hand instead, against the real 130 MB archive. What
remains untested is the Tauri glue between them: `resource_dir()` resolving and
`ensure_unpacked` being reached from a click.

**Not verified: a machine that has never seen the source.** Everything above ran
on the build machine.

---

## 22. Per-output background colour — verified over NDI

The same page leaving one process twice: transparent for a keyer, composited
over a flat colour for a switcher that only has a chroma keyer. Measured off the
wire with `ndi_probe --save`, against a reference computed here from the
definition of the `over` operator rather than from anything in the app.

`tools/alphabars.html` at 1080p50 — opaque red, 50% green, 25% blue, and nothing
at all — with the background switched at run time through
`POST /api/output/background`.

Transparent, the baseline, which is what every output did before this existed:

```
  band          sampled R/G/B     expected R/G/B
  opaque red     191    0    1      192    0    0
  50% green        1   96    0        0   96    0
  25% blue         0    0   48        0    0   48
  nothing          0    0    0        0    0    0
```

The same page on the same output, composited over `#00b140`:

```
  band          sampled R/G/B     expected R/G/B
  opaque red     191    0    1      192    0    0
  50% green        0  184   33        0  184   32
  25% blue         0  133   96        0  133   96
  nothing          1  177   64        0  177   64
```

Everything within one code value, which is the 4:2:2 round trip and studio-swing
quantisation, not the composite. The band that matters most is the last one:
nothing painted comes back as **exactly `#00b140`**, not a shade off it — a
keyer set to a nominal green and fed 0x00b03f leaves a fringe round every
graphic.

**Per output, not per source — verified with two senders at once.** A second NDI
output added on `#0000ff` while the first stayed on green, both fed by one
browser paint:

```
  band          sampled R/G/B     expected R/G/B
  opaque red     191    0    1      192    0    0
  50% green        1   96  127        0   96  127
  25% blue         0    0  239        0    0  239
  nothing          1    0  255        0    0  255
```

**Changing it does not restart the output.** The reason it is a field on the spec
rather than an entry in `options`: nothing in a backend acts on it, so the change
is an assignment under the lock the clock thread already reads specs under.
Driving it from the settings page, the sender's own frame counter ran 8923 →
9751 straight through the change, and the log records the assignment rather than
a reopen. Reopening a DeckLink to nudge a green would drop frames on air.

**Pacing is unaffected.** 4,687 ticks with three outputs, two of them
compositing: **0 dropped ticks**. The composite is one pass over the frame with
a fast path for opaque pixels, and it is cached per colour per tick, so four
feeds on the same green cost one composite between them.

**The control page.** Asserted from the DOM rather than a screenshot, per the
`[hidden]` trap in section 13: the colour well and its hint are `display: none`
while the background is transparent, appear on the toggle, and every editor
populates from `/api/state`. No console errors.

**Not verified:** the composite reaching a real chroma keyer — no switcher has
consumed one of these feeds. That it is the right *colour* on the wire is
measured above; that a given keyer likes it is a question about the keyer.
DeckLink and AJA were not compiled into this build, so the composite has only
been measured down the NDI path — the same frame path serves all of them, but
"the same code" is not "was run".

---

## 23. The shared output, into Resolume Arena — verified, and it found two traps

A page on a VJ layer without leaving the machine: `--syphon` publishes an
IOSurface, another application picks it up. Verified three ways, because the
interesting failures are not the same failure:

* `tools/syphon_probe.mm`, an independent receiver, for the pixels;
* Resolume Arena 7.27.1 itself, for the thing this is actually for;
* the pacing counters, for what the copy costs.

**The probe is independent in the way that matters.** It links **Arena's own
bundled Syphon 5 framework**, not the Syphon 6 server sources vendored into
`third_party/syphon`. A pass is two implementations shipped years apart by
different people agreeing about the protocol, rather than this repository
agreeing with itself.

```bash
clang++ -std=c++20 -fobjc-arc -Wno-deprecated-declarations tools/syphon_probe.mm \
  -F "/Applications/Resolume Arena/Arena.app/Contents/Frameworks" \
  -framework Syphon -framework Foundation -framework IOSurface \
  -framework OpenGL -framework Cocoa \
  -rpath "/Applications/Resolume Arena/Arena.app/Contents/Frameworks" -o syphon_probe

./build/Release/WebLinked.app/Contents/MacOS/WebLinked \
  --url file://$PWD/tools/alphabars.html --format 1080p50 --syphon=WLTest --headless
```

**Discovery, from a process that started afterwards.** `./syphon_probe --list`:

```
1 Syphon server(s):
  WLTest (WebLinked)
```

That is not the easy half. `SyphonServerBase` registers for the
announce-*request* notification from `-init`, on whichever thread called it, and
`NSDistributedNotificationCenter` delivers to that thread's run loop. Created on
the HTTP thread — which is what happens when an operator adds this output from
the control page — the server posts its opening announce and then answers
nothing. A consumer already running finds it; one started later never does.
`SyphonSurface::open` marshals to the main thread for exactly this reason, and
the probe is written to start second so the check cannot pass by accident.

**Colour and premultiplied alpha.** `--alphabars`, at 1080p50:

```
  (240,540)  opaque red   got BGRA   0   0 192 255  want   0   0 192 255  ok
  (720,540)  50% green    got BGRA   0  96   0 128  want   0  96   0 128  ok
  (1200,540) 25% blue     got BGRA  48   0   0  64  want  48   0   0  64  ok
  (1680,540) transparent  got BGRA   0   0   0   0  want   0   0   0   0  ok
  PASS
```

Exact, every channel — no 4:2:2 round trip to forgive anything, because there
isn't one. Green at 96 and blue at 48 are the *premultiplied* values: this
output does not undo Chromium's premultiply, and a table written against the
unpremultiplied ones would have "failed" a correct implementation.

**Orientation, separately.** `tools/alphabars.html` cannot test this — its bands
are vertical, so it reads identically through a vertical flip. `tools/updown.html`
(red over blue) exists only for this, via `--orientation`:

```
  (960,135) top red      got BGRA   0   0 192 255  want   0   0 192 255  ok
  (960,945) bottom blue  got BGRA 192   0   0 255  want 192   0   0 255  ok
  PASS
```

Row 0 of the surface is the top of the page, as predicted from
`SyphonMetalServer`, which blits without inverting when told its source is
unflipped. The copy is a copy.

**Arena itself.** Arena's Sources panel lists it under **SYPHON SERVERS** as
`WebLinked - WLTest`, and its own REST API agrees:

```bash
curl -s http://127.0.0.1:8080/api/v1/sources | grep -o '"name":"WebLinked - WLTest"'
```

Loaded onto layer 1 and triggered, Arena reports `connected: Connected` at
1920x1080, and — without being told — labels the clip **Alpha Type:
Premultiplied**. The Composition Monitor shows the four bands. That is the whole
claim of this output demonstrated by the application it was written for.

**What the copy costs — measured, not asserted.** Steady state, 30 s windows at
1080p50, with the surface being written every tick in all three:

| | ticks | dropped | published |
|---|---|---|---|
| Client attached, Arena idle | 1502 | 1 | 1501 |
| Client attached, Arena rendering the clip | 1502 | 49 | 1454 |

1502 ticks in 30 s is 50.07 Hz — the clock, not the consumer. The copy itself is
free to within one tick in 1502 (0.07%). The 49 dropped ticks in the second row
are **not** the memcpy: disconnecting the clip while the Syphon client stayed
attached left the copy running at full rate and drops fell to 7, so what costs
is a VJ application rendering 1080p on the same GPU, which it would do to any
source. Worth knowing before blaming this output for a heavy night.

**Nobody listening costs nothing.** Before any client attached: `frames 6788,
published 4, skipped 6784`. `hasClients` gates the copy, so an idle source is
not memcpying 8 MB fifty times a second. `published + skipped == frames`, and
the split is what tells "publishing correctly to nobody" apart from "broken".

### The two traps

**`glGetTexImage` silently returns zeros for an IOSurface-backed texture.** The
first probe read the Syphon texture that way, got four bands of `0 0 0 0`, and
reported `GL_NO_ERROR`. That is indistinguishable from a server publishing blank
frames, and it cost a round of debugging aimed at the wrong half — the preview
endpoint showed the page rendering correctly all along, which is what settled
it. `CGLTexImageIOSurface2D` textures have to be read through an FBO and
`glReadPixels`. The probe does, and says so at the function.

**`-newFrameImage` hands back an image before any frame has been published into
it.** Read that first one and every channel is zero — the same symptom again,
from a different cause. The probe waits on `-hasNewFrame` and takes several
frames rather than the first.

Neither trap is in the server. Both would have been read as server bugs by
anyone verifying with a receiver written the obvious way.

**Not verified:** Spout, and therefore Windows — the backend is not written, and
`WEBLINKED_WITH_SHARED` disables itself off macOS rather than shipping a stub
that would make `--spout` look supported. Nothing has been checked in a consumer
other than Arena, though the probe's use of the Syphon 5 framework is evidence
for anything else that links it. Nothing has run for longer than a few minutes.

---

## 24. The shared output on Windows — the backend verified, the build not

Spout is the Windows half of the shared output, and it is the first Windows code
in this repository that has actually been **run**. That is a smaller claim than
it sounds, so it is worth being precise about what was and was not established.

WebLinked itself still does not build on Windows — CEF, the engine, the control
API and the screen output remain compile-targeted and untouched. So this could
not be verified the way NDI and Syphon were, by running the application and
pointing a receiver at it. Instead `tools/spout_send_test.cpp` links the **real**
`src/outputs/shared_surface_win.cpp` against the **real** vendored Spout SDK,
hands it a `VideoFrame` exactly as the engine would, and
`tools/spout_probe.cpp` reads the result back.

Run on a Windows 11 ARM64 VM with VS 2022 Build Tools 14.44, both binaries built
**x64** — the architecture real Spout applications use, so both then ran under
the machine's x86-64 emulation.

```powershell
.\build_spout_test.ps1 -Repo C:\wl -Out C:\wl\out
.\out\spout_send_test.exe --pattern alphabars --seconds 60
.\out\spout_probe.exe --list
.\out\spout_probe.exe --source WLTest --pattern alphabars
```

**The sender opens and registers.**

```
Spout sender 'WLTest' open: Spout sender, DirectX 11 shared texture, BGRA8, CPU copy
sending alphabars at 1920x1080 for 10 s
published 500, skipped 0
```

500 published in 10 s at 50 Hz, and `skipped 0` is correct rather than a
counter that was never wired: Spout tells a *sender* nothing about receivers, so
unlike Syphon there is no attach signal to skip on. `spoutDX::IsConnected` is
the receiver's question. On Windows `published` and `frames` should therefore
track each other where on macOS they diverge whenever nothing is listening.

**Discovery**, from `--list`:

```
1 Spout sender(s):
  WLTest
```

**Colour and alpha**, at tolerance **0** — there is no 4:2:2 round trip and no
premultiply rounding here, because the harness writes the bytes literally, so
anything but an exact match is a real difference:

```
  ( 240,540) opaque red   got   0   0 192 255  want BGRA   0   0 192 255  ok
  ( 720,540) 50% green    got   0  96   0 128  want BGRA   0  96   0 128  ok
  (1200,540) 25% blue     got  48   0   0  64  want BGRA  48   0   0  64  ok
  (1680,540) transparent  got   0   0   0   0  want BGRA   0   0   0   0  ok
  alphabars: PASS (BGRA, as sent)
```

The alpha channel survives the shared texture intact — 255, 128, 64, 0 — which
is the whole point of the output. The probe checks the RGBA reading separately
and reports which matched, rather than assuming: `spoutDX` picks channel order
from the sender's format through `m_bSwapRB`, and "BGRA, as sent" is a measured
result, not the expectation restated.

**Orientation**, against `--pattern updown`:

```
  (960,135) top red      got   0   0 192 255  want BGRA   0   0 192 255  ok
  (960,945) bottom blue  got 192   0   0 255  want BGRA 192   0   0 255  ok
  orientation: PASS (BGRA, as sent)
```

Row 0 is the top of the image, matching macOS. No flip on either platform.

### How much this is worth

**Weaker evidence than the Syphon pass, deliberately stated.** `syphon_probe`
links *Resolume Arena's own* Syphon framework, so section 23 is two vendors'
implementations agreeing. There is no second Spout implementation to hand here:
this is `spoutDX`'s receive path reading `spoutDX`'s send path, from one SDK.
It proves our backend drives the sender API correctly, that the pitch is
honoured, that channel order and orientation are right and that alpha survives.
It does not independently corroborate Spout itself.

**The CMake Windows build is still unproven.** The harness compiles the backend
with a hand-written `cl` invocation, not through `CMakeLists.txt`. The
`WEBLINKED_WITH_SHARED` Windows branch added alongside it has never been
configured or run, because nothing else in the project builds on Windows to
configure it with. Two things are therefore known only by inspection: that
`d3d11`/`dxgi` are the right link libraries, and that CMake's MSVC defaults
supply `user32`/`gdi32`, which SpoutUtils needs — driving `cl` directly does
not, and that cost one link failure.

**Not verified:** any real Spout consumer. Nothing was tested against Resolume
for Windows, TouchDesigner, OBS or anything else — none is installed on that VM.
Also unverified: pacing (the harness sleeps, it does not use the engine clock,
and nothing here should be read as evidence about WebLinked's timing on
Windows), long runs, and format changes.

### One trap

**`near` is still a reserved word.** `windows.h` defines the legacy `near`/`far`
pointer keywords, so a helper called `near` fails to compile with a bewildering
`error C2062: type 'int' unexpected` pointing at a function that is obviously
fine. Renamed to `matches`. Worth knowing before writing any small helper in a
Windows translation unit.

---

## 25. The Resolume plugin — it crashed Arena, and why

The FFGL plugin in `plugin/` took Resolume down. Not while rendering: **while
scanning the plugin folder**, before anything was placed on a layer. Worth
recording in full, because the cause is not where anyone would look and the
first fix was wrong in an instructive way.

```
EXC_BAD_ACCESS (SIGSEGV) at 0x13e52b150
  getMethodNoSuper_nolock                     libobjc
  lookUpImpOrForward                          libobjc
  _objc_msgSend_uncached                      libobjc
  +[NSArray arrayWithArray:]                  CoreFoundation
  -[SyphonServerDirectory servers]            Syphon   <- Arena's, not ours
```

The crash is inside **Arena's own Syphon**, and our bundle does not appear in
the report's image list at all. Both facts are the answer.

**Resolume `dlclose`s a plugin after inspecting it.** The first version of the
plugin vendored Syphon's client sources so it could receive WebLinked's feed.
Knowing that Resolume already loads `Syphon.framework`, every Syphon class was
renamed at compile time through `-D` macros — `WLSyphonClient` and so on — and
that part worked: the built bundle contained no `_OBJC_CLASS_$_Syphon*` symbol.

It made no difference, because **the problem was never the classes**. Syphon's
sources add *categories to Foundation classes*:

```
NSArray      (SyphonServerDirectoryServerSearch)
NSDictionary (SyphonServerDirectoryPimpMyDictionary)
```

A category attaches to its target whatever the contributing image is called.
`NSArray` outlives the bundle, so unloading left a method list on it pointing
into unmapped memory, and the next `objc_msgSend` on an `NSArray` that missed
the method cache walked it. That the caller was Arena's Syphon rather than ours
is incidental — anything touching `NSArray` could have been the one to die.

**An Objective-C image that has extended somebody else's class is not safely
unloadable.** Renaming does not help. Hiding symbols does not help — the
runtime attaches categories from `__objc_catlist`, not from the symbol table.

### The fix

The plugin now contributes **no Objective-C metadata at all** and borrows the
Syphon that Resolume already has, through `NSClassFromString` and typed
`objc_msgSend` casts. No vendored client, no `@interface` (which would emit an
undefined `_OBJC_CLASS_$_` and force linking a Syphon), no `@protocol`, no
category. The built bundle has no `__objc_classlist`, `__objc_catlist` or
`__objc_protolist` section, so there is nothing for `dlclose` to leave behind.

### Verified, with a control

`plugin/tools/unload_probe.mm` reproduces the whole sequence in under a second
and with nothing to lose: load, drive `plugMain` as a scan does, unload, then
hammer Syphon 200 times. `plugin/tools/build_unload_probe.sh` also builds a
**control** — a nine-line bundle whose only content is a category on `NSArray`.

```
$ ./out/unload_probe --bundle ./out/BadCategory.bundle --control
  objc metadata sections: 1
  dlclose...
  unloaded
Syphon after unload (200 rounds):
[exit 139 — SIGSEGV]

$ ./out/unload_probe --bundle ../build/WebLinked.bundle/Contents/MacOS/WebLinked
  objc metadata sections: 0
  plugMain: found
  parameters: 3
    [0] URL
    [1] Source Name
    [2] Run
  instantiateGL: ok
  deinstantiateGL: ok
  dlclose...
  unloaded
Syphon after unload (200 rounds):
  survived
PASS
```

The control has to keep failing. A probe that passes everything proves nothing,
and this one was written after the fact — the discipline is to make it fail on
the known-bad case before believing it about the good one.

**`instantiateGL: ok` is a second check riding along**, and it is the one a
class-level harness cannot make. The SDK sets every parameter's default on a
fresh instance and deletes the instance if any set returns `FF_FAIL`, while the
base `CFFGLPlugin::SetTextParameter` is a stub returning exactly that. A plugin
declaring a text parameter without overriding it — and this one declares two —
cannot be created by any real host, while a harness driving the C++ class
directly carries on passing. Only a probe going through `plugMain` sees it.

**Not verified:** that Arena lists the plugin, draws a WebLinked source, or
behaves over a session. The probe covers load, scan, instantiate and unload —
which is what crashed — and nothing above that.

---

## 26. The plugin's process supervision — verified without Resolume

The plugin launches a WebLinked per instance and re-points it through the
control API. That is the part with consequences an operator meets at the worst
moment — a browser still running after its layer was deleted, two layers
fighting over port 7654 — so it is checked directly rather than by loading the
plugin into Arena and watching.

`plugin/tools/helper_probe.mm` links `Helper.cpp` itself, so what runs is the
shipping code rather than a restatement of it:

```
$ WEBLINKED_BINARY=…/WebLinked.app/Contents/MacOS/WebLinked ./out/helper_probe
binary: …/WebLinked.app/Contents/MacOS/WebLinked

start:
  start() succeeded                                    ok
  was given a control port                             ok
  (port 50681)
  process is alive                                     ok
  published a Syphon source under its given name       ok

second instance:
  a second helper starts alongside the first           ok
  the two got different control ports                  ok
  the first is undisturbed                             ok

re-point without restart:
  setUrl() accepted by the running helper              ok
  same process still running after setUrl()            ok

stop:
  second helper stopped                                ok
  helper stopped                                       ok
  its Syphon source retired rather than going stale    ok

PASS
```

Four things worth drawing out.

**Two instances do not collide.** Each helper asks the kernel for a free port
by binding to 0 and reading back what it chose. That is a race — something else
could take the port in the gap — and it is accepted because WebLinked refuses
to start on a port already in use and says so, which turns the race into a
clear message rather than two processes quietly sharing a control API.

**A URL edit does not restart the browser.** `setUrl` goes to `/api/url` on the
running helper, and the probe checks the process is the same one afterwards.
Restarting would cost a browser launch and a black layer on every keystroke an
operator typed into the URL box.

**The source retires rather than going stale.** `stop()` sends SIGTERM, which
WebLinked handles by shutting Chromium down in order; only after 2 s does it
escalate to SIGKILL. The probe waits for the Syphon source to disappear, which
is the observable difference between a clean shutdown and a killed one — a
killed server leaves an entry in every consumer's source list until they time
it out.

**Nothing is left behind.** `pgrep -f "MacOS/WebLinked"` reports zero after the
run. `alive()` reaps with `waitpid(WNOHANG)`, so a helper that crashed does not
become a zombie, and the plugin restarts it on the next frame instead of showing
a black layer for ever.

**A plugin scan still launches nothing.** Re-running `unload_probe` after the
supervision was added: four parameters enumerated, `instantiateGL: ok`, and
zero WebLinked processes before and after. Instantiation does not reach
`ProcessOpenGL`, and the empty-URL guard is what stops a scan starting a
browser per installed plugin.

**Not verified:** anything involving Resolume. The plugin has still never been
seen to draw. What sections 25 and 26 establish is that loading it cannot take
Arena down, that a host can create it, and that the processes it owns behave —
not that a picture arrives on a layer.

---

## 27. The plugin draws the page — verified, and Arena loads it

Two separate things, and the second is the smaller of them.

### Arena loads it

With the fixed bundle in `~/Documents/Resolume Arena/Extra Effects/`, Arena
7.27.1 starts, scans, and lists it through its own REST API:

```
WebLinked source entries: 1
   'WebLinked' | category: Video Sources | id: WL01
```

No crash. Section 25's fault is fixed in the real application, not only against
the probe. What is still **not** established is a picture on an actual Resolume
layer: driving Arena's clip grid was not completed, and the machine's owner was
using it. So the next check answers the same question a different way.

### It draws the page

`plugin/tools/render_probe.mm` drives the built bundle through `plugMain`
exactly as a host does — instantiate, set the URL, `ProcessOpenGL` into an
offscreen framebuffer — then reads the pixels back. That exercises the entire
chain in one go: the helper is spawned, WebLinked renders `tools/alphabars.html`,
publishes over Syphon, the plugin's client picks it up and the rectangle-texture
shader draws it.

```
render:
  (first picture on frame 9)
  the plugin drew something                            ok
  the picture settled to a steady state                ok
  (steady from frame 20)

alphabars through the plugin:
  ( 160,360) opaque red   got RGBA 192   0   0 255  want 192   0   0 255  ok
  ( 480,360) 50% green    got RGBA   0  96   0 128  want   0  96   0 128  ok
  ( 800,360) 25% blue     got RGBA   0   0  48  64  want   0   0  48  64  ok
  (1120,360) transparent  got RGBA   0   0   0   0  want   0   0   0   0  ok

teardown:
  deinstantiateGL returned                             ok
  no WebLinked process left behind                     ok

PASS
```

Run twice, identical both times, settling on the same frame. The alpha is
intact — 255, 128, 64, 0 — and the values match the Syphon and Spout tables in
sections 23 and 24 exactly, which is the point of using one test page
throughout: a number that disagrees says *where* in the chain it went wrong.

### Three things this cost

**A harness with no Syphon reports a working plugin as broken.** The first
version of `render_probe` linked only Foundation and OpenGL, so
`NSClassFromString(@"SyphonServerDirectory")` returned nil, the plugin had
nothing to attach to and drew nothing. That is the plugin behaving correctly —
it ships no Syphon by design (section 25) — and it means a stand-in for the
host has to link the framework. Now recorded as rule 6 in
`docs/07-resolume-plugin.md`.

**"Something arrived" is not "the page arrived".** Rendering until the picture
stopped being the clear colour sampled a blank document that WebLinked had
published before the page painted, and read black. Exactly the mistake
`syphon_probe` made with `-newFrameImage` in section 23, in a new place. The
probe now waits for the sampled pixels to hold steady for ten consecutive
frames. Steady is deliberately not the same as correct: it settles on whatever
the plugin actually produces and the assertions then judge it, because a loop
that waited until the pixels matched would be a test that cannot fail.

**A running WebLinked is six processes, not one.** `pgrep -f "MacOS/WebLinked"`
matches the browser *and* five CEF subprocesses under
`Contents/Frameworks/WebLinked Helper.app/Contents/MacOS/…`, which outlive
their parent by a moment. That made the leak check fail against a plugin
shutting down perfectly well. It now excludes `Frameworks` and polls rather
than sleeping.

**Not verified:** a picture on a Resolume layer, driven through Resolume's own
clip grid and composited by Resolume. Everything up to and including the
plugin's own draw is measured; what Arena does with the result is inferred.

---

## Bugs this verification actually found

Recorded because they are the argument for doing it at all. Every one was found
by running the thing, not by reading it.

**A browse returns one entry per interface, and picking the first gave an
unreachable address.** Not a bug in this repo but in the consumer, found the
moment rookery was pointed at a real advertisement: a multi-homed Mac answers
once per interface, so one WebLinked resolved to five rows carrying the same
`id` and different addresses. Collapsing them by `id` fixed the count and
introduced a second, quieter fault — whichever answer arrived first won, and on
this machine that was a `127/8` address. An operator could add that row, see it
poll green from the same machine, and find it unreachable from anywhere else.
The fix is to pool every address seen for an id and *then* choose, preferring
routable IPv4 over private, link-local and loopback. This is why the advertised
`id` exists at all, and it is recorded in `docs/03-control-api.md` as a rule for
anyone else consuming the record.

**The screen output's window landed off-screen on every display but the main
one, and every counter said it was working.** `-[NSWindow
initWithContentRect:styleMask:backing:defer:screen:]` interprets the rect
relative to the origin of the *screen it is given*, while `-[NSScreen frame]` is
in global coordinates. Passing one to the other applies the screen's offset
twice. The main display's origin is (0,0), so the two agree and it worked — which
is why the first pass, on a machine with one display, verified the feature
thoroughly and completely missed this.

What makes it worth recording is how convincing the failure was. `open()`
succeeded. The CVDisplayLink was created on the correct display and ticked at
that display's exact refresh rate. Metal drew. `presented` climbed at 60/s
against a 50 Hz source — the precise 1.2 ratio the design predicts, on the right
head, which reads as proof the feature works. Nothing was on any screen. It was
found only by screenshotting *both* displays and discovering the picture on
neither; every number available from inside the process said it was fine.

Fixed by creating the window with `screen:nil` and then `-setFrame:display:`,
which is unambiguously global. Section 20.

**The screen output's window, released twice, which took the process down on the
second remove/add cycle.** `NSWindow.isReleasedWhenClosed` defaults to **YES**
for a window built with `initWithContentRect:`, so `-close` already released it
and the explicit `-release` beside it was one too many. Over-releases do not
fault where they happen: the first cycle survived, the second died on the main
thread inside `objc_autoreleasePoolPop`, with a backtrace consisting entirely of
CEF and AppKit frames and nothing of ours in it. It reads exactly like a CEF
bug. Found only because the add/remove test was run twice rather than once —
a single cycle passes cleanly. Section 20.

**The profile lock, which shipped in every release up to v0.5.1.** Not found by
verification at all — found by an operator double-clicking the installed app and
getting a dialog. Worth recording for that reason: every run in this document
started one instance, from a terminal, on a machine where nothing else CEF-based
was running, and that is precisely the shape of test the bug survives. CEF had
been printing a warning naming the exact cause into a log nobody grepped for
five releases. Section 16 now covers it.

**OSC silently dropped a quarter of all messages — and this one shipped in
v0.3.0.** `readString` advanced its offset by `padded(textLength + 1)` when
`padded()` already accounted for the mandatory NUL terminator. For any string
whose length was 3 mod 4 the offset ran four bytes past the end of the packet,
the read reported failure, and the *whole message* was discarded without a word
in the log. `/weblinked/url` therefore worked for most URLs somebody tried and
did nothing at all for the rest — including
`file:///Users/…/tools/clock.html`, which is 63 characters. It survived a full
release for three compounding reasons worth remembering: it affected only a
quarter of possible strings, so casual testing hit it rarely; it failed silently,
because ignoring a malformed packet is the correct response to a malformed
packet; and the decoder lived in the target that links CEF, so nothing cheap
could reach it. That last one is why `osc_server.cpp` is now part of
`weblinked_core` and `tests/test_osc_server.cpp` exercises every length mod 4.

**A NUL byte in the control page's source killed the entire script.** A
`.join(' ')` in `web_assets.h` was written with a NUL where the space should
have been. The C++ compiled without complaint, the page served with a 200, the
HTML rendered — and every line of JavaScript after it was dead, so the page sat
on "connecting" with a black preview. It also made `grep` treat the file as
binary and silently return nothing, which sent the first ten minutes of the hunt
in the wrong direction. Found by running the page's own script text through
`new Function()` in the browser, which named the line immediately. The lesson is
the same one the earlier TDZ bug taught: **this page has no build step, so a
syntax error anywhere in it is invisible until something is asked to run.**

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

**A popup took the whole application down.** Reported from testing as "clicking
a link that opened a new tab crashed the application". CEF's default answer to
`target="_blank"` is a second browser parented to the first — and the source
browser here is windowless, so there is nothing to parent it to. The route there
was worse than the crash: the popup arrived at the same client, rebound its
browser reference, and pointed the engine's frame requests, navigation and input
at a browser nothing was reading. Section 10.

**Closing a popup quit the application.** The same class of bug on the operator
window's side. Its client called `CefQuitMessageLoop()` from `OnBeforeClose`
without checking which browser had closed, so a popup opened from the control
page took the outputs with it when dismissed. It now counts browsers and quits
on the last.

**A hidden option was not hidden.** `.check { display: flex }` outranks the
browser's own `[hidden] { display: none }` — class specificity beats the UA
stylesheet — so the settings page offered an alpha checkbox on the preview
output, where it does nothing. Found by asserting field visibility from the live
DOM rather than looking at a screenshot. Section 13.

**Changing pacing silently stopped the output.** `setPacing` set a flag that
only `requestFrame()` read, but `external_begin_frame_enabled` is part of
`CefWindowInfo` and fixed when the browser is created. Switching to the internal
timer therefore stopped us asking for frames from a browser that does not paint
on its own: no frames at all, and nothing in any log. It now rebuilds the
browser at the same URL.

## 28. The stream output — the whole pipeline, to a file instead of a server

The `stream` backend encodes the page to H.264/AAC through an `ffmpeg`
subprocess and pushes it to an RTMP or SRT server. No RTMP server was available
here, so the check points the same output at a **file path** instead: ffmpeg
takes the identical two raw inputs over the identical loopback sockets and muxes
the identical FLV — only the final `avio` write target differs. That verifies
everything WebLinked is responsible for, and nothing about RTMP itself.

```bash
./build/Release/WebLinked.app/Contents/MacOS/WebLinked \
  --url file:///tmp/page.html --format 1280x720p25 --port 7754 --headless &

curl -X POST -H 'Content-Type: application/json' \
  -d '{"kind":"stream","name":"restreamer","options":{"url":"/tmp/wl.flv"}}' \
  http://127.0.0.1:7754/api/output/add
# …25 s…
curl -X POST -d '{"name":"restreamer"}' http://127.0.0.1:7754/api/output/remove
```

The page rendered a frame counter over `#112233` and played a 440 Hz WebAudio
tone at 0.2 gain, so both streams carry something checkable rather than black
and silence.

**What came out**, by `ffprobe` on the file — not by WebLinked's own counters:

| | |
|---|---|
| Video | h264, 1280x720, `r_frame_rate` **25/1** |
| Audio | aac, 48000 Hz, 2 channels |
| Length | video 25.040 s, audio 25.067 s — **−27 ms** |
| Picture | decoded frame is the page: right text, right background |
| Sound | `volumedetect` mean −17.0 dB; DFT peak at **439 Hz** |
| Dropped | 0, over 626 frames |

−27 ms is one AAC frame of encoder priming (1024 samples = 21.3 ms) plus
rounding. It is a **constant offset, not a drift** — the deficit was 0 ms when
sampled at 5 s, 10 s and 20 s.

### The two faults this found, both of which shipped in the first version

**ffmpeg will not open its second input until the first one is flowing.** The
first version accepted both loopback connections before writing either, which
deadlocks: `lsof` showed ffmpeg connected to the video socket and blocked
probing it, having never touched the audio socket. `connected: false`,
`frames: 0`, `dropped: 171` — and no error, because nothing had failed. The fix
is two independent writer threads; video moves first and the audio it displaces
is held for the audio writer rather than dropped, so the streams still line up
however late the audio connection is made.

**A subprocess inherits every open descriptor.** This is the first child process
this application has ever had, and ffmpeg came up holding the HTTP control port
and an accepted control connection:

```
ffmpeg 16532 …  50u  TCP localhost:7754 (LISTEN)
ffmpeg 16532 … 117u  TCP localhost:7754->localhost:54749 (CLOSE_WAIT)
```

An orphaned encoder therefore keeps the control port bound, and the next launch
fails with *"port 7654 is already in use — another WebLinked is probably
running"*, naming the wrong cause entirely. `core/socket_inherit.h` now sets
`FD_CLOEXEC` (and clears `HANDLE_FLAG_INHERIT` on Windows) on the HTTP listener,
every accepted HTTP connection, the OSC socket and this backend's own listeners.
Re-checked after the fix: the child holds its own two input sockets and nothing
else.

A third, smaller one came out of the length measurement rather than a crash:
**closing the sockets before joining the writers truncated the tail**, and
**signalling ffmpeg before it had drained its input** truncated more. Together
they cost about half a second of audio against a full-length video — 547 ms,
then 392 ms once the writers were allowed to drain, then 27 ms once ffmpeg was
allowed to exit on EOF instead of being signalled. The `audio_deficit_ms` field
in `/api/state` exists because of this: one tick of video must carry exactly one
tick of audio, and that comparison is the only place a slow desync is visible.

### RTMP itself — since verified, against a real Restreamer

The paragraph that stood here said no streaming server had ever accepted a
connection from this backend. That was true for about an hour. On the same day
it was pointed at a **datarhei Core 16.0.0** (the engine inside Restreamer)
running on another machine, reached over Tailscale, driven by Atem Overseer's
browser source type:

```
WebLinked (this Mac) ──RTMP over tailnet──▶ Restreamer ──split, -c copy──▶ back to Overseer
```

Measured at the far end, not here: the Core's split process took **h264
1280x720 in at 0 drop**, and the monitor copy pulled out of Overseer's http-flv
decoded to the rendered page. This backend reported `connected: true`, 652
frames, **0 dropped**, `audio_deficit_ms: 0`.

So the RTMP handshake, publish and sustained delivery are real. What is still
untested is a commercial ingest (YouTube, Twitch) with its own handshake and
tolerances, SRT in any form, and the reconnect behaviour when a server drops
mid-stream.

**Windows and Linux remain unbuilt.** `posix_spawn` and `CreateProcessA` are
different code paths and only the former has ever run.

## 29. The mDNS advertisement — verified on macOS, against foreign tools

The claim being checked is not "a record appears" but "the record is true and
another program can act on it". Three independent checkers, none of which share
code with the responder.

**Registration and TXT, against Apple's `dns-sd`.** A process registering
exactly what `ControlApi` registers:

```
$ dns-sd -B _weblinked._tcp
12:51:33.072  Add   3   1 local.  _weblinked._tcp.  azlan-1386 (7654)

$ dns-sd -L "azlan-1386 (7654)" _weblinked._tcp
can be reached at azlan-1386.local.:7654 (interface 14)
 txtvers=1 ver=0.7.1 name=azlan-1386\ \(7654\) id=fc0e50e0 path=/api/state
 token=0 osc=1 oscport=7655 oscprefix=/weblinked
```

Every key survives the round trip, including the two that matter — `oscport`
and `oscprefix`. Withdrawal was confirmed by the same browse going quiet after
`Responder::stop()`.

**Honesty, against `tools/mdns_probe`.** The probe browses, decodes the TXT with
the platform's parser, and then connects to the advertised address:

```
$ ./mdns_probe --timeout 3        # nothing listening on the advertised port
  reached : NOTHING ANSWERS at the advertised address        (exit 1)

$ ./mdns_probe --timeout 3        # a control API answering /api/state
  reached : OK — answered /api/state                          (exit 0)
```

Both directions were forced deliberately, because a checker that has only ever
seen the passing case proves nothing about the failing one.

**End to end, against rookery.** rookery's own browse — a different language, a
different mDNS implementation (`mdns-sd`), written from the spec rather than
from this code — resolved the advertisement and read every field:

```json
{ "found_via": "mdns", "host": "10.147.17.93", "http_port": 7654,
  "id": "fc0e50e0", "name": "azlan-1386 (7654)",
  "osc_port": 7655, "osc_prefix": "/weblinked", "version": "0.7.1" }
```

**Unit tests:** 14 tests, 913 checks over the TXT encoding, the instance-name
rules and the loopback refusal. The length sweep runs 1..300 bytes because the
TXT length prefix is a single byte and 255 is the boundary an off-by-one hides
behind — the same reasoning as the OSC padding sweep in section 1.

### What this section does **not** cover

- **Windows and Linux registration is compile-only.** Neither
  `DnsServiceRegister` nor the avahi backend has been run. The Linux backend is
  built against avahi's real headers, so the signatures are compiler-checked,
  but no avahi daemon has ever seen it.
- **Nothing has been proven off-machine.** Every browse above ran on the same
  Mac as the advertisement. Multicast across a real switch, across VLANs, and
  through a venue network's IGMP snooping, is untested.
- **Conflict renaming is untested.** Two instances claiming one name should get
  one of them renamed by the responder, and `registeredName()` should report
  the new one. Not exercised.

## 30. The menu bar item — verified by using it

WebLinked now puts an `NSStatusItem` in the macOS menu bar itself, rather than
leaving that entirely to the tray launcher. `--no-tray` turns it off.

**Why this is not the operator window coming back.** Section 9 removed a CEF
*browser* window, and the crash was specific to browsers: a windowed one
defaults to Chrome runtime style while every source here is windowless and so
Alloy, which put two runtime styles in one process and segfaulted the GPU
process on every launch. A status item is AppKit and owns no browser, so it
cannot reintroduce that — the same reasoning that already lets the screen
output own a real `NSWindow`. Rule 9 is about browsers.

It also closes the second complaint section 9 recorded. That window "never had
a menu bar … and there was no way to quit it from the UI". `NSApp.mainMenu` is
still never set, but the status item's menu carries Quit with ⌘Q.

**What was checked, on this Mac, by clicking it:**

- the icon appears in the menu bar — `dot.radiowaves.left.and.right`, as a
  template image so it recolours for a light or dark bar. Photographed, not
  inferred from the log line;
- the menu opens with the live line filled in: `WebLinked 0.7.1` / `1 source` /
  Open control page / Copy control address / Reveal log in Finder / Quit;
- **the live line is genuinely live** — it is `SourceManager::size()`, read in
  `menuNeedsUpdate:` when the menu opens rather than on a timer;
- **Copy control address** put `http://127.0.0.1:7699/` on the pasteboard,
  confirmed with `pbpaste`;
- **Quit shut down in order**, which is the whole point of it existing:

```
INFO  menu bar item installed
INFO  shutdown requested from the menu bar
INFO  shutdown requested by signal
INFO  WebLinked exiting cleanly
```

  The process exited and port 7699 was released. Quit deliberately sets the same
  flag `SIGTERM` does and lets the watchdog post to the UI thread, rather than
  calling `CefQuitMessageLoop()` from inside AppKit's menu tracking — the one
  place unwinding the loop is not safe. That is why both the menu-bar line and
  the signal line appear: it is one path, not two.

- **`--no-tray` installs nothing** and keeps the Dock icon;
- **no rendering regression** from the accessory activation policy the tray
  turns on: the same build sent 60 frames to `ndi_probe` at 51.24 fps, 1920x1080
  progressive, 50/1, correct stride.

**Not verified.** The status item on a second display or with the menu bar
auto-hidden; behaviour when the menu is open as the process is killed from
outside. Windows and Linux have **no** tray at all here — `installTray` is a
stub returning false on both, deliberately, because a Shell_NotifyIcon message
window and a StatusNotifierItem D-Bus name would be real UI that has never been
run (rule 1), and the tray launcher already covers those platforms.

## 31. The tray on Windows and Linux — verified on real systems

The tray is no longer macOS-only, and neither platform was signed off on a
compile. Both were exercised on machines built for it: a Windows 11 **x86_64**
KVM guest and an Ubuntu 24.04 guest, on the Unraid host. That matters for
Windows in particular — the only Windows machine here before this was ARM64,
while every Windows artefact this project ships is x86_64, so no shipped
Windows binary had ever been run on its own architecture.

### Linux — libayatana-appindicator, verified twice

Against a purpose-built StatusNotifierWatcher **and** against KDE Plasma's own,
which is the reference implementation of the protocol:

```
org.kde.StatusNotifierWatcher          KDE's
org.kde.StatusNotifierHost-6883        plasmashell
RegisteredStatusNotifierItems:
  :1.46/org/ayatana/NotificationItem/weblinked
```

Menu read over `com.canonical.dbusmenu` — header, the live source count,
Open control page, Quit — clicking Quit by its label gave the orderly path:

```
INFO  tray item installed via libayatana-appindicator3.so.1
INFO  shutdown requested from the tray
INFO  shutdown requested by signal
INFO  WebLinked exiting cleanly
```

`--no-tray` installs nothing. A machine with no desktop degrades to
`no tray: no display available for GTK` and keeps serving.

**Not verified:** the icon's pixels on a real desktop. The KDE guest's Plasma
session runs on Xvfb with software GL and is not healthy enough for a blank
tray to mean anything — see the lab notes. Protocol conformance is proven;
appearance is not.

### Windows — Shell_NotifyIcon, verified end to end

- **115 tests, 26191 checks, 0 failures** — the suite's first run on Windows;
- **the icon appears in the notification area**, confirmed by photographing the
  tray with the process up and again with it stopped: the icon is present in
  one and absent in the other, difference bounding box `(0,0,147,37)`;
- Windows' own `HKCU\Control Panel\NotifyIconSettings` carries an entry for
  `weblinked.exe`, which is the shell recording the registration;
- **the menu opens** with the same items as the other two platforms, live source
  count included;
- **Copy control address** put `http://127.0.0.1:7699/` on the clipboard;
- **Quit shut down in order**, port released:

```
DEBUG tray: callback event 0x007b (lparam 0x1007b)   <- WM_CONTEXTMENU
INFO  shutdown requested from the tray
INFO  shutdown requested by signal
INFO  WebLinked exiting cleanly
```

**Three Windows-specific traps this found, all of which compile silently:**

1. `IDI_APPLICATION` expands through `MAKEINTRESOURCE` to the **ANSI** form
   because this project does not define `UNICODE`, so it cannot be handed to
   `LoadIconW`. Use `MAKEINTRESOURCEW(32512)`.
2. `NIM_SETVERSION`/`NOTIFYICON_VERSION_4` **changes the callback encoding**:
   the event moves to `LOWORD(lParam)` and a right-click arrives as
   `WM_CONTEXTMENU`, not `WM_RBUTTONUP`. The icon still appears and every click
   is delivered — the menu simply never opens. The log line above is what
   proved the fix.
3. `Shell_NotifyIcon`'s return value was being discarded, so the log claimed the
   icon was installed without knowing. It is checked now; that check is what
   established the icon was genuinely accepted while it was still hidden behind
   the Windows 11 chevron.

**Not verified on Windows:** behaviour when Explorer restarts. The
`TaskbarCreated` handler is written and is why the window is a real hidden
window rather than `HWND_MESSAGE`, but the re-add path has not been exercised.

## Not verified

Everything in this section is written against a real SDK header set and compiles.
Since section 19 that is no longer true of DeckLink, which has now moved real
pixels down a real cable; the rest of this section still has never done so.

**DeckLink output — verified, on two different card profiles.** Output, pre-roll,
the scheduled-playback sequence and the buffer level have been measured against a
real DeckLink Duo 2, and loopback captures confirm the colour on the wire at
1080p50 and 1080p25 — in a two-sub-device full-duplex profile (section 19) and
again in a four-sub-device half-duplex one (section 19a), with identical sampled
values. The **output path is not a compile-only claim** and has not been one
since section 19.

What remains open on DeckLink is the keyed *content*, plus **audio over SDI**
(not exercised at all) and genlock over hours rather than minutes.

**DeckLink key + fill — partly verified.** `--key` enables the card's keyer
through `IDeckLinkKeyer`, switching the output to 8-bit BGRA so the alpha
survives. Both modes **start on real hardware**, and section 19 measured the
**fill** off the wire: it carries straight, not premultiplied, colour. Two things
about the guards are now known rather than assumed:

- The `SupportsExternalKeying`/`SupportsInternalKeying` flags are *not*
  sufficient alone — they pass on a card that then refuses 8-bit BGRA at
  1080p50, because alpha roughly doubles the link rate (section 19).
- They are also *not* redundant: on a profile with no keyer they are the only
  thing that catches it, and they do (section 19a).

Still unconfirmed: that the **key** output carries alpha as luma, and the
internal-keying composite. Both need a keying-capable profile *and* a spare
input, which no cabling and profile combination has offered at the same time
yet. Section 19a also records a live defect in the 1080p50 keyed error message,
which names rates that a keyer-less profile cannot use.

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
built or run — with one exception since v0.7.0: the **Spout backend** has been
compiled and executed on a Windows 11 ARM64 VM through a standalone harness, and
its pixels checked by a receiver (section 24). That is the output alone; the
application around it, and the CMake that would build it, remain untouched.
Expect the Windows DeckLink path in particular to need work: it uses COM rather
than the dispatch shim, and that code has never seen a compiler.

**Alpha output.** `--alpha` is implemented for NDI and OMT and has not been
checked against a downstream keyer.

**Anything at 4K over a sustained period.** 2160p25 was exercised only as part of
the format-change sequence, for a few seconds.

**A runtime pacing change.** `/api/pacing` rebuilds the browser at the same URL,
because `external_begin_frame_enabled` is fixed when a browser is created and
merely flipping the flag produced no frames at all. The rebuild is implemented
and compiles; it has not been exercised on air, and it is not a mid-show
operation.

**The tray launcher end to end.** No longer a gap, because the launcher is gone:
retired in v1.0.0 in favour of the engine's own status item, which *is* verified
by using it on all three platforms (section 31).

**A projector, and three or more displays.** Two displays are now verified
(section 20); nothing has been shown on a projector.

**The screen output on Windows and Linux.** Written, compile-targeted, never
run — like AJA and OMT before them.
