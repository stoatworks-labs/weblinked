#include "browser/render_client.h"

#include <algorithm>
#include <cstring>

#include "core/pixel_convert.h"
#include "diag/diag.h"

namespace weblinked {

RenderClient::RenderClient(VideoFormat format, LatestFrameSlot* slot,
                           AudioFifo* audio)
    : format_(format),
      slot_(slot),
      audio_(audio),
      // Frames leave here as BGRA, exactly as Chromium paints them; the engine
      // converts once per tick for whichever outputs want YUV.
      pool_(FramePool::create(format, PixelFormat::kBGRA)) {
  diagnostics_.url.clear();
}

void RenderClient::setBrowserReadyCallback(std::function<void()> callback) {
  std::lock_guard<std::mutex> lock(mutex_);
  readyCallback_ = std::move(callback);
}

void RenderClient::setFormat(const VideoFormat& format) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (format_ == format) {
    return;
  }
  format_ = format;
  // A new pool rather than a resized one: frames already in flight belong to the
  // old shape and must stay valid until whoever holds them is finished.
  pool_ = FramePool::create(format, PixelFormat::kBGRA);
}

VideoFormat RenderClient::format() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return format_;
}

CefRefPtr<CefBrowser> RenderClient::browser() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return browser_;
}

RenderClient::Diagnostics RenderClient::diagnostics() const {
  std::lock_guard<std::mutex> lock(mutex_);
  Diagnostics out = diagnostics_;
  out.paints = paints_.load(std::memory_order_relaxed);
  out.audioPackets = audioPackets_.load(std::memory_order_relaxed);
  out.consoleErrors = consoleErrors_.load(std::memory_order_relaxed);
  out.popups = popups_.load(std::memory_order_relaxed);
  out.loading = loading_.load(std::memory_order_relaxed);
  return out;
}

void RenderClient::GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) {
  (void)browser;
  std::lock_guard<std::mutex> lock(mutex_);
  rect.x = 0;
  rect.y = 0;
  rect.width = format_.width;
  rect.height = format_.height;
}

bool RenderClient::GetScreenInfo(CefRefPtr<CefBrowser> browser,
                                 CefScreenInfo& info) {
  (void)browser;
  std::lock_guard<std::mutex> lock(mutex_);
  // Reporting a device scale factor of 1 and a screen the same size as the view
  // keeps CSS pixels equal to output pixels. Without this, a page using vh/vw or
  // media queries lays out for the host's actual display, which on a Retina Mac
  // means everything arrives at half size.
  info.device_scale_factor = 1.0f;
  info.depth = 32;
  info.depth_per_component = 8;
  info.is_monochrome = false;
  info.rect = CefRect(0, 0, format_.width, format_.height);
  info.available_rect = info.rect;
  return true;
}

void RenderClient::OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
                           const RectList& dirtyRects, const void* buffer,
                           int width, int height) {
  (void)browser;
  (void)dirtyRects;

  // PET_POPUP is a dropdown or select box rendered separately. Compositing it
  // properly means tracking the popup rect and alpha-blending; for a broadcast
  // output nobody is clicking a select box, so it is dropped rather than
  // half-implemented.
  if (type != PET_VIEW || buffer == nullptr) {
    return;
  }

  VideoFramePtr frame;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    // A paint that arrives mid-resize is for the old raster; CEF will repaint at
    // the new size immediately afterwards, so dropping this one costs one frame.
    if (width != format_.width || height != format_.height) {
      return;
    }
    frame = pool_->acquire();
  }
  if (!frame) {
    return;
  }

  // CEF's buffer is tightly packed BGRA, top row first — the same order every
  // output wants, so this is a straight copy rather than a conversion.
  const int srcStride = width * 4;
  copyBgra(static_cast<const uint8_t*>(buffer), srcStride, frame->data(),
           frame->rowBytes(), width, height, /*flipVertically=*/false);

  frame->setSequence(sequence_.fetch_add(1, std::memory_order_relaxed));
  slot_->publish(std::move(frame));
  paints_.fetch_add(1, std::memory_order_relaxed);
}

