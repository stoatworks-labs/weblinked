#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>

namespace weblinked {

/// The queue control changes to one source wait in.
///
/// Changes (add, remove, update, enable, format) take turns on a timed mutex,
/// waiting up to `wait` for the one in front, because a format change or a
/// settings apply across several cards legitimately takes seconds. But each
/// waiter is an HTTP thread, and there are only 64 of those (HttpServer): with
/// a driver wedged inside the change at the front, an operator pressing a
/// button repeatedly would park a thread per press for the whole wait, fill
/// the server and turn /api/state away with a 503. So a waiter gives up at
/// once:
///
/// - if `maxWaiting` others are already waiting — a queue that long is not
///   going anywhere soon, and every waiter costs a connection; or
/// - as soon as `holderStuck()` says the change at the front is inside a call
///   that has stopped answering (in the engine: an output gate that
///   CallGate::wedged() reports), checked every `poll`.
///
/// Header-only and CEF-free, so the tests can drive it.
class ControlQueue {
 public:
  using Clock = std::chrono::steady_clock;
  enum class Result { kHeld, kTimedOut, kTooMany, kHolderStuck };

  ControlQueue(Clock::duration wait, int maxWaiting,
               Clock::duration poll = std::chrono::milliseconds(100))
      : wait_(wait), maxWaiting_(maxWaiting), poll_(poll) {}

  /// Locks `lock` (deferred, on the queue's mutex) or says why not.
  Result enter(std::unique_lock<std::timed_mutex>& lock,
               const std::function<bool()>& holderStuck) {
    if (lock.try_lock()) {
      return Result::kHeld;
    }
    if (waiting_.fetch_add(1) >= maxWaiting_) {
      waiting_.fetch_sub(1);
      return Result::kTooMany;
    }
    struct Leave {
      std::atomic<int>& count;
      ~Leave() { count.fetch_sub(1); }
    } leave{waiting_};

    const auto deadline = Clock::now() + wait_;
    while (true) {
      const auto now = Clock::now();
      if (now >= deadline) {
        return Result::kTimedOut;
      }
      if (holderStuck && holderStuck()) {
        return Result::kHolderStuck;
      }
      const auto step = deadline - now < poll_ ? deadline - now : poll_;
      if (lock.try_lock_for(step)) {
        return Result::kHeld;
      }
    }
  }

  std::timed_mutex& mutex() { return mutex_; }
  int waiting() const { return waiting_.load(); }

 private:
  std::timed_mutex mutex_;
  std::atomic<int> waiting_{0};
  const Clock::duration wait_;
  const int maxWaiting_;
  const Clock::duration poll_;
};

}  // namespace weblinked
