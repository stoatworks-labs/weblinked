#include "core/playout_schedule.h"

#include <algorithm>

namespace weblinked {

PlayoutSchedule::PlayoutSchedule(int64_t frameDuration, int64_t timeScale,
                                 int preroll)
    : frameDuration_(std::max<int64_t>(1, frameDuration)),
      timeScale_(std::max<int64_t>(1, timeScale)),
      preroll_(std::max(1, preroll)) {}

int64_t PlayoutSchedule::reanchor(int64_t playhead, bool lateReported) {
  playhead = std::max<int64_t>(0, playhead);

  // "Ahead" means strictly after the playhead; after a late report it means a
  // whole frame after it.
  const int64_t margin = lateReported ? frameDuration_ : 1;
  if (nextDisplayTime() >= playhead + margin) {
    return 0;
  }

  // First whole frame at or after the playhead. If the playhead sits exactly on
  // a frame boundary that frame is the one on air, and the preroll on top of it
  // is the queue depth the output started with.
  const int64_t onAir = (playhead + frameDuration_ - 1) / frameDuration_;
  const int64_t target = std::max(next_, onAir + preroll_);
  const int64_t skipped = target - next_;
  if (skipped <= 0) {
    return 0;
  }
  next_ = target;
  ++reanchors_;
  skipped_ += skipped;
  return skipped;
}

int64_t PlayoutSchedule::samplePosition(int64_t index, int64_t sampleRate) const {
  // index * frameDuration is a display time, which fits comfortably; it is the
  // further multiply by the sample rate that would overflow after a few weeks
  // at 59.94. Splitting off whole multiples of timeScale keeps every
  // intermediate small and the floor exact: (q*s + r)*R/s = q*R + r*R/s.
  const int64_t time = index * frameDuration_;
  const int64_t whole = time / timeScale_;
  const int64_t part = time % timeScale_;
  return whole * sampleRate + (part * sampleRate) / timeScale_;
}

}  // namespace weblinked
