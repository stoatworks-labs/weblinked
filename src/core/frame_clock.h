#pragma once

#include <chrono>
#include <cstdint>

#include "core/video_format.h"

namespace weblinked {

/// The engine's pacing clock.
///
/// Tick deadlines are computed from the start instant as exact rational
/// multiples of the frame period — deadline(n) = start + n * den / num seconds —
/// rather than by repeatedly adding a rounded period. The rounded-period
/// approach drifts by its rounding error times the tick count: at 59.94 that is
/// four frames an hour if the period is held in microseconds, and far worse in
/// milliseconds. Computing each deadline from tick zero accumulates nothing, so
/// a show can run all day. tests/test_frame_clock.cpp quantifies both.
///
/// Nothing here is genlocked. When a DeckLink or AJA card is the output, the
/// card's own scheduled playback is the authority on when a frame reaches SDI;
/// this clock decides only when we *produce* frames, and each card's buffer
/// absorbs the difference. See docs/01-architecture.md for why that boundary is
/// drawn where it is.
class FrameClock {
 public:
  explicit FrameClock(FrameRate rate);

  void setRate(FrameRate rate);
  FrameRate rate() const { return rate_; }

  /// Anchors tick 0 at now.
  void start();

  /// Sleeps until the next tick is due and returns its index.
  ///
  /// If the caller has fallen behind by more than one whole frame, the missed
  /// ticks are abandoned rather than emitted back-to-back: a late frame is
  /// worth less than a correctly paced one, and bursting would only push the
  /// downstream buffers further out of shape. Those ticks are counted in
  /// droppedTicks().
  int64_t waitForNextTick();

  /// Nanoseconds since start(), on the same clock the tick deadlines use.
  int64_t elapsedNanos() const;

  int64_t droppedTicks() const { return droppedTicks_; }
  int64_t tickCount() const { return nextTick_; }

  /// How late the last tick actually fired, in nanoseconds. Reported in the
  /// control API so pacing trouble is visible rather than merely audible.
  int64_t lastLatenessNanos() const { return lastLateness_; }

  /// Nanoseconds after start() at which `tick` is due. Public because outputs
  /// derive presentation timestamps from it, and because it is the part worth
  /// testing without waiting in real time.
  int64_t deadlineNanosForTick(int64_t tick) const;

 private:
  using Clock = std::chrono::steady_clock;

  FrameRate rate_;
  Clock::time_point start_{};
  int64_t nextTick_ = 0;
  int64_t droppedTicks_ = 0;
  int64_t lastLateness_ = 0;
};

}  // namespace weblinked