bool RenderClient::GetAudioParameters(CefRefPtr<CefBrowser> browser,
                                      CefAudioParameters& params) {
  (void)browser;
  // 48 kHz stereo is what every destination downstream wants. Asking Chromium to
  // resample once here is cheaper and more predictable than resampling per
  // output, and it means the FIFO never has to change rate mid-show.
  params.channel_layout = CEF_CHANNEL_LAYOUT_STEREO;
  params.sample_rate = 48000;
  // Frames per packet. 480 is 10 ms at 48 kHz: short enough that the FIFO stays
  // shallow, long enough not to wake the audio thread pointlessly often.
  params.frames_per_buffer = 480;
  return true;
}

void RenderClient::OnAudioStreamStarted(CefRefPtr<CefBrowser> browser,
                                        const CefAudioParameters& params,
                                        int channels) {
  (void)browser;
  // Half a second of buffer. Deep enough to ride out a scheduling hiccup on
  // either side, shallow enough that recovering from one does not mean playing
  // out seconds of stale audio.
  const int capacity = std::max(params.sample_rate / 2, 4800);
  audio_->configure(channels, params.sample_rate, capacity);

  std::lock_guard<std::mutex> lock(mutex_);
  diag::info("browser: audio stream started, %d channels at %d Hz", channels,
             params.sample_rate);
}

void RenderClient::OnAudioStreamPacket(CefRefPtr<CefBrowser> browser,
                                       const float** data, int frames,
                                       int64_t pts) {
  (void)browser;
  (void)pts;  // the engine pairs audio with video by tick, not by timestamp
  if (data == nullptr || frames <= 0) {
    return;
  }
  audio_->write(data, frames);
  audioPackets_.fetch_add(1, std::memory_order_relaxed);
}

void RenderClient::OnAudioStreamStopped(CefRefPtr<CefBrowser> browser) {
  (void)browser;
  // Leave the FIFO configured: the stream stops whenever the page has no
  // audible source, and tearing the buffer down would mean reconfiguring it
  // every time a video element pauses.
  audio_->reset();
  diag::info("browser: audio stream stopped");
}

void RenderClient::OnAudioStreamError(CefRefPtr<CefBrowser> browser,
                                      const CefString& message) {
  (void)browser;
  std::lock_guard<std::mutex> lock(mutex_);
  diagnostics_.lastError = "audio: " + message.ToString();
  diag::warn("browser: audio stream error: %s", message.ToString().c_str());
}

bool RenderClient::OnBeforePopup(
    CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame, int popupId,
    const CefString& targetUrl, const CefString& targetFrameName,
    WindowOpenDisposition targetDisposition, bool userGesture,
    const CefPopupFeatures& popupFeatures, CefWindowInfo& windowInfo,
    CefRefPtr<CefClient>& client, CefBrowserSettings& settings,
    CefRefPtr<CefDictionaryValue>& extraInfo, bool* noJavascriptAccess) {
  (void)frame;
  (void)popupId;
  (void)targetFrameName;
  (void)targetDisposition;
  (void)userGesture;
  (void)popupFeatures;
  (void)windowInfo;
  (void)client;
  (void)settings;
  (void)extraInfo;
  (void)noJavascriptAccess;

  // Returning false here — CEF's default — is what took the application down.
  //
  // A `target="_blank"` link, or any window.open, asks CEF for a second
  // browser. The default path gives it a *windowed* one, parented to this
  // browser, which is windowless: there is no window to parent it to, and the
  // popup arrives at this same client, whose render handler answers for the
  // offscreen raster. Two further things then went wrong on top of that:
  // OnAfterCreated rebound browser_ to the popup, so the engine's frame
  // requests, navigation and input all went to a browser nothing was reading,
  // and OnBeforeClose cleared browser_ when that popup closed — leaving the
  // programme output pointed at nothing.
  //
  // So the popup is always cancelled, and the URL it wanted is handled here
  // instead. Found by clicking an ordinary link in the interactive preview.
  const std::string url = targetUrl.ToString();
  popups_.fetch_add(1, std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(mutex_);
    diagnostics_.lastPopupUrl = url;
  }

  if (popupPolicy_.load() == PopupPolicy::kBlock || url.empty()) {
    diag::info("browser: blocked a popup to %s",
               url.empty() ? "(no url)" : url.c_str());
    return true;
  }

  diag::info("browser: popup to %s redirected into the main frame", url.c_str());
  // Already on the UI thread, and the browser passed in is the opener — which
  // is the one to navigate, not browser_, in case they ever differ.
  if (browser != nullptr && browser->GetMainFrame() != nullptr) {
    browser->GetMainFrame()->LoadURL(url);
  }
  return true;
}

