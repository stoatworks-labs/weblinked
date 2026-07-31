# WebLinked tray launcher

> **AI-assisted project.** This codebase was created with [Claude](https://claude.com/claude-code)
> (Anthropic), directed and reviewed by a human author. The launcher shell is
> [av-launcher](https://github.com/stoatworks-labs/av-launcher), which has been
> exercised elsewhere in this fleet. The launcher builds, its config is
> unit-tested against the file this repo ships, and the panel renders — but
> **the tray has not been clicked through against a live WebLinked**. See
> [Status](#status).

A menu-bar tray app that supervises WebLinked: pick a network interface and
port, start and stop the renderer, and open its control page — without a
terminal.

The shell is app-agnostic and comes from av-launcher; everything specific to
WebLinked is [`src-tauri/launcher.toml`](src-tauri/launcher.toml).

<table>
  <tr>
    <td align="center"><img src="docs/panel-running.png" width="260" alt="Launcher panel, server running"><br><sub>running</sub></td>
    <td align="center"><img src="docs/panel-stopped.png" width="260" alt="Launcher panel, server stopped"><br><sub>stopped</sub></td>
  </tr>
</table>

*Rendered from the exact panel HTML/CSS via
[`scripts/screenshot.sh`](scripts/screenshot.sh) (headless Chrome), themed from
WebLinked's own palette.*

## Why a launcher when WebLinked already has a window

WebLinked's operator window *is* its control page, served over HTTP by the
process itself. That is the right thing when you run it from a terminal or a
show-control system. It is the wrong thing on a machine where WebLinked should
come up at login, sit quietly in the menu bar, and be reachable from a browser
on another machine — which is what a rack PC in a facility actually needs.

So the launcher runs it `--headless` and takes over the lifecycle:

| | WebLinked on its own | Under the launcher |
|---|---|---|
| UI | its own window, the control page | your browser, the same page |
| bind address | `--bind`, defaults to loopback | picked from a list of interfaces |
| lifetime | as long as the terminal | tray, survives closing the window |
| stopping | Ctrl-C or the red button | Stop, or Quit |

Both paths serve the same control page with the same settings and diagnostics.

## Running it

```bash
cd launcher
npm install
npm run tauri dev
```

Under `tauri dev` the config is read from `src-tauri/launcher.toml`, whose
`command` is relative to `src-tauri/`. Point it at your build tree first, or
target a config of your own:

```bash
AV_LAUNCHER_CONFIG=/path/to/my-weblinked.toml npm run tauri dev
```

## Building a distributable

```bash
npm run tauri build
```

The launcher does **not** embed WebLinked, which is where the rest of the fleet
puts its server. Install WebLinked to `/Applications` and the shipped
`launcher.toml` finds it there. See [Status](#status) for why.

## Status

What has been done, and what has not:

- **Verified** — the shell itself, in the other fleet apps that use it. This
  repo's `launcher.toml` is parsed by a unit test
  (`shipped_weblinked_config_produces_the_right_argv`) that asserts the exact
  argv it produces, including `--headless`; the release binary builds and comes
  up with its own log at `~/Library/Logs/WebLinked Launcher`; the panel is
  rendered above from the real HTML and CSS.
- **Not verified** — Start, Stop and Launch GUI driven against a live
  WebLinked, and the built `.app` on a machine that has never seen the source.

### Why WebLinked is not embedded

Every other app in the fleet ships its server inside the launcher bundle. This
one does not, for two reasons found in that order:

1. Tauri's resource collector walks the tree it is told to bundle. The Chromium
   Embedded Framework is a macOS framework, so `Versions/Current` and
   `Resources` are symlinks; the walk follows them and fails on a path that does
   not exist (`.../Resources/Resources`).
2. Past that, it would not be safe anyway. WebLinked is ad-hoc signed without a
   hardened runtime and carries five name-matched helper `.app`s. Nesting that
   inside another ad-hoc signed bundle is exactly the arrangement where
   Gatekeeper approves the outer app and then kills the inner helpers silently,
   with nothing in any log to say why.

Unpacking a zipped bundle on first run would work around both. It is not worth
312 MB of resource and a first-launch delay for something an operator installs
once.

The injection mode is `args`, so nothing about the launcher's behaviour depends
on parsing or patching a config file.

## Configuration

See [av-launcher's schema](https://github.com/stoatworks-labs/av-launcher/blob/main/docs/adding-an-app.md)
for the full set. WebLinked's is short, because it takes `--bind` and `--port`
directly:

```toml
[app]
name = "WebLinked"
command = "bin/WebLinked.app/Contents/MacOS/WebLinked"
args = ["--headless", "--bind", "{host}", "--port", "{port}"]
url = "http://{host}:{port}/"
default_port = 7654

[inject]
mode = "args"
```

Note what is *not* passed: `--no-settings`. The saved settings file is how an
operator's outputs survive a restart, and the launcher supplies only the bind
address and port — both of which take precedence over the file anyway.
