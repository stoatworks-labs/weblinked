#pragma once

#include <atomic>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <vector>

#include "outputs/output.h"

namespace weblinked {

/// The control UI's confidence monitor.
///
/// Modelled as an output rather than bolted onto the engine, so it goes through
/// exactly the same frame path as SDI and NDI. If the preview is wrong, the
/// outputs are wrong — which is the property you want from a monitor.
///
/// It holds a box-downscaled BGRA copy that the HTTP server serves as raw
/// bytes. No JPEG encoder, no WebSocket: the page fetches the buffer a few
/// times a second and blits it into a canvas. At 1/4 scale that is about
/// 500 kB a frame on loopback, which costs nothing and keeps the dependency
/// list empty.
class PreviewOutput final : public IOutput {
 public:
  explicit PreviewOutput(const OutputSpec& spec);

  std::string kind() const override { return "preview"; }
  PixelFormat pixelFormat() const override { return PixelFormat::kBGRA; }
  bool wantsAudio() const override { return true; }

  bool start(const VideoFormat& format, std::string& error) override;
  void stop() override;
  void submit(const VideoFrame& video, const AudioBlock& audio) override;
  json::Value status() const override;

  struct Snapshot {
    int width = 0;
    int height = 0;
    int64_t sequence = 0;
    std::vector<uint8_t> pixels;  // BGRA, tightly packed
  };

  /// A copy of the latest downscaled frame. Copies deliberately: the HTTP
  /// thread must not hold a lock while writing to a socket.
  Snapshot snapshot() const;

 private:
  mutable std::mutex mutex_;
  VideoFormat sourceFormat_;
  int factor_ = 4;
  int width_ = 0;
  int height_ = 0;
  int64_t frames_ = 0;
  int64_t sequence_ = 0;
  std::vector<uint8_t> buffer_;
  std::atomic<float> audioPeak_{0.0f};
};

}  // namespace weblinked
