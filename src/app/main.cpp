// WebLinked — a URL in, SDI and video-over-IP out.
//
// Process layout on macOS and Windows differs enough to matter:
//
//   macOS: this binary is only ever the browser process. The renderer, GPU and
//          utility processes are the helper .app bundles, which run
//          helper_main.cpp. The CEF framework is loaded at run time by
//          CefScopedLibraryLoader rather than linked, which is the pattern the
//          binary distribution is built for.
//
//   Windows/Linux: this same binary is re-executed for every subprocess type,
//          so CefExecuteProcess must come first and return immediately for any
//          invocation that is not the browser.

#if defined(_WIN32)
// For GetModuleHandle below. NOMINMAX comes from CMakeLists.
#include <windows.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <system_error>
#include <thread>
#include <vector>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#if defined(__APPLE__)
#include "app/mac_application.h"
#include "include/wrapper/cef_library_loader.h"
#endif

#include "browser/cef_app.h"
#include "control/control_api.h"
#include "core/json.h"
#include "core/settings_store.h"
#include "core/video_format.h"
#include "diag/diag.h"
#include "engine/engine.h"
#include "engine/source_manager.h"
#include "outputs/output.h"

namespace {

using namespace weblinked;

struct Options {
  std::string url = "about:blank";
  VideoFormat format;
  std::vector<OutputSpec> outputs;
  ControlApi::Config control;
  bool audio = true;
  bool verbose = false;
  ColourMatrix matrix = ColourMatrix::kAuto;
  BrowserSource::Pacing pacing = BrowserSource::Pacing::kExternalBeginFrame;
  std::string cachePath;
  bool interactive = true;
  RenderClient::PopupPolicy popupPolicy =
      RenderClient::PopupPolicy::kNavigateInPlace;
  /// Empty means the platform default. See settings::defaultPath().
  std::string settingsPath;
  /// Whether a saved settings file is read at startup. Anything given on the
  /// command line still wins — see applySavedSettings().
  bool useSavedSettings = true;
  bool wantPreview = true;
  /// A configuration file describing several sources. Mutually exclusive with
  /// the per-source flags — see resolveSources().
  std::string configPath;
  /// What actually gets started: either the one source the flags describe, or
  /// every source in --config. Filled in by resolveSources() once parsing and
  /// the settings file have both had their say.
  AppConfig sources;

