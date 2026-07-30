# Diagnostics

Three artefacts, because a failure on site needs different things at different
moments.

## 1. The rotating log

```
macOS    ~/Library/Logs/WebLinked/WebLinked.log
Windows  %LOCALAPPDATA%\WebLinked\logs\WebLinked.log
Linux    ~/.local/state/WebLinked/logs/WebLinked.log
```

Rotates at 4 MB keeping one previous file. Every line is flushed as it is
written — a crash must not lose the line that explains it.

| Variable | Effect |
|---|---|
| `WEBLINKED_LOG` | `trace`, `debug`, `info`, `warn`, `error`, `fatal` |
| `WEBLINKED_LOG_DIR` | Write somewhere else |

`--verbose` is equivalent to `WEBLINKED_LOG=debug`.

Warnings and errors also go to stderr, which is where an operator running from a
terminal is looking.

Page console errors are logged. That matters more than it sounds: a page whose
own assets 404 looks identical to a page that simply renders nothing, and this is
the only place it says so. `source.console_errors` in `/api/state` counts them.

## 2. The crash report

Written automatically when the process dies on SIGSEGV, SIGABRT, SIGBUS, SIGILL
or SIGFPE (or an unhandled Windows exception), next to the log:

```
WebLinked-crash-20260730-131817.json
```

It carries the build identity, the platform, the redacted configuration, the last
400 log lines and a backtrace.

```json
{
  "schema": "stoatworks.diagnostics/1",
  "kind": "crash-report",
  "reason": "SIGBUS (bus error)",
  "backtrace": ["... weblinked::Engine::clockLoop + 1364", "..."],
  "app": { "name": "WebLinked", "version": "0.1.0", "pid": 83769 },
  "platform": { "os": "macos", "os_version": "26.4.1", "arch": "arm64" },
  "config": { "http_token": "[redacted]", "...": "..." },
  "log_tail": ["..."]
}
```

Config keys whose names look like secrets (`token`, `password`, `secret`, `key`,
`auth`, `credential`, `cookie`) are redacted once, when the config is handed over,
so nothing downstream has to remember to do it.

Writing a file from a signal handler is against the rules and is done anyway,
deliberately: the process is already dying, and a report that usually arrives is
worth more than a guarantee of never deadlocking on the way out. The handler
resets to the default first, so a fault inside it still terminates.

**The crash handler is installed after `CefInitialize`**, because Chromium
installs its own and would replace it. An earlier build had it installed before,
and produced no crash reports at all.

## 3. The diagnostics bundle

```bash
curl http://127.0.0.1:7654/api/diagnostics
```

Writes one file with the build identity, platform, redacted config, recent log
lines and a list of any crash reports present, and returns its path. Deliberately
a GET so *"open this link and send me the file it names"* is one instruction, and
works from a phone.

The schema string `stoatworks.diagnostics/1` is shared with the other projects in
this fleet — the same tooling reads a crash report from a Rust daemon, a JUCE
plugin and this. Treat it as the contract.

---

## Reading the live state

`GET /api/state` is the first thing to look at when something is wrong. The
fields that diagnose most problems:

**"The output is black."** Check `source.loading`, `source.last_error` and
`source.console_errors`. A page that failed to load leaves `last_error` set. A
page that loaded but renders nothing usually has console errors. Check
`pacing.frames_published` is climbing — if it is zero, the browser has never
painted.

**"The picture is stuttering."** `pacing.dropped_ticks` rising means the clock
fell more than a frame behind — the machine is overloaded. `pacing.repeated_frames`
rising on an animated page means the browser is not keeping up, which is a page
problem rather than a machine one. `pacing.last_lateness_us` in the tens of
milliseconds means scheduler contention.

**"The SDI output glitches every few minutes."** Watch
`outputs[].buffered_frames` (DeckLink) or `buffer_level` (AJA). Steady means the
engine's clock and the card's clock agree. Drifting in either direction means
they do not, and the buffer will eventually run dry or overflow. See
[01-architecture.md](01-architecture.md) for why that is a known limitation.

**"The audio is drifting or glitching."** `audio.underruns` climbing means the
page is not producing enough audio — often a page that pauses its media element.
`audio.overruns` means the opposite. A handful of underruns at startup is normal.

**"Nobody can see the NDI source."** Check `outputs[].receivers` and
`outputs[].library` (it reports which libndi was actually loaded, and its
version). If a previous instance was killed without a clean shutdown, a stale
source can linger and the new one may take some minutes to become discoverable —
always stop it with SIGTERM or the control page rather than SIGKILL.

**"A card will not open."** `outputs[].error` carries the reason verbatim,
including the case where the card lists your raster but not at your rate — the
message names the rates it does offer.
