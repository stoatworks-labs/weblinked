#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>

#include "core/rotated_blit.h"
#include "outputs/output.h"

namespace weblinked {

/// A display driven directly through Linux DRM/KMS, with no X server, no
/// compositor and no GPU.
///
/// The `screen` output wants X11 and EGL. A board running a vendor's embedded
/// userland may have neither — the BirdDog PLAY has a KMS driver and a Mali
/// blob built for Wayland, and that is all — but every Linux display driver
/// offers *dumb buffers*: plain CPU-mapped memory the display controller scans
/// out. This output copies each frame into one with RotatedBlit and flips to
/// it, which is the whole of it.
///
/// Rotation happens in that copy, because it costs nothing extra there: the
/// copy is already one read and one write per pixel, and turning it only
/// changes which source address each write reads from. That is what lets a
/// page rendered at 1080x1920 fill a 1920x1080 panel hung on its side.
///
/// ## Threading
///
/// Everything happens on the engine's clock thread, inside submit(), and none
/// of it waits: completed flips are collected with a zero-timeout poll, and a
/// frame that arrives while the previous flip is still pending is dropped and
/// counted rather than queued. Two buffers, front and back, are therefore
/// enough.
///
/// A frame whose sequence number matches the last one drawn is not copied
/// again. The engine submits every tick whether or not the page painted, and on
/// a static page — which is most signage — skipping those is most of the CPU
/// this output would otherwise use.
///
/// ## DRM master
///
/// Only one process may set modes on a card. start() fails with a message that
/// says so when another one holds it, rather than a bare EACCES.
class KmsOutput final : public IOutput {
 public:
  explicit KmsOutput(const OutputSpec& spec);
  ~KmsOutput() override;

  std::string kind() const override { return "kms"; }
  PixelFormat pixelFormat() const override { return PixelFormat::kBGRA; }
  bool wantsAudio() const override { return false; }
  bool wantsStraightAlpha() const override { return false; }

  bool start(const VideoFormat& format, std::string& error) override;
  void stop() override;
  void submit(const VideoFrame& video, const AudioBlock& audio) override;
  json::Value status() const override;

 private:
  struct Buffer {
    uint32_t handle = 0;
    uint32_t fb = 0;
    uint32_t pitch = 0;
    uint64_t size = 0;
    uint8_t* map = nullptr;
  };

  bool openDevice(std::string& error);
  bool createBuffer(Buffer& buffer, std::string& error);
  void destroyBuffer(Buffer& buffer);
  void collectFlips(int timeoutMs);
  void teardown();

  static void onPageFlip(int fd, unsigned int frame, unsigned int sec,
                         unsigned int usec, void* data);

  // Configuration, fixed at construction.
  std::string card_;
  std::string wantedConnector_;
  std::string wantedMode_;
  BlitRotation rotation_ = BlitRotation::k0;
  BlitFit fit_ = BlitFit::kFit;

  // Device state. Touched by start()/stop() and the clock thread only, which
  // never run at once; status() reads the copies under statusMutex_.
  int fd_ = -1;
  uint32_t connectorId_ = 0;
  uint32_t crtcId_ = 0;
  void* mode_ = nullptr;        ///< drmModeModeInfo, opaque here
  void* savedCrtc_ = nullptr;   ///< drmModeCrtc to restore on stop
  Buffer buffers_[2];
  int front_ = 0;
  bool flipPending_ = false;
  bool flipsSupported_ = true;
  /// The sequence last copied to a buffer. Starts below anything the engine
  /// sends — its black stand-in is -1 — so the very first frame is always drawn.
  static constexpr int64_t kNothingDrawn = INT64_MIN;
  int64_t lastSequence_ = kNothingDrawn;
  RotatedBlit blit_;
  int sourceWidth_ = 0;
  int sourceHeight_ = 0;

  mutable std::mutex statusMutex_;
  std::string connectorName_;
  std::string modeName_;
  int displayWidth_ = 0;
  int displayHeight_ = 0;
  double refreshHz_ = 0;

  std::atomic<int64_t> submitted_{0};
  std::atomic<int64_t> presented_{0};
  std::atomic<int64_t> dropped_{0};
  std::atomic<int64_t> repeats_{0};
  std::atomic<int64_t> copyMicros_{0};  ///< last copy's duration
};

}  // namespace weblinked
