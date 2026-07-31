# Control API

Two protocols, the same verbs. HTTP is for the control page and for scripting;
OSC is for a Companion button or a show-control cue. Neither is a superset of the
other in practice, and a graphic that can only be changed by someone with a
browser open is no use in a running show.

## Security

The HTTP server binds to `127.0.0.1:7654` with **no authentication by default**.

If you bind it to a network interface (`--bind 0.0.0.0`), set `--token`. Anyone
who can reach the port can change what is on air. There is no TLS: put it behind
a reverse proxy or keep it on a trusted show network.

With a token set, every request must carry `?token=<secret>` or
`Authorization: Bearer <secret>`.

OSC has no authentication at all — that is the protocol, not a choice made here.
It listens on `0.0.0.0:7655` by default; `--no-osc` disables it.

---

## Which source a request means

WebLinked can run several independent pipelines in one process — see `--config`
in `--help` and section 15 of [04-verification.md](04-verification.md). Every
per-source verb below therefore takes an optional selector:

```
?source=<id>
```

**Leave it off and the request goes to the primary source**, which is the first
one configured and, for any command-line launch, the only one there is. That is
what keeps every v0.3.0 client working unchanged: a Companion config, a curl in
somebody's runbook, or the shipped control page never has to know this exists.

An unknown id is a `404`, not a silent no-op. Naming the wrong feed and getting
an error back is cheaper than naming the wrong feed and changing it.

The selector is a query parameter rather than a body field because `/api/state`
and `/api/preview` are GETs with no body, and one mechanism that works
everywhere beats two that each work half the time.

The process-wide endpoints — `/api/log`, `/api/diagnostics*`,
`/api/settings/save` and `/api/sources*` — ignore it.

### The collection

| Endpoint | Does |
|---|---|
| `GET /api/sources` | `{"primary": "<id>", "sources": [ ...state... ]}` — every pipeline, each entry the same shape `/api/state` returns |
| `POST /api/sources/add` | `{"source": { ...source config... }}`. `409` on a duplicate id, `400` on an invalid one. A preview output is added if the config has none |
| `POST /api/sources/remove` | `{"id": "..."}`. `409` if it is the only source: a process with no sources has no primary, so removing the last one would make every later request answer 503. Stopping WebLinked is what closing the window is for |
| `POST /api/sources/apply` | `{"sources": [ ... ]}` — reconciles the whole set at once. Sources that have gone are stopped, new ones started, and the rest updated in place so an unchanged output keeps running |

```bash
# What is running
curl http://127.0.0.1:7654/api/sources

# Retarget one feed without touching the others
curl -X POST -d '{"url":"https://example.com/next"}' \
  'http://127.0.0.1:7654/api/url?source=lower-third'

# Start another pipeline mid-show
curl -X POST -H 'Content-Type: application/json' \
  -d '{"source":{"id":"clock","url":"file:///tmp/clock.html","format":"1080p50","outputs":[{"kind":"ndi","name":"Clock"}]}}' \
  http://127.0.0.1:7654/api/sources/add
```

## HTTP

Base: `http://127.0.0.1:7654`

### GET `/`

The control page. The operator window loads this same URL, so a browser on the
same network is a remote panel.

### GET `/api/state`

Everything, as one JSON object. The shape:

```json
{
  "version": "0.1.0",
  "running": true,
  "format": "1920x1080p50",
  "format_detail": { "width": 1920, "height": 1080,
                     "rate_numerator": 50, "rate_denominator": 1,
                     "interlaced": false },
  "outputs": [
    { "kind": "ndi", "name": "Graphic", "running": true, "enabled": true,
      "device_index": 0, "options": { "alpha": true },
      "pixel_format": "UYVY", "frames": 10464, "audio_frames": 10460,
      "receivers": 1, "library": "/Library/NDI SDK for Apple/lib/macOS/libndi.dylib",
      "sdk_version": "NDI SDK APPLE ... 6.3.2.0" }
  ],
  "compiled_backends": ["preview", "ndi", "omt", "decklink"],
  "source": { "url": "...", "loaded_url": "...", "loading": false,
              "paints": 10440, "audio_packets": 20880, "console_errors": 0,
              "audio_muted": false, "pacing": "external",
              "popups": 0, "popup_policy": "navigate" },
  "settings": { "matrix": "auto", "interactive_by_default": true,
                "audio_enabled": true },
  "pacing": { "ticks": 10463, "repeated_frames": 23, "dropped_ticks": 0,
              "frames_published": 10440, "last_lateness_us": 3566 },
  "audio": { "channels": 2, "sample_rate": 48000, "buffered_frames": 480,
             "underruns": 0, "overruns": 0 }
}
```

Object key order is stable, so two responses can be diffed by eye mid-show.

**The numbers worth watching:**

