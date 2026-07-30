#include "core/frame_clock.h"

#include <thread>

namespace weblinked {

namespace {
constexpr int64_t kNanosPerSecond = 1'000'000'000LL;
}

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
    std::this_thread::sleep_for(std::chrono::nanoseconds(deadline - now));
  }

  lastLateness_ = elapsedNanos() - deadline;
  nextTick_ = tick + 1;
  return tick;
}

}  // namespace weblinked
