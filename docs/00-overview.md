# Overview

## The problem

Live events increasingly need graphics that are *already* web pages: a scoreboard
driven by a timing system, a dashboard, a countdown, a lower-third built by a
designer in HTML, a results table fed by an API. Getting one of those onto a
vision mixer usually means one of:

- **A laptop on a screen output**, captured through an HDMI-to-SDI box. Works,
  but burns a machine and a capture path, and the raster and frame rate are
  whatever the display negotiated rather than what you asked for.
- **A full media server** — CasparCG, vMix, a graphics system — where the HTML
  page is one small feature of something much larger to install, configure and
  learn.
- **A bespoke integration** written for one show and thrown away.

WebLinked is the small middle option: one binary that takes a URL and produces a
proper broadcast signal, at the raster and rate you specify, on SDI and on the
network at the same time.

## Who it is for

Someone who already knows what 1080p50 means and has a vision mixer to plug into.
The tool assumes broadcast literacy and does not try to hide it: you choose the
raster, the rate, the scan type and the colour matrix, and it tells you when a
card cannot do what you asked.

## What it is not

**Not a media server.** No playlist, no layers, no compositing, no transitions.
It renders one page. Two graphics at once is a job for the page.

**Not a capture tool.** Output only.

**Not genlocked.** The engine's clock is a software clock; an SDI card's
scheduled playback absorbs the difference. That is a real limitation, described
honestly in [01-architecture.md](01-architecture.md) along with why the boundary
is drawn where it is and which number in `/api/state` tells you it is going
wrong.

**Not a replacement for CasparCG** if you already run CasparCG. Caspar's HTML
producer does this and much more. WebLinked exists for the case where the HTML
page is the *whole* requirement and a media server is more than you want to
deploy, learn and keep running.

## Design commitments

**One page in, several destinations out, all identical.** Every output receives
the same frame from the same pipeline, so what NDI carries is what SDI carries.
The preview is itself an output, for the same reason — if the preview is right,
the outputs are right.

**Exactness where it matters.** Frame rates are exact rationals, not doubles.
Audio is sliced into each frame's exact share of samples, so 59.94 alternates
800 and 801 rather than drifting 40 ms a minute. Colour uses studio-swing
BT.601/709 with the correct matrix for the raster, verified against an
independent implementation.

**Fail visibly, and only where the failure is.** A card that will not open does
not take the NDI feed down with it; it reports its error in `/api/state` and
everything else keeps running. Formats that cannot be carried are refused rather
than silently approximated.

**Two ways in, because shows work that way.** A control page for the operator,
and OSC for the Companion button or the show-control cue. `/weblinked/script`
lets a graphic that already exposes its own functions be driven with no
integration work at all.

**Small.** One binary. No service to install, no database, no framework in the
browser, no GUI toolkit — the operator window is a CEF window showing the same
control page the HTTP server already serves.

## Where to go next

- [01-architecture.md](01-architecture.md) — the pipeline and the decisions
  behind it
- [02-building.md](02-building.md) — building, and the macOS bundle traps
- [03-control-api.md](03-control-api.md) — HTTP and OSC reference
- [04-verification.md](04-verification.md) — **what has actually been proven**
- [diagnostics.md](diagnostics.md) — when something goes wrong on site
