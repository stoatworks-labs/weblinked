#include <chrono>
#include <thread>

#include "core/frame_clock.h"
#include "core/frame.h"
#include "core/frame_ring.h"
#include "test_support.h"

using namespace weblinked;

WEBLINKED_TEST(clock_deadlines_do_not_drift_at_fractional_rates) {
  FrameClock clock(FrameRate{60000, 1001});

  // Roughly one hour of ticks. Note it is not exactly an hour: 59.94 Hz gives
  // 216000000/1001 = 215784.2 ticks per hour, so tick 215784 falls 3.6 ms short
  // of the hour mark. That is the correct answer, not an error.
  const int64_t oneHourTicks = 215'784;
  const int64_t deadline = clock.deadlineNanosForTick(oneHourTicks);

  const int64_t expected = (oneHourTicks * 1'000'000'000LL * 1001LL) / 60000LL;
  CHECK_EQ(deadline, expected);
  CHECK_NEAR(static_cast<double>(deadline), 3'600'000'000'000.0, 4'000'000.0);

  // Why the rational: an implementation that computes one rounded period and
  // accumulates it drifts by the rounding error times the tick count. In
  // nanoseconds that error is small — 0.33 ns per frame, about 72 microseconds
  // over an hour — but the same code written against microsecond or millisecond
  // periods, which is the usual way to reach for sleep_for, drifts a thousand
  // or a million times worse. Below is the microsecond case: ~72 ms after an
  // hour, more than four frames.
  const int64_t periodMicros = 1'000'000LL * 1001LL / 60000LL;  // 16683, not .33
  const int64_t naiveMicros = periodMicros * 1000LL * oneHourTicks;
  const int64_t driftNanos = deadline - naiveMicros;
  CHECK(driftNanos > 50'000'000LL);
  // Expressed in frames, which is the unit anyone actually cares about.
  const double driftFrames = static_cast<double>(driftNanos) /
                             static_cast<double>(clock.deadlineNanosForTick(1));
  CHECK(driftFrames > 4.0);

  // The shipped path accumulates nothing at all, so its own error stays inside
  // a single nanosecond regardless of how long the show runs.
  const int64_t tenHours = oneHourTicks * 10;
  const int64_t exactTenHours = (tenHours * 1'000'000'000LL * 1001LL) / 60000LL;
  CHECK_EQ(clock.deadlineNanosForTick(tenHours), exactTenHours);
}

WEBLINKED_TEST(clock_deadlines_are_exact_at_integer_rates) {
  FrameClock clock(FrameRate{50, 1});
  CHECK_EQ(clock.deadlineNanosForTick(0), 0);
  CHECK_EQ(clock.deadlineNanosForTick(1), 20'000'000);
  CHECK_EQ(clock.deadlineNanosForTick(50), 1'000'000'000);
  CHECK_EQ(clock.deadlineNanosForTick(180'000), 3'600'000'000'000LL);
}

WEBLINKED_TEST(clock_ticks_advance_and_account_for_every_drop) {
  // This test used to assert droppedTicks() == 0 after 25 ticks at 500 Hz, and
  // failed intermittently on a busy machine. The clock was right and the test was
  // wrong: a 2 ms deadline missed on a non-real-time scheduler is exactly the
  // case waitForNextTick() is designed to handle by skipping ahead. Asserting it
  // never happens is asserting the OS is real-time.
  //
  // What is deterministic, and is the property actually worth pinning, is that
  // the tick index never silently loses time: after N calls the index reached
  // must be exactly (N-1) plus however many ticks were abandoned. That holds
  // whether the machine is idle or thrashing.
  FrameClock clock(FrameRate{200, 1});  // 5 ms
  clock.start();

  const auto wallStart = std::chrono::steady_clock::now();
  constexpr int kCalls = 20;
  int64_t last = -1;
  for (int i = 0; i < kCalls; ++i) {
    const int64_t tick = clock.waitForNextTick();
    CHECK(tick > last);  // strictly increasing, always
    last = tick;
  }
  const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - wallStart)
                           .count();

  CHECK_EQ(last, static_cast<int64_t>(kCalls - 1) + clock.droppedTicks());

  // Time really did pass, and the clock did not run ahead of its own deadlines.
  CHECK(elapsed >= 90);  // 19 waits at 5 ms, less a little slack
  CHECK(clock.elapsedNanos() >= clock.deadlineNanosForTick(last));
}

WEBLINKED_TEST(clock_abandons_missed_ticks_instead_of_bursting) {
  FrameClock clock(FrameRate{1000, 1});  // 1 ms
  clock.start();
  clock.waitForNextTick();

  // Stall for far longer than a frame, as a page doing a heavy layout would.
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  const int64_t tick = clock.waitForNextTick();
  // The missed ticks are skipped, not replayed back-to-back.
  CHECK(tick > 20);
  CHECK(clock.droppedTicks() > 20);
}

WEBLINKED_TEST(latest_frame_slot_keeps_only_the_newest_paint) {
  VideoFormat format;
  format.width = 16;
  format.height = 16;
  auto pool = FramePool::create(format, PixelFormat::kBGRA);
  LatestFrameSlot slot;

  auto first = pool->acquire();
  first->setSequence(1);
  auto second = pool->acquire();
  second->setSequence(2);

  slot.publish(first);
  slot.publish(second);

  CHECK_EQ(slot.publishedCount(), 2);
  CHECK_EQ(slot.overwrittenCount(), 1);  // the browser outran the clock once

  auto taken = slot.take();
  CHECK(taken != nullptr);
  CHECK_EQ(taken ? taken->sequence() : -1, 2);
  CHECK(slot.take() == nullptr);  // taking empties the slot
}

WEBLINKED_TEST(latest_frame_slot_peek_allows_repeating_a_static_page) {
  VideoFormat format;
  format.width = 8;
  format.height = 8;
  auto pool = FramePool::create(format, PixelFormat::kBGRA);
  LatestFrameSlot slot;

  auto frame = pool->acquire();
  frame->setSequence(7);
  slot.publish(frame);

  // A page that has not repainted still has to produce a frame every tick.
  CHECK_EQ(slot.peek()->sequence(), 7);
  CHECK_EQ(slot.peek()->sequence(), 7);
}

WEBLINKED_TEST(frame_ring_drops_the_oldest_when_full) {
  VideoFormat format;
  format.width = 8;
  format.height = 8;
  auto pool = FramePool::create(format, PixelFormat::kBGRA);
  FrameRing ring(3);

  for (int i = 0; i < 5; ++i) {
    auto frame = pool->acquire();
    frame->setSequence(i);
    ring.push(std::move(frame));
  }

  CHECK_EQ(ring.size(), static_cast<size_t>(3));
  CHECK_EQ(ring.droppedCount(), 2);
  // What survives is the newest three.
  CHECK_EQ(ring.pop()->sequence(), 2);
  CHECK_EQ(ring.pop()->sequence(), 3);
  CHECK_EQ(ring.pop()->sequence(), 4);
  CHECK(ring.pop() == nullptr);
}

WEBLINKED_TEST(frame_pool_recycles_buffers) {
  VideoFormat format;
  format.width = 64;
  format.height = 64;
  auto pool = FramePool::create(format, PixelFormat::kBGRA);

  VideoFrame* firstAddress = nullptr;
  {
    auto frame = pool->acquire();
    firstAddress = frame.get();
    CHECK_EQ(frame->sizeBytes(), static_cast<size_t>(64 * 64 * 4));
    CHECK_EQ(frame->rowBytes(), 64 * 4);
    CHECK_EQ(pool->retainedCount(), static_cast<size_t>(0));
  }
  // Dropping the last reference should return the buffer rather than free it.
  CHECK_EQ(pool->retainedCount(), static_cast<size_t>(1));

  auto reused = pool->acquire();
  CHECK(reused.get() == firstAddress);
  CHECK_EQ(pool->retainedCount(), static_cast<size_t>(0));
}

WEBLINKED_TEST(frame_pool_stops_retaining_past_its_limit) {
  VideoFormat format;
  format.width = 8;
  format.height = 8;
  auto pool = FramePool::create(format, PixelFormat::kUYVY, 2);

  {
    std::vector<VideoFramePtr> frames;
    for (int i = 0; i < 5; ++i) {
      frames.push_back(pool->acquire());
    }
  }
  CHECK_EQ(pool->retainedCount(), static_cast<size_t>(2));
}
