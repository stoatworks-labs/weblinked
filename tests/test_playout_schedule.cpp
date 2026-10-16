// Scheduled playback after a stall — issue #13.
//
// The DeckLink output schedules frame N at N * frameDuration. Before the fix
// nothing ever moved N forward, so after a stall longer than the card's queue
// every frame from then on was scheduled behind the card's playhead and played
// late, for ever: 253,368 late out of 269,280 over an hour on a real rig after
// one three-second SIGSTOP. These drive PlayoutSchedule against a model card
// whose playhead moves one frame per step, and count what would have been late.
//
// The model: at each step the card is sending frame `cardFrame`, so its
// playhead is cardFrame * frameDuration. A frame scheduled now is on time if its
// display time is strictly after that, and late otherwise. The card itself is
// not modelled further, because that inequality is the whole of issue #13.

#include <cstdint>

#include "core/playout_schedule.h"
#include "test_support.h"

using namespace weblinked;

namespace {

struct ModelCard {
  PlayoutSchedule schedule;
  bool reanchoring = true;  ///< false is the behaviour before the fix
  int64_t cardFrame = 0;
  int64_t lastIndex = -1;
  int64_t onTime = 0;
  int64_t late = 0;
  bool monotonic = true;
  bool prerolling = false;

  ModelCard(int64_t frameDuration, int64_t timeScale, int preroll,
            bool reanchor = true)
      : schedule(frameDuration, timeScale, preroll), reanchoring(reanchor) {
    // Pre-rolled before StartScheduledPlayback, as the output does. Playback
    // has not started, so frame 0 at time 0 is on time by definition.
    prerolling = true;
    for (int i = 0; i < preroll; ++i) {
      deliver();
    }
    prerolling = false;
  }

  int64_t playhead() const { return cardFrame * schedule.frameDuration(); }
  int64_t depth() const { return schedule.nextIndex() - cardFrame; }

  void deliver() {
    const int64_t index = schedule.nextIndex();
    if (index <= lastIndex) {
      monotonic = false;
    }
    lastIndex = index;
    if (prerolling || schedule.nextDisplayTime() > playhead()) {
      ++onTime;
    } else {
      ++late;
    }
    schedule.advance();
  }

  /// One frame passes on the card. The producer delivers one frame in it
  /// unless it is stalled.
  void frame(bool produce = true) {
    if (produce) {
      if (reanchoring) {
        schedule.reanchor(playhead(), false);
      }
      deliver();
    }
    ++cardFrame;
  }

  void run(int frames) {
    for (int i = 0; i < frames; ++i) {
      frame(true);
    }
  }

  void stall(int frames) {
    for (int i = 0; i < frames; ++i) {
      frame(false);
    }
  }
};

}  // namespace

