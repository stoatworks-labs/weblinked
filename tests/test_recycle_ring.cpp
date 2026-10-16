// The frame ring behind the DeckLink output — issue #12.
//
// Frames are created once at start and recycled as the card hands them back,
// so the SDK's buffer allocator is never emptied mid-run. What has to hold for
// that to be safe: a frame is never handed to two writers, a frame the card
// still has is never handed out at all, the clock thread never waits for one,
// and every frame comes back at close() for its owner to destroy.

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

#include "core/recycle_ring.h"
#include "test_support.h"

using namespace weblinked;

namespace {

struct Frame {
  int id = 0;
  std::atomic<int> holders{0};  ///< > 1 would mean two writers at once
};

}  // namespace

WEBLINKED_TEST(ring_hands_out_each_frame_once_and_counts_misses) {
  Frame frames[4];
  RecycleRing<Frame> ring;
  for (auto& frame : frames) {
    ring.add(&frame);
  }
  CHECK_EQ(ring.size(), size_t{4});
  CHECK_EQ(ring.freeCount(), size_t{4});

  std::set<Frame*> out;
  for (int i = 0; i < 4; ++i) {
    Frame* frame = ring.acquire();
    CHECK(frame != nullptr);
    out.insert(frame);
  }
  CHECK_EQ(out.size(), size_t{4});
  CHECK_EQ(ring.freeCount(), size_t{0});

  // Every frame is with the card: no waiting, a null and a miss.
  CHECK(ring.acquire() == nullptr);
  CHECK(ring.acquire() == nullptr);
  CHECK_EQ(ring.misses(), int64_t{2});

  // One comes back and is the one handed out next.
  CHECK(ring.release(&frames[2]));
  CHECK(ring.acquire() == &frames[2]);
}

WEBLINKED_TEST(ring_refuses_a_double_release_and_a_stranger) {
  Frame frames[2];
  Frame stranger;
  RecycleRing<Frame> ring;
  ring.add(&frames[0]);
  ring.add(&frames[1]);

  Frame* frame = ring.acquire();
  CHECK(ring.release(frame));
  // A completion reported twice must not free a frame that has since been
  // handed out again — or free one twice.
  CHECK(!ring.release(frame));
  CHECK_EQ(ring.doubleReleases(), int64_t{1});
  CHECK_EQ(ring.freeCount(), size_t{2});

  CHECK(!ring.release(&stranger));
  CHECK_EQ(ring.strangers(), int64_t{1});
  CHECK_EQ(ring.freeCount(), size_t{2});

  // Matched by address, as a completion arrives through a base pointer.
  Frame* again = ring.acquire();
  const void* asBase = again;
  CHECK(ring.release(asBase));
}

WEBLINKED_TEST(ring_close_returns_everything_and_stops_both_directions) {
  Frame frames[3];
  RecycleRing<Frame> ring;
  for (auto& frame : frames) {
    ring.add(&frame);
  }
  Frame* held = ring.acquire();
  CHECK(held != nullptr);

  size_t outstanding = 99;
  const auto items = ring.close(&outstanding);
  CHECK_EQ(items.size(), size_t{3});
  CHECK_EQ(outstanding, size_t{1});

  // After a stop nothing is handed out, and a completion that straggles in is
  // ignored rather than putting a frame its owner is destroying back in play.
  CHECK(ring.acquire() == nullptr);
  CHECK(!ring.release(held));
  CHECK_EQ(ring.size(), size_t{0});
}

WEBLINKED_TEST(ring_survives_a_clock_and_a_completion_thread_at_once) {
  // The real shape: the clock thread acquires, writes and "schedules"; the
  // completion thread hands frames back a few frames later. No frame may ever
  // have two holders, and the clock must never block.
  constexpr int kFrames = 7;
  Frame frames[kFrames];
  RecycleRing<Frame> ring;
  for (int i = 0; i < kFrames; ++i) {
    frames[i].id = i;
    ring.add(&frames[i]);
  }

  std::mutex queueMutex;
  std::condition_variable queueSignal;
  std::deque<Frame*> withCard;
  std::atomic<bool> done{false};
  std::atomic<int> overlaps{0};
  std::atomic<int64_t> completed{0};
  std::atomic<int> refusedReleases{0};

  std::thread completion([&] {
    while (true) {
      Frame* frame = nullptr;
      {
        std::unique_lock<std::mutex> lock(queueMutex);
        queueSignal.wait(lock, [&] { return !withCard.empty() || done.load(); });
        if (withCard.empty()) {
          return;
        }
        // The card keeps a few frames queued; let it lag.
        if (withCard.size() < 3 && !done.load()) {
          lock.unlock();
          std::this_thread::yield();
          continue;
        }
        frame = withCard.front();
        withCard.pop_front();
      }
      frame->holders.fetch_sub(1);
      if (!ring.release(frame)) {
        refusedReleases.fetch_add(1);
      }
      completed.fetch_add(1);
    }
  });

  int64_t scheduled = 0;
  for (int i = 0; i < 200'000; ++i) {
    Frame* frame = ring.acquire();
    if (frame == nullptr) {
      continue;  // dropped and counted, as submit() does
    }
    if (frame->holders.fetch_add(1) != 0) {
      overlaps.fetch_add(1);
    }
    ++scheduled;
    {
      std::lock_guard<std::mutex> lock(queueMutex);
      withCard.push_back(frame);
    }
    queueSignal.notify_one();
  }
  done.store(true);
  queueSignal.notify_all();
  completion.join();

  CHECK_EQ(overlaps.load(), 0);
  CHECK_EQ(refusedReleases.load(), 0);
  CHECK_EQ(completed.load(), scheduled);
  CHECK_EQ(ring.freeCount(), size_t{kFrames});
  CHECK_EQ(ring.doubleReleases(), int64_t{0});
  // Every attempt either got a frame or was counted as a miss; none waited.
  CHECK_EQ(scheduled + ring.misses(), int64_t{200'000});
}
