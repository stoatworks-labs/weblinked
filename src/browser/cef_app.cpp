#include "browser/cef_app.h"

#include "diag/diag.h"

namespace weblinked {

void BrowserApp::OnBeforeCommandLineProcessing(
    const CefString& processType, CefRefPtr<CefCommandLine> commandLine) {
  // Only the browser process; the renderer inherits what it needs.
  if (!processType.empty()) {
    return;
  }

  // Chromium throttles or entirely stops rendering a window it believes nobody
  // can see. For an offscreen render host that is exactly backwards: the output
  // is going to SDI, so "occluded" and "backgrounded" must not mean "stop
  // painting". Without these, output freezes the moment the app loses focus,
  // which is guaranteed to happen during a show.
  commandLine->AppendSwitch("disable-backgrounding-occluded-windows");
  commandLine->AppendSwitch("disable-renderer-backgrounding");
  commandLine->AppendSwitch("disable-background-timer-throttling");

  // No disable-frame-rate-limit. It was here to stop a 60 Hz panel capping a
  // 50p output, but an offscreen browser is paced by our begin frames (or
  // windowless_frame_rate), not by any display. What the switch actually did was
  // uncap the page: requestAnimationFrame and CSS animations ran at ~1,000/s on
  // a 2-core PC and ~10,000/s on an M4 Max for a 50p output, burning whole
  // cores in the renderer, starving page loads and the screen output, and
  // turning smooth motion into cuts on any machine without cores to spare.
  // See §34 of docs/04-verification.md.
  commandLine->AppendSwitch("disable-gpu-vsync");

  // Autoplay: a page whose video only starts after a click is useless as a
  // source. This is the switch that makes an unattended feed work.
  commandLine->AppendSwitchWithValue("autoplay-policy", "no-user-gesture-required");

  // Keep the media stack's audio inside the process so CefAudioHandler sees it
  // rather than it going straight to the system output device.
  commandLine->AppendSwitchWithValue("disable-features", "AudioServiceOutOfProcess");

  // Chromium's password manager asks the OS keyring for a key to encrypt its
  // login database, and on macOS that raises a Keychain authorisation dialog on
  // every single launch. A render host that never signs in to anything has no
  // use for a password store, and a modal dialog appearing on a machine that is
  // live to air is not acceptable. These two switches keep it away from the
  // keyring entirely.
  commandLine->AppendSwitchWithValue("password-store", "basic");
  commandLine->AppendSwitch("use-mock-keychain");
}

void BrowserApp::OnContextInitialized() {
  diag::info("cef: context initialised");
}

void configureCefSettings(CefSettings& settings,
                          const std::string& rootCachePath,
                          const std::string& cachePath, bool verboseLogging) {
  settings.no_sandbox = true;
  // The whole point: paint into our buffer rather than onto a screen.
  settings.windowless_rendering_enabled = true;
  // We run CefRunMessageLoop on the main thread ourselves.
  settings.multi_threaded_message_loop = false;
  settings.log_severity = verboseLogging ? LOGSEVERITY_INFO : LOGSEVERITY_WARNING;

  // Never left unset. CEF's default profile directory is shared by every CEF
  // application on the machine, and Chromium allows exactly one browser process
  // per profile directory — so the second one to start finds the lock taken,
  // fails to hand off to a headless render host that has no window to raise,
  // and puts up "your profile could not be loaded correctly" instead of video.
  // CEF warns about precisely this if you leave it alone.
  if (!rootCachePath.empty()) {
    CefString(&settings.root_cache_path).FromString(rootCachePath);
  }

  // Separate from the profile directory: this is what survives a restart.
  // Empty is meaningful — it keeps cookies and storage in memory, which is what
  // an operator gets when they do not pass --cache.
  if (!cachePath.empty()) {
    CefString(&settings.cache_path).FromString(cachePath);
  }

  // On macOS the framework and the helper apps are found by convention inside
  // the bundle, and browser_subprocess_path must be left unset — see
  // cmake/MacBundle.cmake for the naming rules CEF relies on.
}

}  // namespace weblinked
