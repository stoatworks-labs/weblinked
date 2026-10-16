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
| `POST /api/sources/remove` | `{"id": "..."}`. `409` if it is the only source: a process with no sources has no primary, so removing the last one would make every later request answer 503. Stopping WebLinked is what closing the window is for. Waits for the requests already inside that source to finish, for up to 40 s; meanwhile requests to it get a 503 ("being removed"). If one has not finished by then — stuck in an output's driver — the removal answers `409` and the source carries on as it was. A second removal of a source already being removed answers `409` saying so |
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

The control page — the whole UI. A browser on the same network is a remote panel.

### GET `/api/state`

Everything, as one JSON object. The shape:

```json
{
  "version": "0.1.0",
  "running": true,
  "stale": false,
  "format": "1920x1080p50",
  "format_detail": { "width": 1920, "height": 1080,
                     "rate_numerator": 50, "rate_denominator": 1,
                     "interlaced": false },
  "outputs": [
    { "kind": "ndi", "name": "Graphic", "running": true, "enabled": true,
      "stale": false,
      "device_index": 0, "options": { "alpha": true },
      "background": "transparent", "background_colour": "#00b140",
      "pixel_format": "UYVY", "frames": 10464, "audio_frames": 10460,
      "receivers": 1, "library": "/Library/NDI SDK for Apple/lib/macOS/libndi.dylib",
      "sdk_version": "NDI SDK APPLE ... 6.3.2.0" },
    { "kind": "decklink", "name": "decklink0", "running": true, "enabled": true,
      "stale": false, "device": "UltraStudio Mini Replay 12G", "device_index": 0,
      "frames": 10467, "frames_completed": 10458, "frames_late": 0,
      "frames_dropped": 0, "frames_flushed": 0, "buffered_frames": 6,
      "ring_size": 10, "ring_free": 3, "ring_misses": 0,
      "reanchors": 0, "reanchored_frames": 0, "keying": "external",
      "premultiplied_fill": true, "genlocked": false },
    { "kind": "screen", "name": "Wall", "running": true, "enabled": true,
      "device_index": 0, "options": { "display": 1, "scaling": "fit" },
      "display": 1, "scaling": "fit", "frames": 10464,
      "presented": 12557, "dropped": 0, "renderer": "Metal, Apple M4 Max" }
  ],
  "compiled_backends": ["preview", "ndi", "omt", "decklink", "screen"],
  "displays": [
    { "index": 0, "name": "Built-in Retina Display", "width": 4112,
      "height": 2658, "refresh_hz": 50, "primary": true }
  ],
  "source": { "url": "...", "loaded_url": "...", "loading": false,
              "paints": 10440, "audio_packets": 20880, "console_errors": 0,
              "audio_muted": false, "pacing": "external",
              "popups": 0, "popup_policy": "navigate" },
  "settings": { "matrix": "auto", "interactive_by_default": true,
                "audio_enabled": true },
  "pacing": { "ticks": 10463, "repeated_frames": 23, "dropped_ticks": 0,
              "frames_published": 10440, "last_lateness_us": 3566,
              "last_tick_age_ms": 7, "output_skips": 0 },
  "audio": { "channels": 2, "sample_rate": 48000, "buffered_frames": 480,
             "underruns": 0, "overruns": 0 }
}
```

`displays` is present only in a build with the screen backend, and is
re-enumerated on every read — a monitor plugged in after start-up is exactly
when someone goes looking for it. `refresh_hz` is 0 where the platform will not
say (built-in Apple panels sometimes, and X11 always, since the Linux backend
does not link RandR). Index 0 is the main display.

Object key order is stable, so two responses can be diffed by eye mid-show.

**The numbers worth watching:**

