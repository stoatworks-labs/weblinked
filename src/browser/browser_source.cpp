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

/// Creates the offscreen browser. Free of BrowserSource so it can be posted to
/// the UI thread without capturing `this` — see setPacing.
bool createOffscreenBrowser(const CefRefPtr<RenderClient>& client,
                            BrowserSource::Pacing pacing, const std::string& url,
                            std::string& error) {
  CefWindowInfo windowInfo;
  // The whole browser is offscreen; there is no parent view.
  // kNullWindowHandle, not nullptr: cef_window_handle_t is a pointer on macOS
  // and Windows but an X11 Window id (unsigned long) on Linux, where nullptr
  // does not convert. CEF defines the right zero for each platform.
  windowInfo.SetAsWindowless(kNullWindowHandle);
  windowInfo.windowless_rendering_enabled = true;
  // Painting on request rather than on Chromium's timer. This is what makes one
  // engine tick produce exactly one paint. Fixed for the browser's lifetime,
  // which is why changing pacing has to build a new one.
  windowInfo.external_begin_frame_enabled =
      pacing == BrowserSource::Pacing::kExternalBeginFrame;
  windowInfo.shared_texture_enabled = false;  // we want CPU pixels, not a texture

  CefBrowserSettings browserSettings;
  // Only consulted under internal-timer pacing. 60 is CEF's ceiling.
  browserSettings.windowless_frame_rate = 60;
  // A transparent background so a page with no <body> colour keys correctly on
  // an alpha-capable output. Opaque outputs composite it over black.
  browserSettings.background_color = CefColorSetARGB(0, 0, 0, 0);

  CefRefPtr<CefRequestContext> context = CefRequestContext::GetGlobalContext();
  if (!CefBrowserHost::CreateBrowser(windowInfo, client, url, browserSettings,
                                     nullptr, context)) {
    error = "CefBrowserHost::CreateBrowser failed";
    return false;
  }
  return true;
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
  if (!createOffscreenBrowser(client_, pacing_.load(), url, error)) {
    return false;
  }
  diag::info("browser: opening %s at %s", url.c_str(),
             format_.toString().c_str());
  return true;
}

void BrowserSource::setPacing(Pacing pacing) {
  if (pacing_.exchange(pacing) == pacing) {
    return;
  }
  auto existing = client_->browser();
  if (existing == nullptr) {
    // Not open yet; open() will use the new value.
    return;
  }

  // A live browser cannot be switched: external_begin_frame_enabled is part of
  // CefWindowInfo and only read when the browser is created. Setting the field
  // alone used to look like it worked and then produced no frames at all —
  // requestFrame() stopped asking, and a browser created with external begin
  // frame does not paint on its own. So the browser is rebuilt at the same URL.
  //
  // The old and new browsers overlap for a moment. That is safe because
  // RenderClient::OnBeforeClose ignores any browser that is not the one it is
  // currently rendering.
  const std::string current = url();
  diag::info("browser: pacing now %s — reopening %s",
             pacing == Pacing::kExternalBeginFrame ? "external" : "internal",
             current.c_str());

  // Captures only refcounted handles and values, never `this`: the task can
  // outlive nothing here, but a dangling BrowserSource would be a crash on
  // shutdown.
  CefRefPtr<RenderClient> client = client_;
  onUiThread([client, existing, pacing, current]() {
    existing->GetHost()->CloseBrowser(true);
    std::string error;
    if (!createOffscreenBrowser(client, pacing, current, error)) {
      diag::error("browser: could not reopen after a pacing change: %s",
                  error.c_str());
    }
  });
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
  if (pacing_.load() != Pacing::kExternalBeginFrame) {
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

void BrowserSource::sendInput(const InputEvent& event, int x, int y) {
  auto browser = client_->browser();
  if (browser == nullptr) {
    return;
  }

  CefMouseEvent mouse;
  mouse.x = x;
  mouse.y = y;
  mouse.modifiers = event.modifiers;

  switch (event.type) {
    case InputEvent::Type::kMove: {
      const bool leaving = event.leaving;
      onUiThread([browser, mouse, leaving]() {
        browser->GetHost()->SendMouseMoveEvent(mouse, leaving);
      });
      break;
    }
    case InputEvent::Type::kButton: {
      cef_mouse_button_type_t button = MBT_LEFT;
      if (event.button == InputEvent::Button::kMiddle) {
        button = MBT_MIDDLE;
      } else if (event.button == InputEvent::Button::kRight) {
        button = MBT_RIGHT;
      }
      const bool up = !event.down;
      const int clicks = event.clickCount;
      onUiThread([browser, mouse, button, up, clicks]() {
        browser->GetHost()->SendMouseClickEvent(mouse, button, up, clicks);
      });
      break;
    }
    case InputEvent::Type::kWheel: {
      const int dx = event.deltaX;
      const int dy = event.deltaY;
      onUiThread([browser, mouse, dx, dy]() {
        browser->GetHost()->SendMouseWheelEvent(mouse, dx, dy);
      });
      break;
    }
    case InputEvent::Type::kKey: {
      CefKeyEvent key;
      switch (event.keyAction) {
        case InputEvent::KeyAction::kKeyDown: key.type = KEYEVENT_KEYDOWN; break;
        case InputEvent::KeyAction::kKeyUp:   key.type = KEYEVENT_KEYUP;   break;
        case InputEvent::KeyAction::kChar:    key.type = KEYEVENT_CHAR;    break;
        case InputEvent::KeyAction::kRawKeyDown:
        default:                              key.type = KEYEVENT_RAWKEYDOWN; break;
      }
      key.modifiers = event.modifiers;
      key.windows_key_code = event.keyCode;
      // Chromium tolerates a zero native code on this path; supplying a wrong
      // platform scancode would be worse than supplying none.
      key.native_key_code = 0;
      key.is_system_key = 0;
      key.character = static_cast<char16_t>(event.character);
      key.unmodified_character = static_cast<char16_t>(event.character);
      key.focus_on_editable_field = 0;
      onUiThread([browser, key]() { browser->GetHost()->SendKeyEvent(key); });
      break;
    }
    case InputEvent::Type::kFocus: {
      const bool focused = event.focused;
      onUiThread([browser, focused]() { browser->GetHost()->SetFocus(focused); });
      break;
    }
  }
}

void BrowserSource::setFocused(bool focused) {
  auto browser = client_->browser();
  if (browser == nullptr) {
    return;
  }
  onUiThread([browser, focused]() { browser->GetHost()->SetFocus(focused); });
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