  /// Which of the above the operator actually typed.
  ///
  /// The settings file only fills in what the command line left alone. Without
  /// this, a saved file would quietly override an explicit `--url` — and the
  /// operator would be looking at the wrong page with no clue why.
  struct Given {
    bool url = false;
    bool format = false;
    bool matrix = false;
    bool pacing = false;
    bool interactive = false;
    bool popups = false;
    bool outputs = false;
  } given;
};

void printUsage() {
  std::printf(R"(WebLinked %s — renders a URL to SDI and video-over-IP.

Usage: weblinked [options]

Source
  --url <url>              Page to render. Default: about:blank
  --format <spec>          1080p50, 720p59.94, 1920x1080i25. Default: 1080p50
  --matrix <auto|601|709>  RGB to Y'CbCr matrix. Default: auto (709 at 720+ lines)
  --pacing <external|internal>
                           Who drives frames. Default: external (we do)
  --no-audio               Ignore the page's audio entirely
  --cache <dir>            Persist cookies and storage here. Default: none,
                           which keeps them in memory. Two instances cannot
                           share a directory — Chromium locks it — so give
                           each one its own if you set this at all.
  --popups <navigate|block>
                           What a link that wants a new tab does. navigate
                           (default) loads it here; block drops it. A real
                           popup window is never opened — see docs/01.

Outputs (repeatable; without any, only the preview runs)
  --ndi[=name]             NDI sender. Default name: WebLinked
  --omt[=name]             OMT sender. Default name: WebLinked
  --decklink[=index]       DeckLink device by index. Default: 0
  --aja[=index]            AJA device by index. Default: 0
  --alpha                  Applies to the next --ndi/--omt: send BGRA with alpha
  --key[=mode]             Applies to the next --decklink/--aja: key + fill.
                           external (default) puts fill and key on separate SDI
                           connectors; internal composites over the card's input
  --key-level <0-255>      Overall keyer opacity. Default: 255
  --no-preview             Do not run the preview output (the control page needs it)

Control
  --port <n>               HTTP control port. Default: 7654
  --bind <addr>            HTTP bind address. Default: 127.0.0.1
  --token <secret>         Require this token on every HTTP request
  --osc-port <n>           OSC listen port. Default: 7655
  --no-osc                 Do not listen for OSC
  --headless               Accepted and ignored; there is no window to suppress.
                           The control page is served over HTTP — open it in a
                           browser, or use the tray launcher in launcher/
  --no-interactive         Do not arm the preview for input on load
  --verbose                Verbose logging
  --help                   This text

Settings
  --settings <file>        Settings file to load and save. Default: the
                           platform's config directory
  --no-settings            Ignore any saved settings file

Several sources at once
  --config <file>          Run every source the file describes, each with its
                           own browser, clock and outputs. Cannot be combined
                           with the source flags above — the file replaces
                           them. --port and --token still apply.
                           The file is the same shape the settings page saves,
                           so the quickest way to write one is to configure a
                           source, save, and copy the entry.

Backends compiled into this build: )", WEBLINKED_VERSION);
  for (const auto& kind : compiledOutputKinds()) {
    std::printf("%s ", kind.c_str());
  }
  std::printf("\n");
}

/// Splits "--flag=value" and returns the value, or an empty string.
std::string inlineValue(const std::string& argument) {
  const auto equals = argument.find('=');
  return equals == std::string::npos ? std::string{} : argument.substr(equals + 1);
}

bool parseArguments(int argc, char** argv, Options& options, bool& shouldExit) {
  bool nextAlpha = false;
  std::string nextKeying;
  int keyLevel = 255;

  for (int i = 1; i < argc; ++i) {
    const std::string argument = argv[i];
    const auto needsValue = [&](std::string& out) {
      const std::string inline_ = inlineValue(argument);
      if (!inline_.empty()) {
        out = inline_;
        return true;
      }
      if (i + 1 < argc) {
        out = argv[++i];
        return true;
      }
      std::fprintf(stderr, "%s needs a value\n", argument.c_str());
      return false;
    };

    if (argument == "--help" || argument == "-h") {
      printUsage();
      shouldExit = true;
      return true;
    }
    if (argument.rfind("--url", 0) == 0) {
      if (!needsValue(options.url)) return false;
      options.given.url = true;
      continue;
    }
    if (argument.rfind("--format", 0) == 0) {
      std::string text;
      if (!needsValue(text)) return false;
      const auto parsed = VideoFormat::parse(text);
      if (!parsed) {
        std::fprintf(stderr, "cannot parse format '%s'\n", text.c_str());
        return false;
      }
      options.format = *parsed;
      options.given.format = true;
      continue;
    }
    if (argument.rfind("--matrix", 0) == 0) {
      std::string text;
      if (!needsValue(text)) return false;
      if (text == "601") options.matrix = ColourMatrix::kBt601;
      else if (text == "709") options.matrix = ColourMatrix::kBt709;
      else options.matrix = ColourMatrix::kAuto;
      options.given.matrix = true;
      continue;
    }
    if (argument.rfind("--pacing", 0) == 0) {
      std::string text;
      if (!needsValue(text)) return false;
      options.pacing = text == "internal" ? BrowserSource::Pacing::kInternalTimer
                                          : BrowserSource::Pacing::kExternalBeginFrame;
      options.given.pacing = true;
      continue;
    }
    if (argument.rfind("--cache", 0) == 0) {
      if (!needsValue(options.cachePath)) return false;
      continue;
    }
    if (argument == "--no-audio") { options.audio = false; continue; }
    // Accepted and ignored. There is no window to suppress any more, but the
    // shipped launcher.toml passes it and so does every script written against
    // an older build; rejecting it would break both for no gain.
    if (argument == "--headless") { continue; }
    if (argument == "--verbose") { options.verbose = true; continue; }
    if (argument == "--no-preview") { options.wantPreview = false; continue; }
    if (argument == "--alpha") { nextAlpha = true; continue; }
    if (argument.rfind("--key-level", 0) == 0) {
      std::string text;
      if (!needsValue(text)) return false;
      keyLevel = std::atoi(text.c_str());
      continue;
    }
    if (argument.rfind("--key", 0) == 0) {
      const std::string mode = inlineValue(argument);
      nextKeying = mode.empty() ? "external" : mode;
      if (nextKeying != "external" && nextKeying != "internal") {
        std::fprintf(stderr, "--key must be external or internal, not '%s'\n",
                     nextKeying.c_str());
        return false;
      }
      continue;
    }
    if (argument == "--no-osc") { options.control.oscEnabled = false; continue; }
    if (argument == "--no-interactive") {
      options.interactive = false;
      options.given.interactive = true;
      continue;
    }
    if (argument == "--no-settings") { options.useSavedSettings = false; continue; }
    if (argument.rfind("--settings", 0) == 0) {
      if (!needsValue(options.settingsPath)) return false;
      continue;
    }
    if (argument.rfind("--config", 0) == 0) {
      if (!needsValue(options.configPath)) return false;
      continue;
    }
    if (argument.rfind("--popups", 0) == 0) {
      std::string text;
      if (!needsValue(text)) return false;
      if (text == "block") {
        options.popupPolicy = RenderClient::PopupPolicy::kBlock;
      } else if (text == "navigate") {
        options.popupPolicy = RenderClient::PopupPolicy::kNavigateInPlace;
      } else {
        std::fprintf(stderr, "--popups must be navigate or block, not '%s'\n",
                     text.c_str());
        return false;
      }
      options.given.popups = true;
      continue;
    }

    if (argument.rfind("--ndi", 0) == 0 || argument.rfind("--omt", 0) == 0) {
      OutputSpec spec;
      spec.kind = argument.compare(0, 5, "--ndi") == 0 ? "ndi" : "omt";
      spec.name = inlineValue(argument);
      if (spec.name.empty()) {
        spec.name = "WebLinked";
      }
      if (nextAlpha) {
        spec.options.set("alpha", json::Value(true));
        nextAlpha = false;
      }
      options.outputs.push_back(spec);
      options.given.outputs = true;
      continue;
    }

    if (argument.rfind("--decklink", 0) == 0 || argument.rfind("--aja", 0) == 0) {
      OutputSpec spec;
      spec.kind = argument.compare(0, 10, "--decklink") == 0 ? "decklink" : "aja";
      const std::string index = inlineValue(argument);
      spec.deviceIndex = index.empty() ? 0 : std::atoi(index.c_str());
      spec.name = spec.kind + std::to_string(spec.deviceIndex);
      if (!nextKeying.empty()) {
        spec.options.set("keying", json::Value(nextKeying));
        spec.options.set("key_level", json::Value(keyLevel));
        nextKeying.clear();
      }
      options.outputs.push_back(spec);
      options.given.outputs = true;
      continue;
    }

    if (argument.rfind("--port", 0) == 0) {
      std::string text;
      if (!needsValue(text)) return false;
      options.control.httpPort = std::atoi(text.c_str());
      continue;
    }
    if (argument.rfind("--bind", 0) == 0) {
      if (!needsValue(options.control.httpBind)) return false;
      continue;
    }
    if (argument.rfind("--token", 0) == 0) {
      if (!needsValue(options.control.httpToken)) return false;
      continue;
    }
    if (argument.rfind("--osc-port", 0) == 0) {
      std::string text;
      if (!needsValue(text)) return false;
      options.control.oscPort = std::atoi(text.c_str());
      continue;
    }

    // CEF and Chromium switches pass straight through; they are consumed from
    // the command line CEF builds for itself.
    if (argument.rfind("--", 0) == 0) {
      continue;
    }

    std::fprintf(stderr, "unexpected argument '%s' (try --help)\n", argument.c_str());
    return false;
  }

  return true;
}

/// Puts the preview output at the front of the list if it is wanted and not
/// already there.
///
/// Called after the settings file has been merged rather than during parsing: a
/// saved configuration carries its own outputs, and inserting a second preview
/// would leave two of them fighting over one name.
void ensurePreview(Options& options) {
  if (!options.wantPreview) {
    return;
  }
  const bool present =
      std::any_of(options.outputs.begin(), options.outputs.end(),
                  [](const OutputSpec& spec) { return spec.kind == "preview"; });
  if (present) {
    return;
  }
  OutputSpec preview;
  preview.kind = "preview";
  preview.name = "preview";
  // 1/4 scale: enough to see a graphic is present and framed, cheap to move.
  preview.options.set("factor", json::Value(4));
  options.outputs.insert(options.outputs.begin(), preview);
}

/// Fills in whatever the command line left alone from a saved settings file.
///
/// Deliberately one-directional: the file never overrides an explicit flag. A
/// launcher passing --port and --headless must not have those undone by
/// whatever the last operator happened to save.
void applySavedSettings(Options& options) {
  if (!options.useSavedSettings) {
    return;
  }
  const std::string path = options.settingsPath.empty()
                               ? settings::defaultPath()
                               : options.settingsPath;
  std::string error;
  const auto file = settings::load(path, &error);
  if (!file) {
    // Absent is the normal case on a first run, so this is not a warning.
    diag::debug("settings: not loading %s: %s", path.c_str(), error.c_str());
    return;
  }
  if (file->sources.empty()) {
    diag::warn("settings: %s has no sources", path.c_str());
    return;
  }

  const SourceConfig& saved = file->sources.front();
  if (!options.given.url) options.url = saved.url;
  if (!options.given.format) options.format = saved.format;
  if (!options.given.matrix) options.matrix = saved.matrix;
  if (!options.given.pacing) {
    options.pacing = saved.externalPacing
                         ? BrowserSource::Pacing::kExternalBeginFrame
                         : BrowserSource::Pacing::kInternalTimer;
  }
  if (!options.given.interactive) {
    options.interactive = saved.interactiveByDefault;
  }
  if (!options.given.popups) {
    options.popupPolicy = saved.popupPolicy == "block"
                              ? RenderClient::PopupPolicy::kBlock
                              : RenderClient::PopupPolicy::kNavigateInPlace;
  }
  if (!options.given.outputs) {
    options.outputs.clear();
    for (const auto& output : saved.outputs) {
      options.outputs.push_back(outputSpecFromConfig(output));
    }
  }
  // --no-audio is a refusal, so it wins either way round.
  options.audio = options.audio && saved.audioEnabled;
  diag::info("settings: loaded %s", path.c_str());
}

/// Decides what actually gets started: one source from the flags, or a whole set
/// from --config.
///
/// The two are deliberately mutually exclusive rather than merged. Merging would
/// mean answering "which source does a bare --url mean?" for a file that
/// describes four of them, and every answer to that is a guess the operator
/// cannot see. Refusing is one line in a terminal; guessing wrong is the wrong
/// page on air.
///
/// Returns false with `error` set if the two were combined or the file will not
/// load.
bool resolveSources(Options& options, std::string& error) {
  if (options.configPath.empty()) {
    SourceConfig source;
    source.id = "main";
    source.url = options.url;
    source.format = options.format;
    source.audioEnabled = options.audio;
    source.matrix = options.matrix;
    source.externalPacing =
        options.pacing == BrowserSource::Pacing::kExternalBeginFrame;
    source.interactiveByDefault = options.interactive;
    source.popupPolicy = options.popupPolicy == RenderClient::PopupPolicy::kBlock
                             ? "block"
                             : "navigate";
    // ensurePreview() has already put one in options.outputs if it was wanted,
    // so asking for a second here would duplicate it.
    source.wantPreview = false;
    for (const auto& spec : options.outputs) {
      source.outputs.push_back(outputConfigFromSpec(spec));
    }
    options.sources.sources.push_back(source);
    return true;
  }

  const bool sourceFlagsGiven = options.given.url || options.given.format ||
                                options.given.matrix || options.given.pacing ||
                                options.given.interactive ||
                                options.given.popups || options.given.outputs;
  if (sourceFlagsGiven) {
    error =
        "--config describes the sources, so it cannot be combined with --url, "
        "--format, --matrix, --pacing, --interactive, --popups or an output "
        "flag. Put those in the file instead.";
    return false;
  }

  std::string loadError;
  const auto file = settings::load(options.configPath, &loadError);
  if (!file) {
    error = "cannot read " + options.configPath + ": " + loadError;
    return false;
  }
  if (file->sources.empty()) {
    error = options.configPath + " describes no sources";
    return false;
  }

  options.sources = *file;
  for (auto& source : options.sources.sources) {
    // The file may leave the preview out; the control page needs one to show
    // anything, and an operator who cannot see a source cannot fix it.
    if (options.wantPreview) {
      source.ensurePreview();
    }
  }
  // The control surface belongs to the process, not to a source, so a --port or
  // --token on the command line still wins over the file — the same rule the
  // settings file follows. Comparing against the defaults is how "not given" is
  // detected here; the flags that have a `given` bit use that instead.
  if (options.control.httpPort == 7654) {
    options.control.httpPort = options.sources.httpPort;
  }
  if (options.control.httpBind == "127.0.0.1") {
    options.control.httpBind = options.sources.httpBind;
  }
  if (options.control.httpToken.empty()) {
    options.control.httpToken = options.sources.httpToken;
  }
  if (options.control.oscPort == 7655) {
    options.control.oscPort = options.sources.oscPort;
  }
  if (options.control.oscBind == "0.0.0.0") {
    options.control.oscBind = options.sources.oscBind;
  }
  // --no-osc is a refusal, so it wins either way round.
  options.control.oscEnabled = options.control.oscEnabled && options.sources.oscEnabled;
  return true;
}


std::atomic<bool> g_quitRequested{false};

/// UI thread. Begins an orderly shutdown.
///
/// Quitting the loop directly is correct now that this process never owns a
/// browser *window*: CefRunMessageLoop() returns as soon as it is asked to.
/// Closing the offscreen browsers here would be wrong — their destruction then
/// never gets pumped and CefShutdown() waits for them forever. They are closed
/// after the loop returns, in engine.stop(), which is where it was already being
/// done and which works.
void beginShutdown() { CefQuitMessageLoop(); }

void requestQuit(int) {
  // Only a flag: the watchdog below does the real work, because CefPostTask is
  // not something to call from a signal handler.
  g_quitRequested.store(true);
}

/// Turns SIGINT and SIGTERM into an orderly shutdown.
///
/// This matters more than it looks for a headless renderer. Without it, a
/// `kill` or a Ctrl-C tears the process down without destroying the NDI and OMT
/// senders or releasing the SDI device — which leaves a stale source advertised
/// on the network that receivers keep trying to connect to, and a card that the
/// next application cannot claim. Learned by doing exactly that during
/// verification and then wondering why nothing could discover the next run.
void installSignalHandlers() {
  std::signal(SIGINT, requestQuit);
  std::signal(SIGTERM, requestQuit);
#if !defined(_WIN32)
  // A control page left open in a closed terminal should not kill the output.
  std::signal(SIGHUP, SIG_IGN);
  // Writing to a socket whose peer has gone must not be fatal.
  std::signal(SIGPIPE, SIG_IGN);
#endif

  std::thread([] {
    while (!g_quitRequested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    weblinked::diag::info("shutdown requested by signal");
    // Must run on the UI thread.
    CefPostTask(TID_UI, base::BindOnce(&beginShutdown));
  }).detach();
}

json::Value describeOptions(const Options& options) {
  json::Value value = json::Value::object();
  value.set("url", json::Value(options.url));
  value.set("format", json::Value(options.format.toString()));
  value.set("audio", json::Value(options.audio));
  value.set("http_bind", json::Value(options.control.httpBind));
  value.set("http_port", json::Value(options.control.httpPort));
  // Named so the diag module's redaction catches it.
  value.set("http_token", json::Value(options.control.httpToken));
  value.set("osc_port", json::Value(options.control.oscPort));
  value.set("interactive", json::Value(options.interactive));
  value.set("popups",
            json::Value(options.popupPolicy == RenderClient::PopupPolicy::kBlock
                            ? "block"
                            : "navigate"));
  json::Value outputs = json::Value::array();
  for (const auto& spec : options.outputs) {
    json::Value entry = json::Value::object();
    entry.set("kind", json::Value(spec.kind));
    entry.set("name", json::Value(spec.name));
    outputs.push(entry);
  }
  value.set("outputs", outputs);
  return value;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(__APPLE__)
  // Must happen before any other CEF call: nothing links against the framework,
  // so until this succeeds none of the CEF symbols exist.
  CefScopedLibraryLoader libraryLoader;
  if (!libraryLoader.LoadInMain()) {
    std::fprintf(stderr,
                 "Could not load the Chromium Embedded Framework.\n"
                 "The app bundle is incomplete — see docs/02-building.md.\n");
    return 1;
  }
#endif

#if defined(__APPLE__)
  // Before anything else touches NSApp. See mac_application.mm.
  weblinked::installMacApplication();
#endif

  // CefMainArgs is one of the few genuinely different shapes across platforms:
  // POSIX takes the command line, Windows takes the module handle and reads its
  // own command line from the OS.
#if defined(_WIN32)
  CefMainArgs mainArgs(::GetModuleHandle(nullptr));
#else
  CefMainArgs mainArgs(argc, argv);
#endif

#if !defined(__APPLE__)
  // On Windows and Linux this binary is also every subprocess. A non-negative
  // result means we were a subprocess and have finished.
  {
    CefRefPtr<weblinked::BrowserApp> subprocessApp(new weblinked::BrowserApp());
    const int exitCode = CefExecuteProcess(mainArgs, subprocessApp, nullptr);
    if (exitCode >= 0) {
      return exitCode;
    }
  }
#endif

  Options options;
  options.format = *VideoFormat::parse("1080p50");
  bool shouldExit = false;
  if (!parseArguments(argc, argv, options, shouldExit)) {
    return 2;
  }
  if (shouldExit) {
    return 0;
  }

  weblinked::diag::Options diagOptions;
  diagOptions.appName = "WebLinked";
  diagOptions.envPrefix = "WEBLINKED";
  diagOptions.version = WEBLINKED_VERSION;
  diagOptions.defaultLevel = options.verbose ? weblinked::diag::Level::kDebug
                                             : weblinked::diag::Level::kInfo;
  // Installed after CefInitialize instead — Chromium would replace it. See
  // installSignalHandlers() below for the same trap.
  diagOptions.installCrashHandler = false;
  weblinked::diag::init(diagOptions);

  // After logging is up, so a settings file that will not parse says so
  // somewhere an operator can find it; before the config is recorded, so a
  // crash report describes what is actually running.
  // --config is more specific than the saved settings file, and reading both
  // would mean a source from one and a raster from the other.
  if (options.configPath.empty()) {
    applySavedSettings(options);
    ensurePreview(options);
  }
  std::string sourceError;
  if (!resolveSources(options, sourceError)) {
    weblinked::diag::error("%s", sourceError.c_str());
    std::fprintf(stderr, "%s\n", sourceError.c_str());
    return 1;
  }
  weblinked::diag::setConfig(describeOptions(options));

  // Before CefInitialize, because a control port already in use almost always
  // means another WebLinked is running — and that same clash reaches Chromium's
  // profile lock first, where it becomes a dialog about a profile that cannot
  // be loaded rather than a sentence about a port.
  if (!weblinked::HttpServer::portAvailable(options.control.httpBind,
                                            options.control.httpPort)) {
    const std::string message =
        "control port " + std::to_string(options.control.httpPort) + " on " +
        options.control.httpBind +
        " is already in use — another WebLinked is probably running. Quit it, "
        "or give this one --port.";
    weblinked::diag::error("%s", message.c_str());
    std::fprintf(stderr, "%s\n", message.c_str());
    return 1;
  }

  // Chromium keeps one lock per profile directory and refuses to start a second
  // browser process in the same one. --cache names a profile explicitly, so it
  // serves as both the root and the persistent cache; without it each instance
  // gets a directory of its own and storage stays in memory.
  const std::string rootCachePath =
      options.cachePath.empty()
          ? weblinked::settings::profileDirectory(options.control.httpPort)
          : options.cachePath;
  std::error_code cacheError;
  std::filesystem::create_directories(rootCachePath, cacheError);
  if (cacheError) {
    const std::string message = "cannot create the profile directory " +
                                rootCachePath + ": " + cacheError.message();
    weblinked::diag::error("%s", message.c_str());
    std::fprintf(stderr, "%s\n", message.c_str());
    return 1;
  }

  CefSettings settings;
  weblinked::configureCefSettings(settings, rootCachePath, options.cachePath,
                                  options.verbose);

  CefRefPtr<weblinked::BrowserApp> app(new weblinked::BrowserApp());
  if (!CefInitialize(mainArgs, settings, app, nullptr)) {
    weblinked::diag::error("CefInitialize failed");
    std::fprintf(stderr, "CefInitialize failed — see %s\n",
                 weblinked::diag::logFilePath().c_str());
    return 1;
  }

  // *After* CefInitialize, not before. Chromium installs its own SIGTERM and
  // SIGINT handlers during initialisation, so anything registered earlier is
  // silently replaced — the process still dies on `kill`, it just skips our
  // orderly shutdown, which is exactly the failure this is here to prevent.
  installSignalHandlers();
  weblinked::diag::installCrashHandler();

  weblinked::SourceManager sources;
  weblinked::SourceManager::Options managerOptions;
  managerOptions.cachePath = options.cachePath;

  std::string error;
  if (!sources.start(options.sources, managerOptions, error)) {
    weblinked::diag::error("no source could be started: %s", error.c_str());
    std::fprintf(stderr, "%s\n", error.c_str());
    CefShutdown();
    return 1;
  }
  if (!error.empty()) {
    // Some sources came up and some did not. Worth saying out loud on the
    // terminal as well as in the log: the operator is about to see fewer feeds
    // than they asked for, and silence would make that look like a bug here.
    std::fprintf(stderr, "warning: %s\n", error.c_str());
  }

  weblinked::ControlApi control(&sources);
  options.control.settingsPath = options.settingsPath;
  if (!control.start(options.control, error)) {
    weblinked::diag::error("control surface failed to start: %s", error.c_str());
    std::fprintf(stderr, "%s\n", error.c_str());
    sources.stop();
    CefShutdown();
    return 1;
  }

  if (options.sources.sources.size() == 1) {
    std::printf("WebLinked %s — %s at %s\n", WEBLINKED_VERSION,
                options.sources.sources.front().url.c_str(),
                options.sources.sources.front().format.toString().c_str());
  } else {
    std::printf("WebLinked %s — %zu sources\n", WEBLINKED_VERSION,
                sources.size());
    for (const auto& source : options.sources.sources) {
      std::printf("  %-12s %s at %s\n", source.id.c_str(), source.url.c_str(),
                  source.format.toString().c_str());
    }
  }
  std::printf("Control: %s\n", control.controlUrl().c_str());
  std::printf("Log:     %s\n", weblinked::diag::logFilePath().c_str());

  // No window is ever created here. This process is a render host and a control
  // server; the operator's view is a browser pointed at the address above, and
  // the tray launcher in launcher/ is what puts that on a desktop.
  //
  // It used to open the control page in a CEF window of its own. That could not
  // work: every source browser is windowless, windowless always means Alloy
  // runtime style, and a windowed browser defaults to Chrome style — so the
  // process ran two runtime styles at once, the GPU process segfaulted on
  // startup, and the window came up correctly sized and completely empty. It
  // also never had a menu bar, so it could not be quit from one.
  //
  // Blocks until beginShutdown() quits it. Everything else runs on its own
  // thread: the engine's clock, the HTTP connections, the OSC receiver.
  CefRunMessageLoop();
  control.stop();
  sources.stop();
  CefShutdown();
  weblinked::diag::info("WebLinked exiting cleanly");
  weblinked::diag::shutdown();
  return 0;
}
