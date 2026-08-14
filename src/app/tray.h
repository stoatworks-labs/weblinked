#pragma once

#include <functional>
#include <string>

namespace weblinked {

/// What the menu-bar item shows, and what its menu can do.
struct TrayOptions {
  std::string appName;     ///< "WebLinked" — the menu's first line.
  std::string version;
  std::string controlUrl;  ///< What "Open control page" hands to a browser.
  /// One line of live state, re-read each time the menu opens rather than on a
  /// timer: a status item nobody has clicked should cost nothing to keep true.
  /// Called on the UI thread, so it must not block — SourceManager::size() and
  /// the like, not a lock the clock thread may be holding.
  std::function<std::string()> status;
  /// "Quit". Called on the UI thread.
  std::function<void()> quit;
};

/// Installs a menu-bar status item, and says whether it got one.
///
/// All three desktop platforms have an implementation: NSStatusItem on macOS,
/// Shell_NotifyIcon on Windows, StatusNotifierItem through
/// libayatana-appindicator on Linux.
///
/// False means there was nowhere to put one, which is an ordinary outcome
/// rather than a failure — a machine with no window server session (an ssh
/// login, a launchd daemon, a container), a Linux box with no desktop
/// libraries, or a shell that refused the icon. Nothing downstream should treat
/// it as an error: WebLinked runs headless by design and the control page is
/// reachable either way. The reason is always in the log.
///
/// This is **not** the operator window returning. That was a CEF browser, and
/// the reason it had to go is specific to browsers: a windowed one defaults to
/// Chrome runtime style while every source here is windowless and therefore
/// Alloy, so the process ran two runtime styles at once and the GPU process
/// segfaulted on every launch (docs/04-verification.md section 9, measured).
/// A status item is AppKit and owns no browser, so it cannot reintroduce that
/// — the same reasoning that already lets the screen output own a real
/// NSWindow. Rule 9 in CLAUDE.md is about browsers, and this is not one.
///
/// Call on the main thread, after installMacApplication() has put the right
/// NSApplication subclass in place and after CefInitialize, so that Chromium
/// has finished standing NSApp up before a status item is hung off it.
bool installTray(const TrayOptions& options);

/// Takes the status item away again. Safe to call when none was installed, and
/// safe to call twice.
void removeTray();

}  // namespace weblinked
