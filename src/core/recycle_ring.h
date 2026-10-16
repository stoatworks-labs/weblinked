#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace weblinked {

/// A fixed set of objects, handed out and given back, never created or
/// destroyed while in use.
///
/// Built for the DeckLink output. Creating a frame per tick and letting the
/// completion thread destroy it meant the SDK's buffer allocator was being
/// emptied and refilled on two threads at once, and on Desktop Video 16.4 that
/// deadlocks inside the SDK (issue #12). A ring of frames created once at start,
/// written only between the card handing one back and it being scheduled again,
/// keeps the allocator untouched for the whole run. The ring is generic so the
/// tests can drive it without a card.
///
/// Thread-safe. acquire() runs on the clock thread and release() on the
/// driver's completion thread; neither holds the lock for longer than it takes
/// to flip a flag, and neither ever calls out while holding it. acquire() never
/// waits for an object to come back — when none is free it returns null and
/// counts a miss, so the caller drops the frame rather than stalling every other
/// output on the same clock.
///
/// The ring owns nothing. The caller creates the objects, add()s them, and gets
/// every one of them back from close() to destroy — after it has made sure
/// nothing else still holds one.
template <typename T>
class RecycleRing {
 public:
  /// Adds an object, free. For setup, before anything is handed out.
  void add(T* item) {
    std::lock_guard<std::mutex> lock(mutex_);
    slots_.push_back(Slot{item, true});
  }

  /// A free object, now in use, or null if every one is out (a miss) or the
  /// ring is closed.
  T* acquire() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return nullptr;
    }
    for (auto& slot : slots_) {
      if (slot.free) {
        slot.free = false;
        return slot.item;
      }
    }
    ++misses_;
    return nullptr;
  }

  /// Gives an object back. False, and nothing changes, for an object that is
  /// not one of ours, one that is already free, or any object once the ring is
  /// closed: a completion arriving twice, or after a stop, must not hand the
  /// same frame to two writers.
  ///
  /// Matched by address, because the SDK hands a completed frame back through a
  /// base interface pointer rather than the type that was added.
  bool release(const void* item) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
      return false;
    }
    for (auto& slot : slots_) {
      if (static_cast<const void*>(slot.item) == item) {
        if (slot.free) {
          ++doubleReleases_;
          return false;
        }
        slot.free = true;
        return true;
      }
    }
    ++strangers_;
    return false;
  }

  /// Stops handing anything out and returns every object, free or not, for the
  /// owner to destroy. `outstanding` is set to how many were still out — on a
  /// clean stop, after the driver has flushed its queue, that is zero.
  std::vector<T*> close(size_t* outstanding = nullptr) {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    std::vector<T*> items;
    items.reserve(slots_.size());
    size_t out = 0;
    for (const auto& slot : slots_) {
      items.push_back(slot.item);
      if (!slot.free) {
        ++out;
      }
    }
    slots_.clear();
    if (outstanding != nullptr) {
      *outstanding = out;
    }
    return items;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return slots_.size();
  }

  size_t freeCount() const {
    std::lock_guard<std::mutex> lock(mutex_);
    size_t count = 0;
    for (const auto& slot : slots_) {
      if (slot.free) {
        ++count;
      }
    }
    return count;
  }

  /// acquire() calls that found nothing free.
  int64_t misses() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return misses_;
  }

  /// release() calls for an object already free, and for one never added.
  /// Either is a bug somewhere; both are refused rather than believed.
  int64_t doubleReleases() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return doubleReleases_;
  }
  int64_t strangers() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return strangers_;
  }

 private:
  struct Slot {
    T* item;
    bool free;
  };

  mutable std::mutex mutex_;
  std::vector<Slot> slots_;
  bool closed_ = false;
  int64_t misses_ = 0;
  int64_t doubleReleases_ = 0;
  int64_t strangers_ = 0;
};

}  // namespace weblinked
