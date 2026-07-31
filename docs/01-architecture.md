# Architecture

The pipeline, and the decisions that shaped it. Read
[04-verification.md](04-verification.md) alongside this: it records which of
these claims have been measured.

## The pipeline in one line

```
URL ──► CEF offscreen browser ──► frame slot ──► clock thread ──► outputs
              │                                        │
              └──► audio FIFO ─────────────────────────┘
```

One browser, one clock, N outputs. Everything else is detail.

## The five decisions worth knowing

### 1. Our clock drives the browser, not the other way round

CEF offers two ways to get frames out of an offscreen browser:

- `windowless_frame_rate` — Chromium paints on its own timer, up to 60 Hz.
- `external_begin_frame_enabled` — Chromium paints only when asked, via
  `SendExternalBeginFrame()`.

We use the second. Chromium's timer has no relationship to a broadcast frame
rate, so at 50p you get an irregular mixture of repeated and dropped paints —
fine for a screen recording, not fine for a vision mixer. One begin-frame request
per tick gives one paint per tick; measured at 1,610 paints across 1,631 ticks,
the difference being startup.

`--pacing internal` switches back, because external begin frame is the
less-travelled path in CEF and an operator should not be stuck if a page
misbehaves under it.

### 2. Frame rates are exact rationals, never doubles

`FrameRate` is `{numerator, denominator}`. 59.94 is `60000/1001`, and it stays
that way from the command line through to the NDI frame header.

Tick deadlines are computed from tick zero — `deadline(n) = n * den / num` — not
by accumulating a period. Accumulation drifts by the rounding error times the
tick count: with a microsecond period at 59.94 that is about four frames an hour.

The same reasoning drives audio. One frame's share at 59.94 is 800.8 samples, so
`audioFramesForTick` takes the difference of two exact positions and naturally
alternates 800/801. Measured mean: 800.8020.

### 3. Pixel conversion happens once, not once per output

Outputs declare a preferred `PixelFormat`. The engine converts BGRA→UYVY at most
once per tick, and only if some enabled output actually asked for it. Four
outputs wanting UYVY cost one conversion.

BGRA is what Chromium paints and what NDI and OMT carry when alpha is wanted;
UYVY is what SDI carries natively and what the IP protocols compress. Handing a
DeckLink BGRA would make its driver convert on the CPU behind our back.

The colour matrix defaults to `auto`: BT.601 below 720 lines, BT.709 at and
above — which is what NDI, OMT and the DeckLink driver each assume when told
nothing.

### 4. The frame slot holds one frame, and `peek` beats `take`

`LatestFrameSlot` keeps only the newest paint. A queue would be wrong: if the
browser paints twice between ticks, the older paint is stale by definition and
showing it adds a frame of latency for nothing.

The clock thread *peeks* rather than takes, so a page that has not repainted —
a static graphic, most of the time — still produces a frame every tick. Repeats
are counted, not hidden; `repeated_frames` in `/api/state` is how you tell a
static page from a stalled one.

### 5. The engine's clock is the master; SDI cards absorb the difference

Nothing here is genlocked. The engine's clock is a software clock. A DeckLink or
AJA card has its own clock, and its scheduled-playback ring absorbs the
difference between them.

This is a real trade-off, stated plainly: if the two clocks disagree
persistently, the card's buffer drifts in one direction and eventually
underflows or overflows. `buffered_frames` (DeckLink) and `buffer_level` (AJA) in
`/api/state` are the numbers that show it happening. A steady value means the
clocks agree.

The alternative — making the card's frame-completion callback the master clock —
is the right answer for a single-card installation and the wrong one for a
machine feeding SDI and NDI at once, because then the IP outputs inherit the
card's timing and there is nothing sensible to do when there are two cards. That
is why the boundary is where it is.

## Threads

| Thread | Owns | Notes |
|---|---|---|
| Main | CEF message loop | `CefRunMessageLoop`; must be the main thread on macOS |
| CEF UI | `OnPaint`, browser lifetime | Publishes into the frame slot |
| CEF audio | `OnAudioStreamPacket` | Writes into the audio FIFO |
| Clock | Pacing, conversion, `submit()` | The only thread that touches an output |
| HTTP × N | Control requests | Thread per connection |
| OSC | Control messages | One receive loop |

