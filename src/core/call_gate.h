#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <utility>

namespace weblinked {

/// A lock for calls into something that may never return.
///
/// Every call into an output — submit, start, stop, status — goes through that
/// output's gate, and nobody waits for a gate for ever. Whoever cannot have it
/// by their deadline does without: the clock skips that output for a tick, a
/// control request reports it busy, and the state report falls back to the last
/// status that did complete. That is what stops a vendor driver wedged inside
/// one call (issue #12: DeckLink's allocator deadlocking on itself) from taking
/// the engine's own mutex, and with it the whole control API, down too.
///
/// A gate that has been held for longer than `wedgedAfter` is presumed wedged,
/// and later callers try it once without waiting rather than each paying their
/// whole deadline to learn the same thing again.
class CallGate {
 public:
  using Clock = std::chrono::steady_clock;

  explicit CallGate(Clock::duration wedgedAfter = std::chrono::seconds(1))
      : wedgedAfterNanos_(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wedgedAfter)
                .count()) {}

  CallGate(const CallGate&) = delete;
  CallGate& operator=(const CallGate&) = delete;

  /// Runs `fn` holding the gate, if it can be had by `deadline`. Returns false
  /// without running `fn` if not.
  template <typename Fn>
  bool tryRun(Clock::time_point deadline, Fn&& fn) {
    if (wedged()) {
      deadline = Clock::now();
    }
    std::unique_lock<std::timed_mutex> lock(mutex_, std::defer_lock);
    if (!lock.try_lock_until(deadline)) {
      refusals_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    // Cleared on the way out however fn leaves, so a gate is never reported
    // busy after the call that held it has gone.
    struct Busy {
      std::atomic<int64_t>& since;
      explicit Busy(std::atomic<int64_t>& s) : since(s) { since.store(nowNanos()); }
      ~Busy() { since.store(0); }
    } busy(busySince_);
    std::forward<Fn>(fn)();
    return true;
  }

  /// How long the call holding the gate has been running, or 0 if nothing
  /// holds it.
  int64_t busyNanos() const {
    const int64_t since = busySince_.load();
    return since == 0 ? 0 : nowNanos() - since;
  }

  /// How many callers gave up waiting.
  int64_t refusals() const { return refusals_.load(std::memory_order_relaxed); }

  /// Whether the call holding the gate has been running for longer than
  /// `wedgedAfter`. Below that it is merely busy — a submit, a card opening —
  /// and a caller that could not wait should not call it stuck.
  bool wedged() const { return busyNanos() > wedgedAfterNanos_; }

 private:
  static int64_t nowNanos() {
    // Never 0, which means "not busy".
    const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            Clock::now().time_since_epoch())
                            .count();
    return now == 0 ? 1 : now;
  }

  std::timed_mutex mutex_;
  std::atomic<int64_t> busySince_{0};
  std::atomic<int64_t> refusals_{0};
  const int64_t wedgedAfterNanos_;
};

}  // namespace weblinked