WEBLINKED_TEST(playout_without_a_stall_never_moves) {
  ModelCard card(1000, 25000, 3);  // 1080p25, as the card reports it
  card.run(10'000);
  CHECK_EQ(card.late, 0);
  CHECK_EQ(card.schedule.reanchors(), 0);
  CHECK_EQ(card.depth(), 3);
  CHECK(card.monotonic);
}

WEBLINKED_TEST(playout_stall_shorter_than_the_queue_does_not_jump) {
  ModelCard card(1000, 25000, 3);
  card.run(100);
  card.stall(2);
  card.run(1'000);
  CHECK_EQ(card.late, 0);
  CHECK_EQ(card.schedule.reanchors(), 0);
  // The queue paid for the stall and stays shorter; that alone is not late.
  CHECK_EQ(card.depth(), 1);
  CHECK(card.monotonic);
}

WEBLINKED_TEST(playout_stall_longer_than_the_queue_was_late_for_ever) {
  // The behaviour before the fix, kept as a test so the failure stays
  // described: three seconds stopped at 25p, then every frame late.
  ModelCard card(1000, 25000, 3, /*reanchor=*/false);
  card.run(100);
  card.stall(75);
  card.run(1'000);
  CHECK_EQ(card.late, 1'000);
}

WEBLINKED_TEST(playout_stall_longer_than_the_queue_reanchors_once) {
  ModelCard card(1000, 25000, 3);
  card.run(100);
  card.stall(75);
  // On resume the card is sending frame 175 and the next slot is 103 — 72
  // frames in the past. It moves to 175 + 3.
  CHECK_EQ(card.schedule.nextIndex(), 103);
  card.frame();
  CHECK_EQ(card.schedule.reanchors(), 1);
  CHECK_EQ(card.schedule.skippedSlots(), 75);
  CHECK_EQ(card.lastIndex, 178);
  card.run(1'000);
  CHECK_EQ(card.late, 0);
  CHECK_EQ(card.schedule.reanchors(), 1);
  CHECK_EQ(card.depth(), 3);
  CHECK(card.monotonic);
}

WEBLINKED_TEST(playout_repeated_stalls_each_reanchor_and_stay_on_time) {
  ModelCard card(1001, 30000, 3);  // 29.97
  for (int i = 0; i < 11; ++i) {  // Daniel's eleven SIGSTOP cycles
    card.run(1'500);
    card.stall(90);  // three seconds
  }
  card.run(1'500);
  CHECK_EQ(card.late, 0);
  CHECK_EQ(card.schedule.reanchors(), 11);
  CHECK_EQ(card.depth(), 3);
  CHECK(card.monotonic);
}

WEBLINKED_TEST(playout_short_stalls_that_drain_the_queue_reanchor_when_it_runs_out) {
  // Each one-frame stall costs the queue a frame and nothing gives it back.
  // The third leaves the next slot on the frame being sent, which is late, so
  // that is where it moves — and not before.
  ModelCard card(1000, 50000, 3);
  card.run(50);
  card.stall(1);
  card.run(50);
  card.stall(1);
  card.run(50);
  CHECK_EQ(card.schedule.reanchors(), 0);
  CHECK_EQ(card.depth(), 1);
  card.stall(1);
  card.run(50);
  CHECK_EQ(card.schedule.reanchors(), 1);
  CHECK_EQ(card.depth(), 3);
  CHECK_EQ(card.late, 0);
  CHECK(card.monotonic);
}

WEBLINKED_TEST(playout_a_slow_clock_reanchors_periodically_instead_of_going_late) {
  // The engine's clock is software and the card's is not. One that runs 1 part
  // in 1001 slow loses a frame every 1001: the queue drains in three of those,
  // and without re-anchoring every frame after that is late.
  ModelCard before(1000, 25000, 3, /*reanchor=*/false);
  ModelCard after(1000, 25000, 3);
  for (int i = 1; i <= 100'100; ++i) {
    const bool produce = i % 1001 != 0;
    before.frame(produce);
    after.frame(produce);
  }
  CHECK(before.late > 90'000);
  CHECK_EQ(after.late, 0);
  // 100 lost frames, each re-anchor buying back three.
  CHECK(after.schedule.reanchors() >= 32);
  CHECK(after.schedule.reanchors() <= 34);
  CHECK(after.monotonic);
}

WEBLINKED_TEST(playout_a_late_report_needs_a_whole_frame_of_headroom) {
  PlayoutSchedule schedule(1000, 25000, 3);
  for (int i = 0; i < 10; ++i) {
    schedule.advance();
  }
  // Next slot is frame 10. The card is part-way through frame 9.
  const int64_t playhead = 9 * 1000 + 400;
  CHECK_EQ(schedule.reanchor(playhead, false), 0);  // still ahead
  // But the card has just said a frame was late, and a slot 0.6 of a frame out
  // has no time to be transferred: refill to the preroll.
  const int64_t skipped = schedule.reanchor(playhead, true);
  CHECK_EQ(skipped, 3);
  CHECK_EQ(schedule.nextIndex(), 13);  // first whole frame after 9.4 is 10, + 3
  // With the queue at the preroll a late report is not acted on again.
  CHECK_EQ(schedule.reanchor(playhead, true), 0);
}

WEBLINKED_TEST(playout_never_moves_backwards) {
  PlayoutSchedule schedule(1000, 25000, 3);
  for (int i = 0; i < 100; ++i) {
    schedule.advance();
  }
  // A playhead behind the schedule — a card that restarted, a stale reading —
  // must not pull the index back onto slots already used.
  CHECK_EQ(schedule.reanchor(0, true), 0);
  CHECK_EQ(schedule.reanchor(-5'000, false), 0);
  CHECK_EQ(schedule.nextIndex(), 100);
  // And a playhead exactly on the next slot is behind it.
  CHECK_EQ(schedule.reanchor(100 * 1000, false), 3);
  CHECK_EQ(schedule.nextIndex(), 103);
}

WEBLINKED_TEST(playout_time_is_exact_at_59_94) {
  // The card's own pair for 1080p59.94: 1001 in 60000. Ten hours of frames.
  PlayoutSchedule schedule(1001, 60000, 3);
  const int64_t tenHours = 2'157'842;
  for (int64_t i = 0; i < tenHours; ++i) {
    schedule.advance();
  }
  CHECK_EQ(schedule.nextDisplayTime(), tenHours * 1001);

  // Audio per frame alternates 800/801 and never drifts: 1001 * 48000 / 60000
  // is 4004/5 samples a frame.
  for (int64_t i = 0; i < 10'000; ++i) {
    const int64_t share =
        schedule.samplePosition(i + 1, 48000) - schedule.samplePosition(i, 48000);
    CHECK(share == 800 || share == 801);
  }
  CHECK_EQ(schedule.samplePosition(5, 48000), 4004);
  // Far past where index * frameDuration * sampleRate would overflow 64 bits
  // done naively (~1.9e11 frames); the split keeps it exact.
  for (const int64_t index :
       {int64_t{1'000'000'000}, int64_t{123'456'789'012}, int64_t{900'000'000'000}}) {
    CHECK_EQ(schedule.samplePosition(index, 48000), (index * 4004) / 5);
  }

  PlayoutSchedule pal(1000, 25000, 3);
  CHECK_EQ(pal.samplePosition(75, 48000), 75 * 1920);
}
