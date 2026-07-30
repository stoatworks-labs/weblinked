#include "core/frame.h"

namespace weblinked {

VideoFrame::VideoFrame(VideoFormat format, PixelFormat pixelFormat)
    : format_(format),
      pixelFormat_(pixelFormat),
      rowBytes_(format.rowBytes(pixelFormat)),
      data_(format.bufferSize(pixelFormat)) {}

FramePool::FramePool(VideoFormat format, PixelFormat pixelFormat, size_t maxRetained)
    : format_(format), pixelFormat_(pixelFormat), maxRetained_(maxRetained) {}

/// Exists only so create() can reach the private constructor without
/// make_shared needing to be a friend.
class FramePoolAccess {
 public:
  static std::shared_ptr<FramePool> make(VideoFormat format,
                                         PixelFormat pixelFormat,
                                         size_t maxRetained) {
    return std::shared_ptr<FramePool>(
        new FramePool(format, pixelFormat, maxRetained));
  }
};

std::shared_ptr<FramePool> FramePool::create(VideoFormat format,
                                             PixelFormat pixelFormat,
                                             size_t maxRetained) {
  return FramePoolAccess::make(format, pixelFormat, maxRetained);
}

VideoFramePtr FramePool::acquire() {
  std::unique_ptr<VideoFrame> frame;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!free_.empty()) {
      frame = std::move(free_.back());
      free_.pop_back();
    }
  }
  if (!frame) {
    frame = std::make_unique<VideoFrame>(format_, pixelFormat_);
  }

  // The deleter returns the buffer to the pool instead of freeing it — but only
  // if the pool still exists. A weak reference rather than a strong one, because
  // a strong one would keep every retired pool alive as long as any frame from
  // it survived, and a raw pointer would dangle.
  std::weak_ptr<FramePool> weak = weak_from_this();
  VideoFrame* raw = frame.release();
  return VideoFramePtr(raw, [weak](VideoFrame* f) {
    if (auto pool = weak.lock()) {
      pool->recycle(std::unique_ptr<VideoFrame>(f));
    } else {
      delete f;
    }
  });
}

void FramePool::recycle(std::unique_ptr<VideoFrame> frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  if (free_.size() >= maxRetained_) {
    return;  // let it go; the pool is already big enough
  }
  free_.push_back(std::move(frame));
}

size_t FramePool::retainedCount() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return free_.size();
}

}  // namespace weblinked
