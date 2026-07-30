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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "include/base/cef_callback.h"
#include "include/cef_app.h"
#include "include/cef_command_line.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#if defined(__APPLE__)
#include "include/wrapper/cef_library_loader.h"
#endif

#include "browser/cef_app.h"
#include "control/control_api.h"
#include "core/json.h"
#include "core/video_format.h"
#include "diag/diag.h"
#include "engine/engine.h"
#include "outputs/output.h"

namespace {

using namespace weblinked;

struct Options {
  std::string url = "about:blank";
  VideoFormat format;
  std::vector<OutputSpec> outputs;
  ControlApi::Config control;
  bool audio = true;
  bool openWindow = true;
  bool verbose = false;
  ColourMatrix matrix = ColourMatrix::kAuto;
  BrowserSource::Pacing pacing = BrowserSource::Pacing::kExternalBeginFrame;
  std::string cachePath;
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
  --cache <dir>            Persist cookies and storage here. Default: none

Outputs (repeatable; without any, only the preview runs)
  --ndi[=name]             NDI sender. Default name: WebLinked
  --omt[=name]             OMT sender. Default name: WebLinked
  --decklink[=index]       DeckLink device by index. Default: 0
  --aja[=index]            AJA device by index. Default: 0
  --alpha                  Applies to the next --ndi/--omt: send BGRA with alpha
  --no-preview             Do not run the preview output (the control page needs it)

Control
  --port <n>               HTTP control port. Default: 7654
  --bind <addr>            HTTP bind address. Default: 127.0.0.1
  --token <secret>         Require this token on every HTTP request
  --osc-port <n>           OSC listen port. Default: 7655
  --no-osc                 Do not listen for OSC
  --headless               Do not open the operator window
  --verbose                Verbose logging
  --help                   This text

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
  bool wantPreview = true;

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
      continue;
    }
    if (argument.rfind("--matrix", 0) == 0) {
      std::string text;
      if (!needsValue(text)) return false;
      if (text == "601") options.matrix = ColourMatrix::kBt601;
      else if (text == "709") options.matrix = ColourMatrix::kBt709;
      else options.matrix = ColourMatrix::kAuto;
      continue;
    }
    if (argument.rfind("--pacing", 0) == 0) {
      std::string text;
      if (!needsValue(text)) return false;
      options.pacing = text == "internal" ? BrowserSource::Pacing::kInternalTimer
                                          : BrowserSource::Pacing::kExternalBeginFrame;
      continue;
    }
    if (argument.rfind("--cache", 0) == 0) {
      if (!needsValue(options.cachePath)) return false;
      continue;
    }
    if (argument == "--no-audio") { options.audio = false; continue; }
    if (argument == "--headless") { options.openWindow = false; continue; }
    if (argument == "--verbose") { options.verbose = true; continue; }
    if (argument == "--no-preview") { wantPreview = false; continue; }
    if (argument == "--alpha") { nextAlpha = true; continue; }
    if (argument == "--no-osc") { options.control.oscEnabled = false; continue; }

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
      continue;
    }

    if (argument.rfind("--decklink", 0) == 0 || argument.rfind("--aja", 0) == 0) {
      OutputSpec spec;
      spec.kind = argument.compare(0, 10, "--decklink") == 0 ? "decklink" : "aja";
      const std::string index = inlineValue(argument);
      spec.deviceIndex = index.empty() ? 0 : std::atoi(index.c_str());
      spec.name = spec.kind + std::to_string(spec.deviceIndex);
      options.outputs.push_back(spec);
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

  if (wantPreview) {
    OutputSpec preview;
    preview.kind = "preview";
    preview.name = "preview";
    // 1/4 scale: enough to see a graphic is present and framed, cheap to move.
    preview.options.set("factor", json::Value(4));
    options.outputs.insert(options.outputs.begin(), preview);
  }
  return true;
}

std::atomic<bool> g_quitRequested{false};

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
    // CefQuitMessageLoop must run on the UI thread.
    CefPostTask(TID_UI, base::BindOnce(&CefQuitMessageLoop));
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

  CefMainArgs mainArgs(argc, argv);

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
  weblinked::diag::setConfig(describeOptions(options));

  CefSettings settings;
  weblinked::configureCefSettings(settings, options.cachePath, options.verbose);

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

  weblinked::Engine engine;
  weblinked::Engine::Config engineConfig;
  engineConfig.url = options.url;
  engineConfig.format = options.format;
  engineConfig.outputs = options.outputs;
  engineConfig.audioEnabled = options.audio;
  engineConfig.matrix = options.matrix;
  engineConfig.pacing = options.pacing;
  engineConfig.cachePath = options.cachePath;

  std::string error;
  if (!engine.start(engineConfig, error)) {
    weblinked::diag::error("engine failed to start: %s", error.c_str());
    std::fprintf(stderr, "%s\n", error.c_str());
    CefShutdown();
    return 1;
  }

  weblinked::ControlApi control(&engine);
  if (!control.start(options.control, error)) {
    weblinked::diag::error("control surface failed to start: %s", error.c_str());
    std::fprintf(stderr, "%s\n", error.c_str());
    engine.stop();
    CefShutdown();
    return 1;
  }

  std::printf("WebLinked %s — %s at %s\n", WEBLINKED_VERSION, options.url.c_str(),
              options.format.toString().c_str());
  std::printf("Control: %s\n", control.controlUrl().c_str());
  std::printf("Log:     %s\n", weblinked::diag::logFilePath().c_str());

  if (options.openWindow) {
    // The operator window is the same control page, in an ordinary windowed
    // browser. This is why there is no GUI toolkit in this project.
    CefWindowInfo windowInfo;
    CefRect bounds(0, 0, 1180, 900);
#if defined(_WIN32)
    windowInfo.SetAsPopup(nullptr, "WebLinked");
    windowInfo.bounds = bounds;
#else
    windowInfo.bounds = bounds;
#endif
    CefBrowserSettings browserSettings;
    CefRefPtr<CefClient> nullClient;  // default handling is all the UI needs
    CefBrowserHost::CreateBrowser(windowInfo, nullClient, control.controlUrl(),
                                  browserSettings, nullptr, nullptr);
  }

  // Blocks until the last browser closes. Everything else runs on its own
  // thread: the engine's clock, the HTTP connections, the OSC receiver.
  CefRunMessageLoop();

  control.stop();
  engine.stop();
  CefShutdown();
  weblinked::diag::info("WebLinked exiting cleanly");
  weblinked::diag::shutdown();
  return 0;
}