| Field | Means |
|---|---|
| `pacing.dropped_ticks` | The clock fell more than a frame behind. Should stay 0. |
| `pacing.repeated_frames` | Ticks that reused the previous paint. High is normal for a static graphic; rising on an animated one means the page is too slow. |
| `pacing.last_lateness_us` | How late the last tick fired. A few ms is scheduler noise. |
| `audio.underruns` | The page did not supply enough audio. A few at startup are normal. |
| `outputs[].buffered_frames` | DeckLink only. **Steady = our clock and the card's agree.** Drifting either way ends in a glitch. |
| `outputs[].buffer_level` | The AJA equivalent. |
| `outputs[].receivers` | How many receivers are connected (NDI/OMT). |
| `source.popups` | How many times the page has tried to open a new tab or window. A page doing this repeatedly is usually about to behave oddly on air. |

`outputs[].device_index` and `outputs[].options` are the spec **as configured**,
not what the backend is doing — that is the rest of the object. The settings
page populates its editors from them.

### GET `/api/preview`

The latest frame, downscaled, as raw **BGRA** bytes. Dimensions travel in
headers so the body is a bare pixel buffer:

```
X-Frame-Width: 480
X-Frame-Height: 270
X-Frame-Sequence: 10440
```

No JPEG encoder and no WebSocket — the page polls this a few times a second and
blits it into a canvas. 404 if no preview output is configured (`--no-preview`),
503 before the first frame.

### GET `/api/diagnostics`

Writes a diagnostics bundle and returns its path. Deliberately a GET: *"open this
link and send me the file it names"* is one instruction, and works from a phone.

```json
{ "bundle": "~/Library/Logs/WebLinked/WebLinked-diagnostics-20260730-131440.json",
  "log": "~/Library/Logs/WebLinked/WebLinked.log",
  "log_directory": "~/Library/Logs/WebLinked" }
```

### GET `/api/diagnostics/bundle`

The same bundle, but as the file rather than a path to it, with a
`Content-Disposition` so a browser downloads it. This is the one to use when the
machine with the fault is in a rack and you are not sitting at it.

### GET `/api/log`

The recent log, from the in-memory ring rather than the file — so it survives a
rotation, and nothing races the logging thread.

```
GET /api/log?lines=400
```

```json
{ "level": "info",
  "path": "~/Library/Logs/WebLinked/WebLinked.log",
  "directory": "~/Library/Logs/WebLinked",
  "lines": ["2026-07-31T04:36:18Z INFO  control: HTTP listening on 127.0.0.1:7654"] }
```

`level` is lower case and unpadded here, unlike in the log file itself.

### GET `/api/settings`

What would be saved, and where.

```json
{ "path": "~/Library/Application Support/WebLinked/settings.json",
  "saved": true,
  "source": { "id": "main", "url": "...", "format": "1920x1080p50",
              "audio": true, "matrix": "auto", "pacing": "external",
              "interactive": true, "popups": "navigate",
              "outputs": [ { "kind": "ndi", "name": "Graphic" } ] } }
```

`source` is the same shape a settings file holds, so what the API returns can be
pasted into one.

### POST endpoints

All take a JSON body and return `{"ok":true}` or `{"error":"..."}`.

| Endpoint | Body | Notes |
|---|---|---|
| `/api/url` | `{"url": "https://..."}` | Navigates |
| `/api/reload` | `{"ignore_cache": true}` | `ignore_cache` bypasses the HTTP cache — what you want after a designer re-uploads a graphic |
| `/api/script` | `{"script": "showLowerThird('Anna')"}` | Runs JavaScript in the page |
| `/api/mute` | `{"muted": true}` | Mutes at the source |
| `/api/format` | `{"format": "1080p50"}` | Restarts every output |
| `/api/output` | `{"name": "Graphic", "enabled": false}` | Disabling stops the device and frees it for another application |
| `/api/output/add` | `{"kind":"ndi","name":"Second","options":{"alpha":true}}` | |
| `/api/output/remove` | `{"name": "Second"}` | |
| `/api/output/update` | `{"name":"Second","output":{"kind":"ndi","name":"Renamed"}}` | Replaces an output in place, keeping its position. On failure the previous one is restarted — see below |
| `/api/pacing` | `{"pacing": "internal"}` | Rebuilds the browser at the same URL |
| `/api/settings/apply` | `{"source": { ... }}` | Applies a whole source configuration; see below |
| `/api/settings/save` | `{}` | Writes the live configuration to the settings file. Returns `{"ok":true,"path":"..."}` |
| `/api/settings/reload` | `{}` | Reads the settings file and applies it |
| `/api/log/level` | `{"level": "debug"}` | Returns the level actually in force |
| `/api/diagnostics/report` | `{"reason": "..."}` | Writes a crash report without a crash. Returns its path |
| `/api/input` | see below | Pointer and keyboard input to the page |

### `/api/output/update` and `/api/settings/apply`

Both exist so the settings page never has to remove something before it can
change it.

`/api/output/update` stops the old output, opens the new one, and — if that
fails — restarts the old. Without it, renaming an output whose card is already
claimed would leave an operator with no output at all, having asked only to
change a label. A rejected update answers 409 with the reason.

