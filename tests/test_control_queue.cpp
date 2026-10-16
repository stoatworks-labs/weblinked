// The control queue — the last route by which one wedged driver could fill the
// HTTP server.
//
// Control changes to a source take turns, waiting up to 30 s for the one in
// front. With a driver wedged inside the change at the front, every press of a
// button by an operator parked an HTTP thread for those 30 s; enough presses
// and the 64-connection cap was full and /api/state got a 503 too. A waiter now
// gives up at once when four are already waiting, or when the change in front
// is inside a call that has stopped answering.

#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>
#include <vector>

#include "core/control_queue.h"
#include "test_support.h"

using namespace weblinked;
using std::chrono::milliseconds;
using Clock = ControlQueue::Clock;

namespace {

int64_t millisSince(Clock::time_point start) {
  return std::chrono::duration_cast<milliseconds>(Clock::now() - start).count();
}

}  // namespace

WEBLINKED_TEST(control_queue_takes_turns_and_waits_for_a_slow_change) {
  ControlQueue queue(std::chrono::seconds(5), 4, milliseconds(20));
  std::unique_lock<std::timed_mutex> holder(queue.mutex());

  std::atomic<int> result{-1};
  std::thread waiter([&] {
    std::unique_lock<std::timed_mutex> lock(queue.mutex(), std::defer_lock);
    result.store(static_cast<int>(queue.enter(lock, [] { return false; })));
  });
  std::this_thread::sleep_for(milliseconds(150));  // a slow, healthy change
  CHECK_EQ(result.load(), -1);
  holder.unlock();
  waiter.join();
  CHECK_EQ(result.load(), static_cast<int>(ControlQueue::Result::kHeld));
}

WEBLINKED_TEST(control_queue_gives_up_at_once_behind_a_stuck_change) {
  ControlQueue queue(std::chrono::seconds(30), 4, milliseconds(20));
  std::unique_lock<std::timed_mutex> holder(queue.mutex());
  std::atomic<bool> stuck{false};

  // While the change in front is merely busy, a waiter waits.
  std::atomic<int> result{-1};
  const auto start = Clock::now();
  std::thread waiter([&] {
    std::unique_lock<std::timed_mutex> lock(queue.mutex(), std::defer_lock);
    result.store(static_cast<int>(queue.enter(lock, [&] { return stuck.load(); })));
  });
  std::this_thread::sleep_for(milliseconds(100));
  CHECK_EQ(result.load(), -1);
  // Once it is stuck in a driver, the waiter leaves within a poll, not after
  // thirty seconds holding an HTTP thread.
  stuck.store(true);
  waiter.join();
  CHECK_EQ(result.load(), static_cast<int>(ControlQueue::Result::kHolderStuck));
  CHECK(millisSince(start) < 2'000);
  CHECK_EQ(queue.waiting(), 0);
}

WEBLINKED_TEST(control_queue_turns_away_a_fifth_waiter) {
  ControlQueue queue(std::chrono::seconds(30), 4, milliseconds(20));
  std::unique_lock<std::timed_mutex> holder(queue.mutex());

  std::vector<std::thread> waiters;
  std::atomic<int> held{0};
  for (int i = 0; i < 4; ++i) {
    waiters.emplace_back([&] {
      std::unique_lock<std::timed_mutex> lock(queue.mutex(), std::defer_lock);
      if (queue.enter(lock, [] { return false; }) == ControlQueue::Result::kHeld) {
        held.fetch_add(1);
      }
    });
  }
  for (int i = 0; i < 200 && queue.waiting() < 4; ++i) {
    std::this_thread::sleep_for(milliseconds(5));
  }
  CHECK_EQ(queue.waiting(), 4);

  // The fifth is answered at once: four waiting is a queue that is not going
  // anywhere soon, and each of them is a connection.
  const auto start = Clock::now();
  std::unique_lock<std::timed_mutex> fifth(queue.mutex(), std::defer_lock);
  CHECK(queue.enter(fifth, [] { return false; }) == ControlQueue::Result::kTooMany);
  CHECK(millisSince(start) < 100);
  CHECK_EQ(queue.waiting(), 4);

  // And the four get their turns, one after another, once the front moves.
  holder.unlock();
  for (auto& thread : waiters) {
    thread.join();
  }
  CHECK_EQ(held.load(), 4);
  CHECK_EQ(queue.waiting(), 0);
}

WEBLINKED_TEST(control_queue_times_out_without_a_stuck_signal) {
  ControlQueue queue(milliseconds(200), 4, milliseconds(20));
  std::unique_lock<std::timed_mutex> holder(queue.mutex());
  std::unique_lock<std::timed_mutex> lock(queue.mutex(), std::defer_lock);
  const auto start = Clock::now();
  CHECK(queue.enter(lock, [] { return false; }) == ControlQueue::Result::kTimedOut);
  const int64_t took = millisSince(start);
  CHECK(took >= 190);
  CHECK(took < 2'000);
}
