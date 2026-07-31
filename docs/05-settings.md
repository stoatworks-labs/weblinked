# Settings

Everything the settings page edits, where it is kept, and what wins when two
things disagree.

## The page

`http://127.0.0.1:7654/` → **Settings**. Three groups:

**Outputs.** One editor per output, showing only the fields that backend
actually has — an NDI sender gets a name and an alpha checkbox, a DeckLink gets
a device index and keying, the preview gets its scale factor. Offering a
DeckLink keying mode on an NDI output would invite an operator to set something
that is then silently ignored.

Each editor applies on its own. **Apply** replaces that output in place rather
than removing and re-adding it: if the new settings cannot open — a card already
claimed, an NDI name in use — the previous output is restarted and the reason is
shown. Renaming an output must not be able to leave you with no output.

**Source.** URL, format, colour matrix, pacing, what a new tab does, and whether
the preview arms itself on load. **Apply** sends the lot as one request, which
the engine reconciles: anything that has not actually changed is left running.

**Saved settings.** Where the file is, and Save / Reload.

## The file

```
macOS    ~/Library/Application Support/WebLinked/settings.json
Windows  %APPDATA%\WebLinked\settings.json
Linux    $XDG_CONFIG_HOME/WebLinked/settings.json, else ~/.config/WebLinked/
```

Overridden by `--settings <file>` or `$WEBLINKED_SETTINGS`. `--no-settings`
ignores it entirely.

Deliberately not the log directory. Logs are disposable and belong in the
platform's log location; this is the show's configuration and belongs where
somebody would think to back it up.

```json
{
  "control": { "http_bind": "127.0.0.1", "http_port": 7654,
               "osc_enabled": true, "osc_bind": "0.0.0.0", "osc_port": 7655 },
  "sources": [
    {
      "id": "main",
      "url": "https://example.com/graphic.html",
      "format": "1920x1080p50",
      "audio": true,
      "matrix": "auto",
      "pacing": "external",
      "interactive": true,
      "popups": "navigate",
      "outputs": [
        { "kind": "preview", "name": "preview", "options": { "factor": 4 } },
        { "kind": "ndi", "name": "Graphic", "options": { "alpha": true } }
      ]
    }
  ]
}
```

It is the same shape `GET /api/settings` returns, so a file can be written by
hand and what the API gives back can be pasted into one. `sources` is an array
because the configuration format already allows for several pipelines in one
process; the engine runs the first.

### Two things it does on purpose

**Save records what is running, not what was asked for.** The body of a save
request is ignored. An output that failed to open is therefore never written
down as working — otherwise a settings file would happily assert a card that has
not been in the machine for a month.

**The write is atomic.** It goes to `settings.json.tmp` and is renamed over the
target. An interrupted save costs the *new* settings rather than the old ones;
losing an edit is recoverable, coming up with an empty output list is not.

## Precedence

The command line always wins. The file only fills in what was left alone:

```bash
# outputs and format from the file; this URL, because it was typed
weblinked --url https://example.com/tonight
```

Without that rule, a launcher passing `--port` and `--headless` could have them
undone by whatever the last operator happened to save, and an operator who typed
`--url` would be looking at the wrong page with no clue why.

`--no-audio` is the exception in spirit rather than in mechanism: it is a
refusal, so it wins whichever way round it is combined with the file's `audio`.

## What is not in the file

`audio` is read at launch and not applied by `/api/settings/apply`. It decides
whether audio is captured and prepared at all — the clock thread reads it on
every tick — and it is not the same thing as the runtime mute the control page
offers. Changing it takes effect next launch.

The control surface's own bind address, port and token are saved, but the
running process does not re-bind itself from a reload. They are there so the
file describes a whole deployment.

## The profile directory

Chromium keeps a profile directory — its "user data dir" — separate from
anything above, and it is not something the settings page edits. WebLinked puts
it here:

```
macOS    ~/Library/Application Support/WebLinked/profiles/<control port>
Windows  %APPDATA%\WebLinked\profiles\<control port>
Linux    $XDG_CONFIG_HOME/WebLinked/profiles/<control port>
```

Chromium permits **exactly one browser process per profile directory** and
enforces it with a lock file inside it. A second process that finds the lock
taken tries to hand the request to the holder, and when the holder is a headless
render host with no window to raise, the attempt times out and Chromium puts up
*"Your profile could not be loaded correctly"* — a dialog, on a machine that is
probably live to air, instead of a picture.

Keying the directory on the control port is what makes several instances legal:
two of them cannot share a port, so they cannot collide on a profile either.
Nothing valuable is kept there — without `--cache`, cookies and storage stay in
memory and the directory holds only Chromium's own scratch state, so deleting it
while WebLinked is not running costs nothing.

Two ways to still get it wrong:

- **`--cache <dir>`** makes that directory both the profile and the persistent
  store, overriding the per-port default. Pointing two instances at one `--cache`
  directory brings the clash straight back. Give each its own.
- **Reusing a port.** Caught before Chromium starts, so it reads
  `control port 7654 on 127.0.0.1 is already in use — another WebLinked is
  probably running`, which is the actual problem.

Earlier builds set no profile directory at all, which left CEF using a default
shared by *every* CEF application on the machine — so a second WebLinked, or an
unrelated app built on CEF, could take the lock first.
