#pragma once

#include <string>

#include "include/cef_app.h"

namespace weblinked {

/// The browser-process CefApp.
///
/// Its only real job is command-line surgery. Chromium's defaults are tuned for
/// an interactive browser on a laptop; several of them are actively wrong for a
/// process whose entire purpose is to paint every frame whether anyone is
/// looking or not.
class BrowserApp : public CefApp, public CefBrowserProcessHandler {
 public:
  BrowserApp() = default;

  CefRefPtr<CefBrowserProcessHandler> GetBrowserProcessHandler() override {
    return this;
  }

  void OnBeforeCommandLineProcessing(const CefString& processType,
                                     CefRefPtr<CefCommandLine> commandLine) override;

  void OnContextInitialized() override;

 private:
  IMPLEMENT_REFCOUNTING(BrowserApp);
};

/// Populates CefSettings for an offscreen render host, including the macOS
/// framework and helper paths.
///
/// `rootCachePath` is the Chromium profile directory and must always be given —
/// see settings::profileDirectory() for why leaving it to CEF is a bug.
/// `cachePath` is where cookies and storage persist, and may be empty, which
/// keeps them in memory; when set it must be `rootCachePath` or below it.
void configureCefSettings(CefSettings& settings,
                          const std::string& rootCachePath,
                          const std::string& cachePath, bool verboseLogging);

}  // namespace weblinked
