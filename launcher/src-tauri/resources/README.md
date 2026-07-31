# Bundled runtime

CI drops the packed WebLinked here just before `tauri build`, so the launcher
ships the renderer inside itself:

- macOS — `WebLinked.app.zip`, made with
  `ditto -c -k --sequesterRsrc --keepParent`. `ditto` rather than `zip` because
  it is the one that round-trips the CEF framework's symlinks, execute bits and
  resource forks.
- Windows and Linux — `WebLinked.zip`.

The archive is **not** committed; it is 100-odd MB and rebuilt every release.
`.gitignore` keeps it out. A build with nothing here still works — the launcher
falls back to a separately installed WebLinked, which is what `tauri dev` uses.

Why an archive rather than the tree itself, and why it is unpacked outside the
bundle rather than nested in it, is in `../src/embedded.rs`.
