#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include "include/cef_browser.h"

#include "browser/render_client.h"
#include "core/audio_fifo.h"
#include "core/frame_ring.h"
#include "core/video_format.h"
#include "engine/input_event.h"

namespace weblinked {

/// A URL rendered offscreen into frames and audio.
///
/// Pacing is the interesting decision. Two options exist in CEF:
///
///   * windowless_frame_rate — Chromium paints on its own timer at up to 60 Hz.
///     Simple, but the timer has no relationship to our frame clock, so at 50p
///     you get an irregular mixture of repeated and dropped paints.
///
///   * external_begin_frame_enabled — Chromium paints only when we ask, via
///     SendExternalBeginFrame(). One request per tick means one paint per tick,
///     which is what an SDI output needs.
///
/// The second is the default here. The first stays available (`setPacing`)
/// because external begin frame is the less-travelled path in CEF and a page
/// that misbehaves under it should not leave an operator stuck.
///
/// All CEF interaction must happen on the CEF UI thread. The methods below can
/// be called from any thread and post to it as needed.
class BrowserSource {
 public:
  enum class Pacing {
    /// We drive every frame. Deterministic; the default.
    kExternalBeginFrame,
    /// Chromium's own timer drives frames; we take the latest.
    kInternalTimer,
  };

  BrowserSource(VideoFormat format, LatestFrameSlot* slot, AudioFifo* audio);
  ~BrowserSource();

  /// Creates the browser and starts loading `url`. Must be called after
  /// CefInitialize, on the UI thread (or before the message loop starts).
  bool open(const std::string& url, std::string& error);

  /// Tears the browser down. Safe to call more than once.
  void close();

  /// Navigates. Thread-safe.
  void loadUrl(const std::string& url);

  /// Reloads, optionally ignoring the HTTP cache — which is what an operator
  /// wants when a designer has just re-uploaded a graphic.
  void reload(bool ignoreCache);

  /// Runs JavaScript in the page. The lever that makes a static page dynamic
  /// without a bespoke integration: a lower-third can be driven by calling a
  /// function the page already defines.
  void executeJavaScript(const std::string& script);

  /// Changes the raster. Triggers a CEF resize and a fresh frame pool.
  void setFormat(const VideoFormat& format);

  /// Changes who drives frames. Rebuilds the browser at the current URL if one
  /// is already open, because the mode is fixed when the browser is created.
  void setPacing(Pacing pacing);
  Pacing pacing() const { return pacing_.load(); }

  /// What happens when the page opens a new tab or window. Thread-safe; see
  /// RenderClient::PopupPolicy for why "let it" is not one of the options.
  void setPopupPolicy(RenderClient::PopupPolicy policy) {
    client_->setPopupPolicy(policy);
  }
  RenderClient::PopupPolicy popupPolicy() const {
    return client_->popupPolicy();
  }

  /// Asks the browser for one frame. Called once per engine tick; a no-op under
  /// kInternalTimer pacing.
  void requestFrame();

  /// Forwards a pointer or keyboard event to the page.
  ///
  /// `x` and `y` are in raster pixels; the caller has already scaled them. This
  /// is what makes the control page's preview interactive — enough to dismiss a
  /// cookie banner, close a modal or sign in, which is otherwise impossible on
  /// a page you can only see.
  void sendInput(const InputEvent& event, int x, int y);

  /// An offscreen browser has no window manager to give it focus, so keyboard
  /// input goes nowhere until it is told it has focus.
  void setFocused(bool focused);

  /// Mutes the page's audio at the source. Cheaper than zeroing samples
  /// downstream and it stops the FIFO filling with silence.
  void setAudioMuted(bool muted);

  std::string url() const;
  RenderClient::Diagnostics diagnostics() const;
  bool isOpen() const;

 private:
  CefRefPtr<RenderClient> client_;
  VideoFormat format_;
  std::string url_;
  mutable std::mutex mutex_;
  /// Atomic: the clock thread reads it in requestFrame() on every tick, and the
  /// control surface can change it.
  std::atomic<Pacing> pacing_{Pacing::kExternalBeginFrame};
};

}  // namespace weblinked