`/api/settings/apply` **reconciles**: an output whose settings have not actually
changed is left running, so saving a change to the NDI name does not interrupt
the SDI feed beside it. Everything that can be applied is, and the first thing
that could not is reported as 409 — a settings file that half-applies and says
so honestly beats one that refuses wholesale because a card is missing.

The `source` body is validated before anything is touched: an unparseable
format or a duplicate output name is a 400 and changes nothing.

### `/api/input`

Makes the page interactive. Positions are **normalised** (0..1 across the
raster), because the control page's preview is a downscaled canvas whose size
nothing outside the browser knows; the engine scales to the current raster, so a
format change mid-drag cannot send a click off the edge.

```jsonc
{"type": "move",  "nx": 0.5, "ny": 0.5}
{"type": "down",  "nx": 0.5, "ny": 0.5, "button": 0, "clicks": 1}
{"type": "up",    "nx": 0.5, "ny": 0.5, "button": 0}
{"type": "wheel", "nx": 0.5, "ny": 0.5, "dx": 0, "dy": -240}
{"type": "key",   "action": "down", "key_code": 87, "character": 87}
{"type": "focus", "focused": true}
```

Batch several as `{"events": [ ... ]}` — the control page does this for pointer
moves, so a drag is one request rather than sixty.

`modifiers` is a bitmask of CEF event flags: shift `1<<1`, control `1<<2`,
alt `1<<3`, command `1<<7`.

Two things that are easy to get wrong:

- **An offscreen browser has no focus until you give it some.** Send
  `{"type":"focus","focused":true}` before any keyboard input or it goes nowhere.
- **Typing needs three events per character**, and the `character` must be on the
  keydown as well as the char event. With only a virtual-key code Chromium
  cannot tell which key was pressed and the page sees `e.key` as
  `"Unidentified"` — so a graphic listening for a specific key never fires:

  ```jsonc
  {"type":"key","action":"down","key_code":87,"character":87}
  {"type":"key","action":"char","key_code":87,"character":87}
  {"type":"key","action":"up",  "key_code":87,"character":87}
  ```

Status codes: 400 for a body that cannot be parsed or a format that cannot be
understood, 401 for a bad token, 404 for an unknown output or endpoint, 409 when
the request was valid but the device refused — for example a format the card does
not support. A 409 from `/api/format` means the format *did* change but an output
could not reopen at it; check `outputs[].error` in `/api/state`.

Formats accept broadcast shorthand (`1080p50`, `720p59.94`, `1080i25`,
`2160p30`) or an explicit raster (`1920x1080p50`, `3840x600p60`). Odd widths are
rejected — 4:2:2 needs pixel pairs, and silently dropping a column would be
worse than refusing.

---

## OSC

Default `0.0.0.0:7655`. Address prefix `/weblinked` (fixed at
`ControlApi::Config::oscPrefix`, so several instances can share a network).

| Address | Arguments | Notes |
|---|---|---|
| `/weblinked/url` | `s` | Navigate |
| `/weblinked/reload` | none or `i` | No argument = plain reload; `1` = bypass cache |
| `/weblinked/script` | `s` | Run JavaScript |
| `/weblinked/mute` | `i` | Non-zero mutes |
| `/weblinked/format` | `s` | e.g. `1080p50` |
| `/weblinked/output/<name>` | `i` | `1` starts, `0` stops |

### Addressing one source

Every verb above also exists under a per-source prefix:

```
/weblinked/source/<id>/url        "https://example.com/next"
/weblinked/source/<id>/reload     1
/weblinked/source/<id>/output/<name>  0
```

A bare `/weblinked/<verb>` goes to the primary source, exactly as it always did.
An unknown id logs `no source called '<id>'` and changes nothing.

The id lives in the address rather than in an argument on purpose: Companion
sends a fixed address per button, so one button binds to one feed and *stays*
bound — which is how an operator expects a physical button to behave.

Implements what a control surface actually sends: address pattern, type tag
string, `i`/`f`/`s`/`T`/`F` arguments, and `#bundle` unwrapping. Bundle timetags
are ignored and contents dispatched immediately — a desk that wanted them later
would not have sent them now. No wildcard pattern matching.

`/weblinked/script` is the one that repays attention. A graphic that already
defines its own functions can be driven from a Companion button with no
integration work:

```
/weblinked/script  "setScore('home', 3)"
/weblinked/script  "document.querySelector('#lower-third').classList.add('in')"
```

### Companion

Use the generic OSC module. Host = the WebLinked machine, port 7655.

A useful pair of buttons for a lower-third:

```
Button 1:  /weblinked/script  s  "lowerThird.show('Anna Kowalski','Head of Sound')"
Button 2:  /weblinked/script  s  "lowerThird.hide()"
```

There is no feedback path — WebLinked does not send OSC back. Poll `/api/state`
over HTTP for tally-style feedback.
