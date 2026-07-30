#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>

#include "core/video_format.h"

namespace weblinked {

/// One video frame. Frames are reference-counted and immutable once published,
/// so several outputs can hold the same frame without copying it and without a
/// lock. Buffers come from a FramePool because allocating 8 MB per frame at
/// 50 Hz is a straightforward way to fight the allocator forever.
class VideoFrame {
 public:
  VideoFrame(VideoFormat format, PixelFormat pixelFormat);

  const VideoFormat& format() const { return format_; }
  PixelFormat pixelFormat() const { return pixelFormat_; }
  int rowBytes() const { return rowBytes_; }

  uint8_t* data() { return data_.data(); }
  const uint8_t* data() const { return data_.data(); }
  size_t sizeBytes() const { return data_.size(); }

  /// Frame counter from the source, monotonic per session. Not a wall clock —
  /// outputs derive their own presentation times from the format's rate.
  int64_t sequence() const { return sequence_; }
  void setSequence(int64_t sequence) { sequence_ = sequence; }

  /// Capture time in the clock's own timebase, nanoseconds since the engine
  /// started. Used for the OMT/NDI presentation timestamps and for latency
  /// reporting, never for pacing.
  int64_t captureNanos() const { return captureNanos_; }
  void setCaptureNanos(int64_t nanos) { captureNanos_ = nanos; }

 private:
  VideoFormat format_;
  PixelFormat pixelFormat_;
  int rowBytes_;
  int64_t sequence_ = 0;
  int64_t captureNanos_ = 0;
  std::vector<uint8_t> data_;
};

using VideoFramePtr = std::shared_ptr<VideoFrame>;

/// Recycles frame buffers of one shape. Drop a frame and its buffer returns
/// here; the pool only ever grows to the number of frames genuinely in flight.
///
/// Always held by shared_ptr, via create(). That is not ceremony: a frame's
/// deleter has to reach the pool, and a raster change replaces the pool while
/// frames from the old one are still being held by the clock thread and by
/// whichever outputs have not finished with them. The deleter therefore holds a
/// weak reference and simply frees the buffer if the pool has already gone.
/// Capturing the pool by raw pointer instead is a use-after-free that only
/// appears when someone changes format mid-show.
class FramePool : public std::enable_shared_from_this<FramePool> {
 public:
  static std::shared_ptr<FramePool> create(VideoFormat format,
                                           PixelFormat pixelFormat,
                                           size_t maxRetained = 8);

  /// A frame of the pool's shape, recycled if one is free.
  VideoFramePtr acquire();

  const VideoFormat& format() const { return format_; }
  PixelFormat pixelFormat() const { return pixelFormat_; }

  size_t retainedCount() const;

  FramePool(const FramePool&) = delete;
  FramePool& operator=(const FramePool&) = delete;

 private:
  friend class FramePoolAccess;
  FramePool(VideoFormat format, PixelFormat pixelFormat, size_t maxRetained);

  void recycle(std::unique_ptr<VideoFrame> frame);

  VideoFormat format_;
  PixelFormat pixelFormat_;
  size_t maxRetained_;
  mutable std::mutex mutex_;
  std::vector<std::unique_ptr<VideoFrame>> free_;
};

using FramePoolPtr = std::shared_ptr<FramePool>;

}  // namespace weblinked
