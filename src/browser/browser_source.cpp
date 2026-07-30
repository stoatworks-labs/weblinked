#include "browser/browser_source.h"

#include "include/base/cef_callback.h"
#include "include/cef_task.h"
#include "include/wrapper/cef_closure_task.h"
#include "include/wrapper/cef_helpers.h"

#include "diag/diag.h"

namespace weblinked {
namespace {

/// Posts to the CEF UI thread, or runs immediately if already on it.
template <typename Fn>
void onUiThread(Fn&& fn) {
  if (CefCurrentlyOn(TID_UI)) {
    fn();
    return;
  }
  CefPostTask(TID_UI, base::BindOnce([](Fn task) { task(); },
                                     std::forward<Fn>(fn)));
}

}  // namespace

BrowserSource::BrowserSource(VideoFormat format, LatestFrameSlot* slot,
                             AudioFifo* audio)
    : client_(new RenderClient(format, slot, audio)), format_(format) {}

BrowserSource::~BrowserSource() { close(); }

bool BrowserSource::open(const std::string& url, std::string& error) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    url_ = url;
  }

  CefWindowInfo windowInfo;
  // The whole browser is offscreen; there is no parent view.
  windowInfo.SetAsWindowless(nullptr);
  windowInfo.windowless_rendering_enabled = true;
  // Painting on request rather than on Chromium's timer. This is what makes one
  // engine tick produce exactly one paint.
  windowInfo.external_begin_frame_enabled =
      pacing_ == Pacing::kExternalBeginFrame;
  windowInfo.shared_texture_enabled = false;  // we want CPU pixels, not a texture

  CefBrowserSettings browserSettings;
  // Only consulted under internal-timer pacing, but set either way so switching
  // does not need a restart. 60 is CEF's ceiling.
  browserSettings.windowless_frame_rate = 60;
  // A transparent background so a page with no <body> colour keys correctly on
  // an alpha-capable output. Opaque outputs composite it over black.
  browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);

  CefRefPtr<CefRequestContext> context = CefRequestContext::GetGlobalContext();
  if (!CefBrowserHost::CreateBrowser(windowInfo, client_, url, browserSettings,
                                     nullptr, context)) {
    error = "CefBrowserHost::CreateBrowser failed";
    return false;
  }

  diag::info("browser: opening %s at %s", url.c_str(),
             format_.toString().c_str());
  return true;
}

void BrowserSource::close() {
  auto browser = client_ != nullptr ? client_->browser() : nullptr;
  if (browser == nullptr) {
    return;
  }
  onUiThread([browser]() {
    // force_close: we are shutting down and a beforeunload dialog would hang the
    // process waiting for a click nobody can give it.
    browser->GetHost()->CloseBrowser(true);
  });
}

void BrowserSource::loadUrl(const std::string& url) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    url_ = url;
  }
  auto browser = client_->browser();
  if (browser == nullptr) {
    return;
  }
  onUiThread([browser, url]() {
    if (auto frame = browser->GetMainFrame()) {
      frame->LoadURL(url);
    }
  });
  diag::info("browser: loading %s", url.c_str());
}

void BrowserSource::reload(bool ignoreCache) {
  auto browser = client_->browser();
  if (browser == nullptr) {
    return;
  }
  onUiThread([browser, ignoreCache]() {
    if (ignoreCache) {
      browser->ReloadIgnoreCache();
    } else {
      browser->Reload();
    }
  });
}

void BrowserSource::executeJavaScript(const std::string& script) {
  auto browser = client_->browser();
  if (browser == nullptr) {
    return;
  }
  onUiThread([browser, script]() {
    if (auto frame = browser->GetMainFrame()) {
      frame->ExecuteJavaScript(script, frame->GetURL(), 0);
    }
  });
}

void BrowserSource::setFormat(const VideoFormat& format) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (format_ == format) {
      return;
    }
    format_ = format;
  }
  client_->setFormat(format);

  auto browser = client_->browser();
  if (browser == nullptr) {
    return;
  }
  onUiThread([browser]() {
    // WasResized makes CEF re-read GetViewRect and repaint at the new size.
    browser->GetHost()->WasResized();
  });
  diag::info("browser: raster now %s", format.toString().c_str());
}

void BrowserSource::requestFrame() {
  if (pacing_ != Pacing::kExternalBeginFrame) {
    return;
  }
  auto browser = client_->browser();
  if (browser == nullptr) {
    return;
  }
  // Fire and forget. The resulting OnPaint lands on the UI thread a moment
  // later and publishes into the frame slot; the engine reads whatever is there
  // when its tick comes round, so a slow page costs a repeated frame rather
  // than a stalled output.
  onUiThread([browser]() { browser->GetHost()->SendExternalBeginFrame(); });
}

void BrowserSource::setAudioMuted(bool muted) {
  auto browser = client_->browser();
  if (browser == nullptr) {
    return;
  }
  onUiThread([browser, muted]() { browser->GetHost()->SetAudioMuted(muted); });
}

std::string BrowserSource::url() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return url_;
}

RenderClient::Diagnostics BrowserSource::diagnostics() const {
  return client_->diagnostics();
}

bool BrowserSource::isOpen() const { return client_->browser() != nullptr; }

}  // namespace weblinked
