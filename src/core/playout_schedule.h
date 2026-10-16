#pragma once

#include <cstdint>

namespace weblinked {

/// Where a scheduled-playback output puts each frame on the card's timeline, and
/// when that timeline has to be moved forward.
///
/// A DeckLink is handed frames with display times and plays each one when its
/// own playhead — GetScheduledStreamTime — reaches it. Display times come from a
/// frame index, so a tick that is a little late only shortens the queue. That
/// holds while the lateness is less than the queue. A stall longer than the
/// queue (the process stopped, the machine swapping) leaves the index behind the
/// playhead, and since exactly one frame is scheduled per tick nothing ever
/// makes the difference up: every frame from then on is scheduled in the past
/// and played late, for ever. That is issue #13, measured on a real rig as
/// 253,368 late frames out of 269,280 over the hour after one three-second stop.
///
/// So before each frame the output tells this where the card's playhead is, and
/// if the next slot is not ahead of it the index jumps to the playhead plus the
/// preroll — the depth playback started with. The card holds its last frame
/// across the skipped slots, which is what it was doing anyway.
///
/// Units are the card's own: `frameDuration` and `timeScale` are the display
/// mode's GetFrameRate pair, so frame N is displayed at exactly
/// N * frameDuration and nothing is rounded or accumulated. At 59.94 that is
/// 1001 in 60000, where the microsecond time base this replaced drifted a third
/// of a microsecond a frame.
///
/// Pure arithmetic: no SDK, no locking. The output calls it from one thread.
class PlayoutSchedule {
 public:
  PlayoutSchedule() = default;
  PlayoutSchedule(int64_t frameDuration, int64_t timeScale, int preroll);

  int64_t frameDuration() const { return frameDuration_; }
  int64_t timeScale() const { return timeScale_; }
  int preroll() const { return preroll_; }

  /// The index of the next frame to schedule, and its display time in
  /// timeScale units.
  int64_t nextIndex() const { return next_; }
  int64_t nextDisplayTime() const { return next_ * frameDuration_; }

  /// The frame at nextIndex() has been accepted by the card. A frame the card
  /// refused does not advance the index, so its slot is offered again.
  void advance() { ++next_; }

  /// Moves the timeline forward if the next frame would be late.
  ///
  /// `playhead` is the card's scheduled stream time, in timeScale units.
  /// Normally the next slot only has to be ahead of it. When `lateReported` —
  /// the card has completed a frame late or dropped one since the last call —
  /// the next slot has to be a whole frame ahead, because a slot due inside the
  /// frame the card is already sending has no time left to be transferred.
  ///
  /// Moving puts the next frame at the first whole frame at or after the
  /// playhead, plus the preroll, and never earlier than where it already was:
  /// indices only ever increase, so no slot is scheduled twice. Returns how many
  /// slots were skipped, 0 if nothing moved.
  int64_t reanchor(int64_t playhead, bool lateReported);

  /// Back to index 0, for a fresh StartScheduledPlayback. Counters are kept.
  void reset() { next_ = 0; }

  /// How many times reanchor() moved, and how many slots it skipped in total.
  int64_t reanchors() const { return reanchors_; }
  int64_t skippedSlots() const { return skipped_; }

  /// The first audio sample belonging to frame `index` at `sampleRate`:
  /// floor(index * frameDuration * sampleRate / timeScale), exactly, and
  /// without overflowing for a show that runs for years. The difference of two
  /// of these is the audio a re-anchor skips, which keeps sound and picture
  /// moving together.
  int64_t samplePosition(int64_t index, int64_t sampleRate) const;

 private:
  int64_t frameDuration_ = 1;
  int64_t timeScale_ = 1;
  int preroll_ = 1;
  int64_t next_ = 0;
  int64_t reanchors_ = 0;
  int64_t skipped_ = 0;
};

}  // namespace weblinked
