# Verification

What has actually been proven about this code, how, and what has not. Written so
that a claim in the README can be checked rather than believed.

The distinction that matters throughout: **compiles against a real SDK** is not
the same as **works against real hardware**. NDI and DeckLink have now been
measured against real hardware (sections 2 and 19); OMT and AJA remain in the
first category.

Everything below was run on macOS 26.4.1, Apple Silicon (M4 Max), against
CEF 150.0.17 / Chromium 150.0.7871.187.

---

## 1. Unit tests — verified

```bash
cmake --build build && ./build/tests/weblinked_tests
# 74 tests, 25063 checks, 0 failures
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

## 14. The tray launcher — partially verified

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

## 18. The tray launcher — built, not yet driven

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

With SDI out (connector 1) looped to SDI in (connector 3) — which on this card in
full-duplex are **the same sub-device**, index 0:

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

### External keying takes the second connector

With external keying enabled on index 0, the loopback goes dark; with keying off
at the identical raster and rate it captures perfectly. That is key + fill doing
what it is supposed to — connector 3 stops being an input and becomes the **key**
output beside the fill on connector 1 — but it means this cable arrangement
cannot see a keyed signal. Inferred from the controlled comparison rather than
measured directly.

**Still not verified, and now for concrete reasons:**

- **The content of a keyed signal.** Proving fill carries straight (not
  premultiplied) colour and key carries alpha needs a capture on a *different*
  sub-device, i.e. a cable from connector 1 to connector 2 or 4. The equivalent
  bug was real on NDI (section 7), so this is worth doing.
- **Internal keying compositing over a real input.** Overlay keys over whatever
  the card is receiving; looping our own output into it is a feedback path, not a
  test. It needs an external source.
- **Audio over SDI.** Not exercised at all.
- **Genlock over hours**, rather than two minutes.
- **AJA.** Still no card. Unchanged from "compiles and nothing more".

---

## Bugs this verification actually found

Recorded because they are the argument for doing it at all. Every one was found
by running the thing, not by reading it.

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

## Not verified

Everything in this section is written against a real SDK header set and compiles.
Since section 19 that is no longer true of DeckLink, which has now moved real
pixels down a real cable; the rest of this section still has never done so.

**DeckLink — now largely verified; see section 19.** Output, pre-roll, the
scheduled-playback sequence and the buffer level have all been measured against a
real DeckLink Duo 2, and a loopback capture confirms the colour on the wire. What
is still open on DeckLink is narrower than it was: the **content** of a keyed
signal (that fill carries straight colour and key carries alpha, which needs a
capture on a different sub-device), internal keying over a genuine external
input, **audio over SDI**, and genlock behaviour over hours rather than minutes.

**DeckLink key + fill.** `--key` enables the card's keyer through
`IDeckLinkKeyer`, switching the output to 8-bit BGRA so the alpha survives. Both
modes now **start on real hardware** — but only where the card can carry RGBA:
a Duo 2 refuses 8-bit BGRA at 1080p50 while accepting it at 1080p25, 1080i50 and
720p50, because alpha roughly doubles the link rate. The
`SupportsExternalKeying`/`SupportsInternalKeying` guard passes on such a card and
is therefore *not* sufficient on its own; the failure surfaces one layer down, at
the pixel format. That no card has yet confirmed fill and key appearing on
separate connectors remains true.

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

**A runtime pacing change.** `/api/pacing` rebuilds the browser at the same URL,
because `external_begin_frame_enabled` is fixed when a browser is created and
merely flipping the flag produced no frames at all. The rebuild is implemented
and compiles; it has not been exercised on air, and it is not a mid-show
operation.

**The tray launcher end to end.** See section 14.
