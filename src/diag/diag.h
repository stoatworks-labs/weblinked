#pragma once

#include <string>
#include <vector>

#include "core/json.h"

/// Logging and crash diagnostics.
///
/// Three artefacts, because a failure on site needs different things at
/// different moments:
///
///   1. A rotating human-readable log, so an operator can see what happened.
///   2. A machine-readable crash report written when the process dies, carrying
///      the build identity, the platform, the redacted config, the last few
///      hundred log lines and a backtrace.
///   3. A single-file diagnostics bundle on demand, so "send me your
///      diagnostics" is one instruction.
///
/// The schema string `stoatworks.diagnostics/1` is shared with the other repos
/// in this fleet — the same tooling reads a crash report from a Rust daemon, a
/// JUCE plugin and this. Treat it as the contract.
///
/// This is the dependency-free C++ port of that module: no JUCE, no CEF, just
/// the standard library, so it can be initialised before anything that might
/// fail.
namespace weblinked::diag {

enum class Level { kTrace, kDebug, kInfo, kWarn, kError, kFatal };

struct Options {
  /// Names the log files, e.g. "WebLinked".
  std::string appName = "WebLinked";
  /// Scopes the environment variables: {PREFIX}_LOG, {PREFIX}_LOG_DIR.
  std::string envPrefix = "WEBLINKED";
  std::string version;
  Level defaultLevel = Level::kInfo;
  /// A CEF helper process must not install a process-wide crash handler; the
  /// browser process owns that.
  bool installCrashHandler = true;
  /// Rotate at this size, keeping one previous file.
  size_t maxLogBytes = 4 * 1024 * 1024;
};

/// Installs logging and, unless disabled, the crash handler. Call once, as early
/// as possible — before anything that can fail.
void init(const Options& options);

/// Installs the crash handler on its own.
///
/// Separate from init() because of a specific trap: Chromium installs its own
/// signal handlers during CefInitialize, silently replacing any registered
/// earlier. Logging must be up before CEF starts, but the crash handler has to
/// be installed *after* it or crash reports simply never appear. Set
/// Options::installCrashHandler to false, then call this once CEF is running.
void installCrashHandler();

/// Flushes and closes the log.
void shutdown();

/// Attaches the effective configuration to crash reports and bundles.
///
/// Separate from init() so logging is up before settings are read. Keys whose
/// names look like secrets are redacted here, once.
void setConfig(const json::Value& config);

void log(Level level, const char* format, ...);

void trace(const char* format, ...);
void debug(const char* format, ...);
void info(const char* format, ...);
void warn(const char* format, ...);
void error(const char* format, ...);

/// Where the log and crash reports are written.
std::string logDirectory();
std::string logFilePath();

/// Raises or lowers the level at run time, so an operator can turn on debug
/// logging from the control page while the fault is happening rather than
/// restarting the show to reproduce it.
void setLevel(Level level);
Level level();

/// The most recent log lines, oldest first, up to `lines`.
///
/// Served from the in-memory ring rather than by reading the file back: the
/// ring survives a rotation, and reading the log from the HTTP thread while the
/// logging thread is appending to it is a race for no benefit.
std::vector<std::string> tail(size_t lines);

/// Writes a crash report immediately, without a crash. Used by the control API's
/// diagnostics endpoint and by tests.
///
/// `reason` describes why; pass something an operator would recognise.
std::string writeReport(const std::string& reason);

/// Writes a single-file diagnostics bundle and returns its path.
std::string collectBundle();

Level levelFromString(const std::string& text);
const char* levelToString(Level level);

}  // namespace weblinked::diag