| Field | Means |
|---|---|
| `pacing.dropped_ticks` | The clock fell more than a frame behind. Should stay 0. |
| `pacing.repeated_frames` | Ticks that reused the previous paint. High is normal for a static graphic; rising on an animated one means the page is too slow. |
| `pacing.last_lateness_us` | How late the last tick fired. A few ms is scheduler noise. |
| `pacing.last_tick_age_ms` | How long since the clock thread started a tick. About a frame period while it runs. **Climbing past a second while `/api/state` still answers means the clock is stuck**, almost always inside an output's driver. This is what a supervisor should watch; see below. |
| `pacing.output_skips` | Submits skipped because an output's gate was busy. A handful around output changes is normal; a steady climb is an output that is not answering. |
| `audio.underruns` | The page did not supply enough audio. A few at startup are normal. |
| `outputs[].stale` | `true` when a call into the output's driver has been running for over a second: the rest of the object is the last status it gave, and `driver_busy_ms` says how long the call has been running. An output busy for less than that (a card opening) reports its last figures with `stale` false. See below. |
| `outputs[].buffered_frames` | DeckLink only. **Steady = our clock and the card's agree.** Drifting either way ends in a glitch. |
| `outputs[].frames_late`, `frames_dropped` | DeckLink only, as the card reports them in its completions: a frame shown after its time, and one never shown because a less late one was ready. Both should stay 0. |
| `outputs[].reanchors`, `reanchored_frames` | DeckLink only. How many times the card's playhead overtook the schedule (a stall longer than the queue) and the output skipped forward to stay `preroll` frames ahead, and how many frame slots that skipped. |
| `outputs[].ring_misses` | DeckLink only. Ticks dropped because the card still had every frame in the ring. Should stay 0; if it doesn't, raise the `ring` option (default twice the preroll plus four). |
| `outputs[].buffer_level` | The AJA equivalent. |
| `outputs[].receivers` | How many receivers are connected (NDI/OMT). |
| `outputs[].presented` | Screen only. Frames put on the glass. Paced by the **display**, not by `--format`, so this is expected to differ from `frames`: a 50 Hz page on a 60 Hz head repeats, and on a 30 Hz head drops. |
| `outputs[].dropped` | Screen only. Frames overwritten before a refresh could show them. Non-zero and steady is normal when the source and the head run at the same nominal rate but unsynchronised. |
| `outputs[].renderer` | Screen only. The GPU path in use, e.g. `Metal, Apple M4 Max`. |
| `source.popups` | How many times the page has tried to open a new tab or window. A page doing this repeatedly is usually about to behave oddly on air. |

`outputs[].device_index`, `outputs[].options` and `outputs[].background` are the
spec **as configured**, not what the backend is doing — that is the rest of the
object. The settings page populates its editors from them.
`background_colour` is reported even while the background is `transparent`, so
the editor's colour well shows what turning it on would give rather than
resetting to the default.

#### When an output stops answering

