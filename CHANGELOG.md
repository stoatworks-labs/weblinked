# Changelog

## v0.2.0 — 2026-07-31

Alpha output, an interactive preview, and the fixes that came out of testing the
operator window for the first time.

### Added

- **DeckLink key + fill.** `--key[=external|internal]` drives the card's keyer
  through `IDeckLinkKeyer`, switching the output to 8-bit BGRA so the alpha
  survives. Checks `SupportsExternalKeying`/`SupportsInternalKeying` first, so an
  unsupported card gets a message rather than a bare failure, and pre-rolls
  transparent rather than opaque black — which would otherwise punch a black hole
  through whatever is behind the key for the first few frames.
- **An interactive preview.** The control page can forward clicks, scrolling and
  typing to the live page — enough to dismiss a cookie banner, close a modal or
  sign in on a machine whose browser is otherwise unreachable. Off by default and
  outlined when armed, because it is the on-air output. New `POST /api/input`.
- **`tools/clock.html`**, a time-of-day clock, and **`tools/screenshots.sh`** to
  regenerate the README images.
- `SourceConfig` / `AppConfig`: a pure-data, unit-tested description of a
  pipeline. Groundwork for running several sources in one process; nothing
  constructs it yet.

### Fixed

- **Alpha was carried wrongly.** Chromium composites premultiplied and NDI, OMT
  and a DeckLink keyer all expect straight alpha, so a 50%-opaque green went out
  at Y=95 where it should be 173. Every partially transparent pixel was too dark
  — soft edges, drop shadows and fades all rendered muddy — and the error is
  invisible on fully opaque graphics, which is why it survived. Measured over NDI
  against a real receiver, before and after.
- **macOS needs an `NSApplication` subclass implementing `CefAppProtocol`.**
  Without it the process died with an uncaught `NSInvalidArgumentException` on
  `-[NSApplication isHandlingSendEvent]` as soon as a real window pumped events.
  Invisible to `--headless`, which is all that had ever been tested.
- **Shutdown left the process alive.** `CefRunMessageLoop()` does not return
  while a window is open, and `CefShutdown()` blocks while any `CefRefPtr` is
  still held. Both presented as a process that logged a clean exit and then sat
  there holding the control port and the NDI source name.
- **Chromium raised a macOS Keychain dialog on every launch**, asking for a
  password-store key. Unacceptable on a machine that is live to air.
- **The control page's script threw on load** — the interaction listeners
  referenced the preview canvas before its declaration, so nothing on the page
  worked and the preview stayed black. Present in v0.1.0's final commits.
- **The preview stopped entirely when `document.hidden` was true.** That covers a
  kiosk shell or an embedded webview, not just a background tab, so the
  confidence monitor could show black while somebody was looking at it. It now
  polls slowly instead of stopping.

### Verified

59 tests, 24,964 checks. NDI end to end including alpha and audio cadence at
59.94; clean shutdown with and without the window; the interactive preview
measured by watching the output rather than the API's return value. See
[docs/04-verification.md](docs/04-verification.md).

## v0.1.0 — 2026-07-30

First release. URL to SDI and video-over-IP: CEF offscreen rendering, clock-driven
frame pacing, NDI / OMT / DeckLink / AJA backends, HTTP and OSC control.

Only the NDI path was verified end to end. DeckLink, AJA and OMT compiled against
real SDK headers but had never touched hardware.
