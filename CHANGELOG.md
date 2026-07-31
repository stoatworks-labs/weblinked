# Changelog

## v0.3.0 — 2026-07-31

A settings page, diagnostics you can reach without a shell, a tray launcher, and
the crash that testing found.

### Added

- **A settings page.** Outputs can be added, edited, removed, started and
  stopped from the browser, with only the fields each backend actually has — an
  NDI sender gets a name and alpha, a DeckLink gets a device index and keying.
  Editing replaces an output *in place*: if the new settings cannot open, the
  previous output is restarted and the reason is shown, because renaming an
  output must not be able to leave you with none. Also raster, colour matrix,
  pacing, and what a new tab does. New `POST /api/output/update`, `/api/pacing`,
  `/api/settings/apply`.
- **Settings that persist.** `Save` writes what is actually running — never what
  was asked for, so a card that failed to open is not recorded as working — to
  `~/Library/Application Support/WebLinked/settings.json` and its equivalents.
  The next launch reads it back, and **anything given on the command line still
  wins**. Written to a temporary file and renamed, so an interrupted save costs
  the new settings rather than the old. `--settings`, `--no-settings`,
  `$WEBLINKED_SETTINGS`. See [docs/05-settings.md](docs/05-settings.md).
- **Diagnostics in the app.** The live log with levels coloured and the level
  changeable *while the fault is happening*; a crash report on demand; and the
  diagnostics bundle as a **download** rather than a path on a machine you are
  not sitting at. New `GET /api/log`, `GET /api/diagnostics/bundle`,
  `POST /api/log/level`, `POST /api/diagnostics/report`.
- **A tray launcher** in [`launcher/`](launcher/), the fleet's av-launcher shell
  configured for WebLinked: pick an interface and port, start and stop, open the
  control page, live in the menu bar. It runs WebLinked `--headless` so there is
  only ever one UI. The launcher builds and its config is unit-tested against
  the file it ships; it has **not** been clicked through against a live
  WebLinked.
- **`--popups navigate|block`** and **`--no-interactive`**.

### Changed

- **The interactive preview is on by default.** It is the only way to reach a
  page that wants a click before it shows anything, and an operator who has to
  find a toggle first usually concludes the preview is broken. Still outlined
  whenever it is armed, because it is the on-air output; `--no-interactive`
  restores the old behaviour, and the engine's answer is in `/api/state` so
  every browser opened on one instance agrees.
- The control page is three views — Control, Settings, Diagnostics — as tabs
  rather than pages, so switching costs neither the preview stream nor the
  state poll.

### Fixed

- **Clicking a link that opened a new tab took the application down.** CEF's
  answer to `target="_blank"` is a second browser parented to the first, and the
  source browser here is windowless — there is nothing to parent it to. The
  route there was worse than the crash: the popup arrived at the same client,
  rebound its browser reference, and left the engine's frame requests,
  navigation and input pointed at a browser nothing was reading. Popups are now
  always cancelled and the URL handled instead — loaded in place by default,
  or dropped. Measured by clicking both a `target="_blank"` link and a
  `window.open` through `/api/input` and watching frames keep flowing.
- **Closing a popup quit the application.** The operator window's client called
  `CefQuitMessageLoop()` from `OnBeforeClose` without checking which browser had
  closed, so a popup opened from the control page took the outputs with it. It
  now counts its browsers and quits on the last, and closes popups on shutdown
  rather than leaving one holding the message loop open after a SIGTERM.
- **Changing pacing silently stopped the output.** `setPacing` set a flag only
  `requestFrame()` read, but `external_begin_frame_enabled` is fixed when a
  browser is created — so switching to the internal timer stopped us asking for
  frames from a browser that does not paint on its own. No frames, nothing in
  any log. It now rebuilds the browser at the same URL.
- **The settings page offered an option that does nothing.** `.check { display:
  flex }` outranks the browser's own `[hidden] { display: none }`, so an alpha
  checkbox appeared on the preview output. Found by asserting field visibility
  from the live DOM rather than by looking at it.
- `matrix` and `pacing` were plain members read by the clock thread outside the
  mutex and now writable from the control surface; both are atomic.

### Verified

66 tests, 24,999 checks. The popup fix against both routes and both policies,
with frames measured after; settings saved, restarted into, and overridden by
the command line; every new endpoint exercised; the control page loaded in a
browser with no console errors and its per-backend fields asserted from the DOM.
See [docs/04-verification.md](docs/04-verification.md) sections 10–14.

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