Two synchronisation points and no more: the frame slot and the audio FIFO.

**The clock thread can be parked.** A raster change has to rebuild the frame
pools, the black frame and the scratch frame — all of which the clock thread
reads outside the output mutex. Rather than make each of those individually safe,
`pauseClock()` asks the clock thread to stand still at the top of its loop and
waits for the acknowledgement. This exists because the alternative caused a real
crash; see [04-verification.md](04-verification.md).

## Why there is no GUI toolkit

CEF is already a dependency, so the operator window is a normal windowed CEF
browser pointed at the control page the HTTP server already serves. One UI, two
ways in — the app window and any browser on the network — and no Qt, no JUCE, no
platform windowing code.

The preview it shows is modelled as an *output*, so it travels the same frame
path as SDI and NDI. If the preview is wrong, the outputs are wrong. That is the
property you want from a confidence monitor, and it is why the preview is not
simply tapped off the browser.

The same page carries the settings and diagnostics views, as tabs rather than
separate pages: switching must not cost the preview stream or the state poll,
and the operator window loads the page exactly once.

For a machine where WebLinked should come up at login and sit in the menu bar
instead of a terminal, [`launcher/`](../launcher/) wraps it in the fleet's
av-launcher tray shell, running it `--headless` so there is only ever one UI.

## A windowless browser cannot own a popup

Chromium's answer to `target="_blank"` and `window.open` is to create a second
browser parented to the first. There is no window here to parent it to: the
source browser is windowless, painting into a frame slot.

Left to CEF's default this took the whole application down, and the route was
worse than a crash on the way there. The popup arrived at the same client, whose
render handler answers for the offscreen raster; `OnAfterCreated` rebound the
client's browser reference to it, so the engine's frame requests, navigation and
input all went to a browser nothing was reading; and when that popup closed,
`OnBeforeClose` cleared the reference and left the programme output pointed at
nothing.

So `OnBeforePopup` always cancels, and the URL the popup wanted is handled
instead — loaded in the same browser (`--popups navigate`, the default) or
dropped (`--popups block`). There is deliberately no third option. The operator
window's client, which *is* windowed and may legitimately open one, counts its
browsers instead and only quits the message loop when the last has gone; before
that, closing a popup quit the application and took the outputs with it.

Found by clicking an ordinary link in the interactive preview.

## Why three of the four SDKs are loaded at run time

- **NDI** may not be redistributed. Resolving `NDIlib_send_create` and friends by
  name at run time means one binary works with the SDK, with the redistributable
  runtime, or with neither — in the last case the output reports itself
  unavailable and everything else keeps working.
- **OMT** publishes binaries for Windows and macOS arm64 only. Loading late keeps
  "no library on this platform" a deployment problem rather than a build failure.
- **DeckLink** reaches the driver through the SDK's own `DeckLinkAPIDispatch.cpp`,
  which dlopens the installed framework. A machine with no Desktop Video runs the
  binary fine and finds no devices.
- **AJA** is the exception: libajantv2 is MIT and statically linked, which is how
  AJA intends it to be consumed. So the AJA backend cannot soft-fail, and is off
  by default.

## Layout

```
src/
  core/       formats, frames, pools, clock, FIFO, JSON, dlopen  ─ no CEF
  diag/       logging, crash reports, diagnostics bundles        ─ no CEF
  browser/    CefApp, CefClient, the offscreen source
  engine/     the clock loop and everything it owns
  outputs/    the IOutput interface and the five backends
  control/    HTTP server, OSC receiver, control page, API
  app/        entry points, Info.plists, entitlements
tools/        ndi_probe — an independent receiver, for verification
tests/        unit tests; link core only, so they build in seconds
```

`weblinked_core` deliberately does not depend on CEF, so the test suite builds
and runs in a couple of seconds rather than linking 200 MB of Chromium.