void RenderClient::OnAfterCreated(CefRefPtr<CefBrowser> browser) {
  // A popup should never reach here now that OnBeforePopup cancels them all,
  // but binding browser_ to one would silently redirect the whole pipeline, so
  // the guard stays as a cheap backstop.
  if (browser != nullptr && browser->IsPopup()) {
    diag::warn("browser: ignoring an unexpected popup browser");
    return;
  }

  std::function<void()> callback;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    browser_ = browser;
    callback = readyCallback_;
  }
  if (callback) {
    callback();
  }
}

void RenderClient::OnBeforeClose(CefRefPtr<CefBrowser> browser) {
  std::lock_guard<std::mutex> lock(mutex_);
  // Only the browser we are actually rendering clears the reference. Anything
  // else closing must not take the source down with it.
  if (browser != nullptr && browser_ != nullptr &&
      !browser->IsSame(browser_)) {
    return;
  }
  browser_ = nullptr;
}

void RenderClient::OnLoadingStateChange(CefRefPtr<CefBrowser> browser,
                                        bool isLoading, bool canGoBack,
                                        bool canGoForward) {
  (void)canGoBack;
  (void)canGoForward;
  loading_.store(isLoading, std::memory_order_relaxed);
  if (!isLoading && browser != nullptr && browser->GetMainFrame() != nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    diagnostics_.url = browser->GetMainFrame()->GetURL().ToString();
  }
}

void RenderClient::OnLoadError(CefRefPtr<CefBrowser> browser,
                               CefRefPtr<CefFrame> frame, ErrorCode errorCode,
                               const CefString& errorText,
                               const CefString& failedUrl) {
  (void)browser;
  // Sub-frame failures are noise; a failed main frame means black output.
  if (frame != nullptr && !frame->IsMain()) {
    return;
  }
  if (errorCode == ERR_ABORTED) {
    return;  // a navigation we replaced ourselves
  }
  std::lock_guard<std::mutex> lock(mutex_);
  diagnostics_.lastError = errorText.ToString() + " (" + failedUrl.ToString() + ")";
  diag::error("browser: load failed: %s", diagnostics_.lastError.c_str());
}

bool RenderClient::OnConsoleMessage(CefRefPtr<CefBrowser> browser,
                                    cef_log_severity_t level,
                                    const CefString& message,
                                    const CefString& source, int line) {
  (void)browser;
  if (level >= LOGSEVERITY_ERROR) {
    consoleErrors_.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(mutex_);
    diagnostics_.lastConsoleError = message.ToString();
    // Worth logging: a page whose own assets 404 looks identical to a page that
    // simply renders nothing, and this is the only place it says so.
    diag::warn("page console: %s (%s:%d)", message.ToString().c_str(),
               source.ToString().c_str(), line);
  }
  return false;  // let CEF log it too
}

}  // namespace weblinked
