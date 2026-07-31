#pragma once

#include <cstdint>
#include <mutex>

namespace weblinked {

/// Slot bookkeeping for a screen backend's frame ring.
///
/// Only the indices live here — the storage itself is the backend's, because
/// what a slot *is* differs per platform: a shared-storage MTLBuffer with a
/// texture aliased onto it on macOS, a plain staging buffer under D3D11 and GL.
/// The part worth writing once is this, because it is the part that is easy to
/// get subtly wrong in three places.
///
/// The problem it solves: the engine's clock thread writes frames at the video
/// rate while the platform's display-refresh callback reads them at the
/// monitor's rate, and neither may block the other. Three slots is the smallest
/// number that guarantees the writer always finds a free one — the reader can
/// hold at most one, at most one is published and waiting, which still leaves a
/// third to write into.
///
/// Every method takes the lock for a few index comparisons only. No copying and
/// no I/O happens inside it: the caller claims a slot, fills it with the lock
/// released, and then publishes.
class ScreenFrameRing {
 public:
  static constexpr int kSlots = 3;

  /// A slot to fill, or -1 when the ring is closed. Fill it *outside* any lock,
  /// then call publish().
  int claim() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_) {
      return -1;
    }
    for (int i = 0; i < kSlots; ++i) {
      if (i != newest_ && i != inUse_ && i != writing_) {
        writing_ = i;
        return i;
      }
    }
    return -1;
  }

  /// Makes a filled slot the one the next refresh will draw.
  void publish(int slot) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!consumed_) {
      // The frame this replaces never reached a refresh. Entirely normal when
      // the page runs faster than the display — a 50 Hz source on a 30 Hz head
      // drops by design — so it is counted rather than warned about.
      ++dropped_;
    }
    newest_ = slot;
    writing_ = -1;
    consumed_ = false;
  }

  /// The slot to draw, or -1 when nothing has been published yet. Must be
  /// followed by release() once the draw has been encoded.
  int acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!open_ || newest_ < 0) {
      return -1;
    }
    inUse_ = newest_;
    consumed_ = true;
    return inUse_;
  }

  /// `presented` is false when the draw was abandoned — no drawable available,
  /// a lost device — so the counter reflects frames that reached the glass.
  void release(bool presented) {
    std::lock_guard<std::mutex> lock(mutex_);
    inUse_ = -1;
    if (presented) {
      ++presented_;
    }
  }

  /// Closes the ring so claim() and acquire() stop handing out slots. Called
  /// before a backend tears its GPU objects down.
  void shutdown() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = false;
    newest_ = -1;
    inUse_ = -1;
    writing_ = -1;
    consumed_ = true;
  }

  void reset() {
    std::lock_guard<std::mutex> lock(mutex_);
    open_ = true;
    newest_ = -1;
    inUse_ = -1;
    writing_ = -1;
    consumed_ = true;
    presented_ = 0;
    dropped_ = 0;
  }

  int64_t presentedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return presented_;
  }

  int64_t droppedCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }

 private:
  mutable std::mutex mutex_;
  bool open_ = false;
  int newest_ = -1;   ///< published, waiting for a refresh
  int inUse_ = -1;    ///< the reader is drawing this one
  int writing_ = -1;  ///< the writer is filling this one
  bool consumed_ = true;
  int64_t presented_ = 0;
  int64_t dropped_ = 0;
};

}  // namespace weblinked
