#include "core/frame_clock.h"

#include <thread>

#if defined(_WIN32)
#include <windows.h>
#endif

namespace weblinked {

namespace {
constexpr int64_t kNanosPerSecond = 1'000'000'000LL;

/// Sleeps for `nanos`, as precisely as the platform allows.
///
/// On Windows, std::this_thread::sleep_for rounds up to the system timer
/// resolution — 15.6 ms unless some process has asked for better — so a 20 ms
/// frame sleeps 31 ms and the clock drops ticks. timeBeginPeriod is not the
/// fix: Windows 11 may ignore it for a process with no visible window, which is
/// what WebLinked usually is. A high-resolution waitable timer (Windows 10 1803
/// and later) is not subject to that. Until v1.0.6 this was masked by
/// Chromium's renderer spinning flat out, which raised the resolution for us.
void sleepPrecise(int64_t nanos) {
#if defined(_WIN32)
  thread_local HANDLE timer = CreateWaitableTimerExW(
      nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if (timer != nullptr) {
    LARGE_INTEGER due;
    due.QuadPart = -(nanos / 100);  // Relative, in 100 ns units.
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) &&
        WaitForSingleObject(timer, INFINITE) == WAIT_OBJECT_0) {
      return;
    }
  }
#endif
  std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
}
}  // namespace

FrameClock::FrameClock(FrameRate rate) : rate_(rate) {}

void FrameClock::setRate(FrameRate rate) {
  rate_ = rate;
  // Re-anchor, otherwise every future deadline is computed against a period the
  // elapsed ticks were never paced at.
  start();
}

void FrameClock::start() {
  start_ = Clock::now();
  nextTick_ = 0;
  droppedTicks_ = 0;
  lastLateness_ = 0;
}

int64_t FrameClock::deadlineNanosForTick(int64_t tick) const {
  if (rate_.numerator <= 0) {
    return 0;
  }
  // Exact: no accumulated rounding, and no overflow until ~292 years at 1 ns.
  return (tick * kNanosPerSecond * rate_.denominator) / rate_.numerator;
}

int64_t FrameClock::elapsedNanos() const {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_)
      .count();
}

int64_t FrameClock::waitForNextTick() {
  const int64_t period = deadlineNanosForTick(1);

  int64_t tick = nextTick_;
  int64_t deadline = deadlineNanosForTick(tick);
  int64_t now = elapsedNanos();

  if (period > 0 && now > deadline + period) {
    // More than a whole frame late. Jump to the next deadline that is still in
    // the future and account for what we skipped.
    const int64_t behind = (now - deadline) / period;
    droppedTicks_ += behind;
    tick += behind;
    deadline = deadlineNanosForTick(tick);
  }

  now = elapsedNanos();
  if (deadline > now) {
    sleepPrecise(deadline - now);
  }

  lastLateness_ = elapsedNanos() - deadline;
  nextTick_ = tick + 1;
  return tick;
}

}  // namespace weblinked
