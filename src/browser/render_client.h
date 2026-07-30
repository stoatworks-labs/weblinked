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
  RenderClient(VideoFormat format, LatestFrameSlot* slot, AudioFifo* audio);

  /// Called on the UI thread once the browser exists.
  void setBrowserReadyCallback(std::function<void()> callback);

  void setFormat(const VideoFormat& format);
  VideoFormat format() const;

  CefRefPtr<CefBrowser> browser() const;

  // Console messages from the page. Kept because a page that fails to load its
  // own assets says so here and nowhere else.
  struct Diagnostics {
    int64_t paints = 0;
    int64_t audioPackets = 0;
    int64_t consoleErrors = 0;
    bool loading = false;
    int lastHttpStatus = 0;
    std::string lastError;
    std::string lastConsoleError;
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
  std::atomic<bool> loading_{false};
  Diagnostics diagnostics_;
};

}  // namespace weblinked
