#pragma once

#include <string>

namespace weblinked {

/// One WebLinked process, owned by one plugin instance.
///
/// **Why the plugin runs a process at all.** Resolume can already show a
/// WebLinked feed through its own Syphon input, so a plugin that only attached
/// to one would be worth nothing. What this buys is that the URL lives *in the
/// composition*: saved with the comp, per clip, deck-triggerable, with no
/// second application for an operator to start and keep alive.
///
/// It is a deliberate divergence from `cartridge`, whose plugin never launches
/// its helper. There the helper is optional — the core can run in process —
/// so attaching to one an operator started is a reasonable default. Here there
/// is no in-process option at all, because Chromium cannot be hosted in
/// Resolume's process, so "attach to something you started yourself" would be
/// the whole product.
///
/// **Nothing here may run during a plugin scan.** Resolume instantiates every
/// plugin it finds when it scans a folder, and a fresh instance has an empty
/// URL. `start()` is only ever reached with a non-empty URL, which is what
/// stops a scan launching a browser per installed plugin. That guard is in the
/// caller and it is load-bearing; see WebLinkedPlugin::ProcessOpenGL.
class Helper {
 public:
  Helper() = default;
  ~Helper();

  Helper(const Helper&) = delete;
  Helper& operator=(const Helper&) = delete;

  /// Launches WebLinked headless, rendering `url` and publishing under
  /// `sourceName`. Returns false with `error` set to something an operator can
  /// act on — a missing binary being much the most likely.
  ///
  /// Blocking, and called from the render thread, but only on a change: a
  /// spawn costs a few milliseconds once, not every frame.
  bool start(const std::string& url, const std::string& sourceName, std::string& error);

  /// Terminates the process and reaps it. Safe to call when nothing is running.
  void stop();

  /// True while the child is alive. Polls rather than blocking, and reaps the
  /// child when it has exited, so a crashed helper does not become a zombie.
  bool alive();

  /// Points the running page at a different URL, through WebLinked's control
  /// API, so a URL edit does not cost a process restart and the black frame
  /// that comes with one. Returns false if there is no helper or it refused.
  bool setUrl(const std::string& url);

  /// The control port this helper was given, or 0. Each instance gets its own:
  /// several plugins on several layers are several browsers, and they must not
  /// fight over 7654.
  int port() const { return port_; }

  /// Where the WebLinked binary was found, for the log and for diagnosing the
  /// case where it was not.
  static std::string findBinary();

 private:
  long pid_ = 0;  ///< pid_t, but this header is included by plain C++.
  int port_ = 0;
  std::string sourceName_;
};

}  // namespace weblinked