A vendor driver can wedge inside a call and never come back: a DeckLink did, on
Desktop Video 16.4 (issue #12). Every call into an output goes through a gate
with a time limit, and the engine's own lock is never held across one, so:

- `/api/state` still answers, within about 200 ms, and so does every other
  source: no lock above the engine is held across a call into one either. The
  stuck output shows its last figures, and once the call has been running for
  over a second, `"stale": true` and `driver_busy_ms`. If the engine's lock
  itself could not be had in time, the whole report is the previous one with a
  top-level `"stale": true`.
- A request that needs that output (enable, disable, update, remove, a format
  change) gives up on it within two seconds with an error naming it, and
  nothing is changed. A change only to its background needs no driver call and
  still goes through.
- Changes to one source queue behind each other, for up to **30 s**, because a
  format change or a settings apply across several cards legitimately takes a
  few seconds and failing the next change would lose an edit. Past that the
  request answers `409` saying another change is still in progress. But a
  queued change answers `409` **at once** if the change in front is stuck in an
  output's driver (the error names the output), or if four changes are already
  waiting: each waiter holds one of the server's 64 connections, and a button
  pressed repeatedly against a stuck driver must not fill them.
- The clock skips that output and keeps feeding the others, including during a
  format change, which no longer parks the clock while outputs reopen. If it was
  the clock that got stuck inside the driver, every output of that source stops,
  and `pacing.last_tick_age_ms` climbs.

So **`/api/state` going silent is no longer how a wedged output shows itself.** A
supervisor that restarts WebLinked on that should watch
`pacing.last_tick_age_ms` (more than a couple of seconds) and `outputs[].stale`
instead.

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
503 before the first frame, and 503 if the preview is being replaced at that
moment.

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

The same response carries a `discovery` block describing the mDNS
advertisement — the *live* result, not the setting:

```json
{ "discovery": { "enabled": true, "advertising": false,
                 "service_type": "_weblinked._tcp", "name": "",
                 "blocked_because": "the control API is bound to loopback, ..." } }
```

`enabled` is what was asked for; `advertising` is what is actually happening.
The two differ in exactly the case that generates support questions, and
`blocked_because` is the answer to "why can rookery not see this machine".

### GET `/api/show`

The running show as a show file (see `docs/05-settings.md`). `?tabs=0` or
`?outputs=0` leaves that half out. The control page adds `name` and `saved`
before offering it as a download.

## Discovery

WebLinked advertises its control API over mDNS so that fleet controllers
(rookery), Companion and anything else can find it without an address being
typed in. **This is the interoperability contract; treat it as API.**

| | |
|---|---|
| Service type | `_weblinked._tcp` |
| Port | the HTTP control port |
| Instance name | `--name`, else `<host> (<port>)` |

One record per process, not per source: a WebLinked with eight sources still
has one control API on one port.

### TXT records

| Key | Meaning |
|---|---|
| `txtvers` | `1`. Refuse a record whose version you do not know. |
| `ver` | WebLinked's version |
| `name` | the operator-facing name |
| `id` | stable across restarts, unique per instance on a host |
| `path` | `/api/state` — where to confirm this is really a WebLinked |
| `token` | `1` when a token is required, `0` when not |
| `osc` | `1` when the OSC listener is running |
| `oscport`, `oscprefix` | present only when `osc=1` |

**`oscport` and `oscprefix` are the point of the whole thing.** Neither appears
anywhere in the HTTP API, so a controller that finds an instance by any other
means has to *assume* 7655 and `/weblinked`. OSC is fire-and-forget with no
replies, so a wrong guess produces an instance that polls perfectly healthy
over HTTP and silently discards every command sent to it.

**TXT carries identity and reachability, never live state.** No source count,
no URL, no counters. Those live on HTTP, where consumers already poll. A record
that changed with state would have to be rewritten at poll rate, and every
rewrite is a multicast the whole subnet pays for.

### Two rules that surprise people

**An instance bound to loopback does not advertise, deliberately.** The default
bind is `127.0.0.1`, so this is the *common* case. Advertising it would publish
a record resolving to the browsing machine's own loopback: the service appears
in every fleet list on the subnet and every one of them fails to connect, with
nothing anywhere saying why. It is refused instead, with the reason logged and
reported in `/api/settings`. `--bind 0.0.0.0` is what makes an instance
discoverable.

**`id` is what identifies an instance, not its address.** A multi-homed machine
answers a browse *once per interface*, so one WebLinked resolves to several
entries carrying different addresses and the same `id`. Collapse on `id`, then
choose the address — a consumer that keeps whichever arrived first will
sometimes keep a loopback or link-local one and produce an entry it can never
reach.

### Turning it off

`--no-mdns`, or `"mdns_enabled": false` in the config file's `control` block.
On by default. It exists for networks where multicast is filtered or
forbidden, where the advertisement is noise that never arrives.

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
| `/api/output/add` (screen) | `{"kind":"screen","name":"Wall","options":{"display":1,"scaling":"fit"}}` | `display` indexes `state.displays`; `scaling` is `fit` (default), `fill` or `stretch`. On macOS the window is created on the main thread however the request arrived |
| `/api/output/add` (decklink) | `{"kind":"decklink","name":"SDI","device_index":0,"options":{"keying":"external","key_level":255,"premultiplied_fill":true}}` | The command line's `--decklink=N --key[=external\|internal] --key-level L --key-premultiplied`. `keying` is `external` (key + fill), `internal` (over the card's input) or absent for fill only; `premultiplied_fill` sends Chromium's premultiplied buffer for a keyer set to premultiplied key, and is ignored without `keying`. Also `preroll` (frames, default 3, at least 2), `ring` (frames made at start, default twice the preroll plus four) and `audio_channels` (default 2) |
| `/api/output/add` (stream) | `{"kind":"stream","name":"Restreamer","options":{"url":"rtmp://host:1935/live/key","bitrate":"6000k"}}` | Encodes to H.264/AAC via an `ffmpeg` on PATH and pushes to RTMP or SRT. See below |
| `/api/output/update` | `{"name":"Second","output":{"kind":"ndi","name":"Renamed"}}` | Replaces an output in place, keeping its position. On failure the previous one is restarted — see below |
| `/api/output/background` | `{"name":"Second","background":"colour","colour":"#00b140"}` | What this output composites the page over. `transparent` keeps the page's own alpha. Applies on the next tick without restarting the output — see below |
| `/api/pacing` | `{"pacing": "internal"}` | Rebuilds the browser at the same URL |
| `/api/settings/apply` | `{"source": { ... }}` | Applies a whole source configuration; see below |
| `/api/settings/save` | `{}` | Writes the live configuration to the settings file. Returns `{"ok":true,"path":"..."}` |
| `/api/settings/reload` | `{}` | Reads the settings file and applies it |
| `/api/show/load` | `{"show": { ... }, "tabs": true, "outputs": false}` | Recalls a show file. Either half may be `false`; both default to `true`. Returns `{"ok":true,"notes":[...]}`, where the notes name any saved rigs skipped because their tab is not open. 400 for a file that is not a show or lacks a half that was asked for; 409 if some of it applied |
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

