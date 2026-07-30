#pragma once

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

#include "core/frame.h"

namespace weblinked {

/// A single-slot handoff holding only the most recent frame.
///
/// This is how the browser's paint thread reaches the engine's clock thread.
/// A queue would be wrong here: if the browser paints twice between ticks, the
/// older paint is stale by definition and showing it would add a frame of
/// latency for no benefit. Overwrites are counted so the control API can show
/// that the browser is outrunning the output, or vice versa.
class LatestFrameSlot {
 public:
  void publish(VideoFramePtr frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (frame_) {
      ++overwritten_;
    }
    frame_ = std::move(frame);
    ++published_;
  }

  /// Takes the frame if one is waiting, leaving the slot empty.
  VideoFramePtr take() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(frame_);
  }

  /// Reads the frame without clearing it, so a tick with no new paint can
  /// repeat the last one — which is what an output needs when a page is static.
  VideoFramePtr peek() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return frame_;
  }

  int64_t publishedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return published_;
  }

  int64_t overwrittenCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return overwritten_;
  }

 private:
  mutable std::mutex mutex_;
  VideoFramePtr frame_;
  int64_t published_ = 0;
  int64_t overwritten_ = 0;
};

/// A bounded frame queue that drops the oldest entry when full.
///
/// Used by outputs that schedule ahead of themselves (DeckLink pre-rolls a few
/// frames before starting playback). Dropping the oldest rather than refusing
/// the newest keeps an output that briefly stalls from falling permanently
/// behind.
class FrameRing {
 public:
  explicit FrameRing(size_t capacity) : capacity_(capacity ? capacity : 1) {}

  bool push(VideoFramePtr frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    bool dropped = false;
    while (queue_.size() >= capacity_) {
      queue_.pop_front();
      ++droppedCount_;
      dropped = true;
    }
    queue_.push_back(std::move(frame));
    return !dropped;
  }

  VideoFramePtr pop() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.empty()) {
      return nullptr;
    }
    VideoFramePtr frame = std::move(queue_.front());
    queue_.pop_front();
    return frame;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.size();
  }

  int64_t droppedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return droppedCount_;
  }

  void clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.clear();
  }

 private:
  mutable std::mutex mutex_;
  size_t capacity_;
  std::deque<VideoFramePtr> queue_;
  int64_t droppedCount_ = 0;
};

}  // namespace weblinked
