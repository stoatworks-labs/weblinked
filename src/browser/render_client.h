#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/cef_client.h"
#include "include/cef_render_handler.h"

#include "core/audio_fifo.h"
#include "core/frame.h"
#include "core/frame_ring.h"
#include "core/video_format.h"

namespace weblinked {

/// The offscreen browser's client: render handler, audio handler, life span and
/// load state, all in one object because CEF hands them out from one interface.
///
/// Threading, which is most of the difficulty here:
///   * OnPaint arrives on the CEF UI thread.
///   * OnAudioStreamPacket arrives on a separate browser audio thread.
///   * The engine's clock thread reads from both.
/// So the frame slot and the audio FIFO are the synchronisation points, and
/// nothing else is shared.
class RenderClient : public CefClient,
                     public CefRenderHandler,
                     public CefAudioHandler,
                     public CefLifeSpanHandler,
                     public CefLoadHandler,
                     public CefDisplayHandler {
 public:
  /// What to do when the page tries to open a new tab or window.
  ///
  /// Never "let it": a windowless browser cannot parent a windowed popup, and
  /// CEF's default is to create one. See OnBeforePopup for the failure that
  /// caused.
  enum class PopupPolicy {
    /// Load the popup's target in this browser instead. The default: an
    /// operator who clicks a `target="_blank"` link in the preview means "go
    /// there", and there is only one raster to go there in.
    kNavigateInPlace,
    /// Drop it and log. For a page whose stray `window.open` calls must never
    /// take the on-air graphic off its own URL.
    kBlock,
  };

  RenderClient(VideoFormat format, LatestFrameSlot* slot, AudioFifo* audio);

  /// Called on the UI thread once the browser exists.
  void setBrowserReadyCallback(std::function<void()> callback);

  void setPopupPolicy(PopupPolicy policy) { popupPolicy_.store(policy); }
  PopupPolicy popupPolicy() const { return popupPolicy_.load(); }

  void setFormat(const VideoFormat& format);
  VideoFormat format() const;

  CefRefPtr<CefBrowser> browser() const;

  // Console messages from the page. Kept because a page that fails to load its
  // own assets says so here and nowhere else.
  struct Diagnostics {
    int64_t paints = 0;
    int64_t audioPackets = 0;
    int64_t consoleErrors = 0;
    int64_t popups = 0;
    bool loading = false;
    int lastHttpStatus = 0;
    std::string lastError;
    std::string lastConsoleError;
    std::string lastPopupUrl;
    std::string url;
  };
  Diagnostics diagnostics() const;

  // CefClient
  CefRefPtr<CefRenderHandler> GetRenderHandler() override { return this; }
  CefRefPtr<CefAudioHandler> GetAudioHandler() override { return this; }
  CefRefPtr<CefLifeSpanHandler> GetLifeSpanHandler() override { return this; }
  CefRefPtr<CefLoadHandler> GetLoadHandler() override { return this; }
  CefRefPtr<CefDisplayHandler> GetDisplayHandler() override { return this; }

  // CefRenderHandler
  void GetViewRect(CefRefPtr<CefBrowser> browser, CefRect& rect) override;
  bool GetScreenInfo(CefRefPtr<CefBrowser> browser, CefScreenInfo& info) override;
  void OnPaint(CefRefPtr<CefBrowser> browser, PaintElementType type,
               const RectList& dirtyRects, const void* buffer, int width,
               int height) override;

  // CefAudioHandler
  bool GetAudioParameters(CefRefPtr<CefBrowser> browser,
                          CefAudioParameters& params) override;
  void OnAudioStreamStarted(CefRefPtr<CefBrowser> browser,
                            const CefAudioParameters& params,
                            int channels) override;
  void OnAudioStreamPacket(CefRefPtr<CefBrowser> browser, const float** data,
                           int frames, int64_t pts) override;
  void OnAudioStreamStopped(CefRefPtr<CefBrowser> browser) override;
  void OnAudioStreamError(CefRefPtr<CefBrowser> browser,
                          const CefString& message) override;

  // CefLifeSpanHandler
  bool OnBeforePopup(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                     int popupId, const CefString& targetUrl,
                     const CefString& targetFrameName,
                     WindowOpenDisposition targetDisposition, bool userGesture,
                     const CefPopupFeatures& popupFeatures,
                     CefWindowInfo& windowInfo, CefRefPtr<CefClient>& client,
                     CefBrowserSettings& settings,
                     CefRefPtr<CefDictionaryValue>& extraInfo,
                     bool* noJavascriptAccess) override;
  void OnAfterCreated(CefRefPtr<CefBrowser> browser) override;
  void OnBeforeClose(CefRefPtr<CefBrowser> browser) override;

  // CefLoadHandler
  void OnLoadingStateChange(CefRefPtr<CefBrowser> browser, bool isLoading,
                            bool canGoBack, bool canGoForward) override;
  void OnLoadError(CefRefPtr<CefBrowser> browser, CefRefPtr<CefFrame> frame,
                   ErrorCode errorCode, const CefString& errorText,
                   const CefString& failedUrl) override;

  // CefDisplayHandler
  bool OnConsoleMessage(CefRefPtr<CefBrowser> browser, cef_log_severity_t level,
                        const CefString& message, const CefString& source,
                        int line) override;

 private:
  IMPLEMENT_REFCOUNTING(RenderClient);

  mutable std::mutex mutex_;
  VideoFormat format_;
  CefRefPtr<CefBrowser> browser_;
  std::function<void()> readyCallback_;

  LatestFrameSlot* slot_;
  AudioFifo* audio_;
  FramePoolPtr pool_;

  std::atomic<int64_t> sequence_{0};
  std::atomic<int64_t> paints_{0};
  std::atomic<int64_t> audioPackets_{0};
  std::atomic<int64_t> consoleErrors_{0};
  std::atomic<int64_t> popups_{0};
  std::atomic<bool> loading_{false};
  std::atomic<PopupPolicy> popupPolicy_{PopupPolicy::kNavigateInPlace};
  Diagnostics diagnostics_;
};

}  // namespace weblinked