### The stream output

`{"kind":"stream","options":{...}}` publishes the page to an RTMP or SRT server —
a Restreamer, a YouTube ingest, anything that takes a push. It is the only
backend that needs something installed beside WebLinked: an `ffmpeg` on `PATH`,
which does the encoding in a subprocess.

| Option | Default | |
|---|---|---|
| `url` | *required* | `rtmp://host:1935/app/key`, `rtmps://…`, `srt://host:6000` |
| `bitrate` | `6000k` | Video bitrate; also sets `maxrate`, with `bufsize` at twice it |
| `codec` | `libx264` | `copy` passes the encoder settings by |
| `preset` | `veryfast` | x264 preset |
| `tune` | *none* | e.g. `zerolatency` |
| `gop_seconds` | `2` | Keyframe interval. Every RTMP ingest wants 2 |
| `audio_codec` | `aac` | |
| `audio_bitrate` | `128k` | |
| `container` | from the URL | `flv` for RTMP, `mpegts` for SRT/UDP |
| `ffmpeg` | `ffmpeg` | Path to the binary, if it is not on `PATH` |

The status fields worth watching:

| `outputs[].connected` | ffmpeg has connected to **both** of the loopback sockets it is fed through. False with a climbing `dropped` means it never started or has died |
| `outputs[].audio_deficit_ms` | How far the audio sent lags the video sent. **This should sit at 0.** One tick of video must carry exactly one tick of audio; a number that grows is a stream whose sound is drifting away from its picture, which nothing else here would show |
| `outputs[].last_logline` | ffmpeg's own most recent line — where "Connection refused" and "Server returned 403" appear |

The stream key is a password, so it is redacted (`…/live/<key>`) everywhere it
would otherwise be written down: the log, `/api/state`, and diagnostics bundles.
Give the output a `name` that is not the key, because the name is not redacted.

**Not verified against a real server** — see section 28 of
[04-verification.md](04-verification.md) for exactly what has and has not been
established.

### Backgrounds

Each output carries its own background: either the page's own transparency —
which is what a keyed SDI fill or an NDI feed with alpha wants — or a flat
colour the page is composited over, for a switcher that only has a chroma keyer.
One browser paint serves both, so the same graphic can leave as a key down one
path and over green down another.

`background` is `"transparent"` or `"colour"`, and `background_colour` is
`#rrggbb`. The two are separate so that toggling the background off and back on
returns to the colour that was picked rather than the default. `"background"`
also accepts a colour directly — `{"background": "#00b140"}` means both at once,
which is the shorthand to reach for when writing a settings file by hand. An
unreadable colour is a 400 rather than a silent fallback: an output quietly on a
colour nobody asked for looks like a broken feature rather than a wrong file.

Backgrounds are accepted by `/api/output/add`, `/api/output/update` and
`/api/settings/apply` as well, and all three change one **without restarting the
output**. That is why the background is not part of `options`: nothing in a
backend acts on it — the engine composites before a frame reaches one — and
reopening a DeckLink to nudge a green would drop frames on air.

A composited output is fully opaque by definition, so an NDI output with both
`options.alpha` and a background sends a solid key. That is not a conflict to
resolve, it is what a chroma-key feed is.

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
could not reopen at it; check `outputs[].error` in `/api/state`. A 409 whose error
says an output **is not answering** means its driver is stuck in a call (see
"When an output stops answering" above) and nothing was changed.

503 means nothing was done and trying again shortly is the right response:
from `/api/format` when another change on the source kept it waiting, when the
clock has not ticked for a second (nothing is touched), or when the clock would
not stop for it (then the outputs are reopened at the old raster); from `/api/preview` while the preview is being
replaced; from any endpoint addressing a source that is being removed; and from
any endpoint when the server already has its limit of **64 connections** open. That one comes with `Retry-After: 1` and
`Connection: close`, before the request is read past its headers. The limit is
there because a thread per connection with none let a client polling a stuck
engine park 1,410 threads (issue #12). Every connection also has a 15-second
idle timeout and a 15-second send timeout.

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
