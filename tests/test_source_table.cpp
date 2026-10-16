// The source table behind SourceManager — the route by which issue #12 could
// still have stopped the whole control API after the engine stopped holding its
// own mutex across driver calls.
//
// SourceManager used to hold one std::shared_mutex across every call into an
// engine. A request wedged in a DeckLink stop() held it for good; the next
// add, remove or settings load queued for the exclusive side; and libc++'s
// shared_mutex queues new readers behind a waiting writer, so every source's
// /api/state, every OSC message and the tray (on the UI thread) waited too.
// The table's own lock is now only held to copy a pointer, and a removal waits
// on the calls inside that one source, with a deadline.
//
// SourceManager itself holds Engines, which own CEF browsers and cannot be
// built here; these drive the same table with a fake engine.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/source_table.h"
#include "test_support.h"

using namespace weblinked;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

namespace {

/// Stands in for an Engine: one call that can be made to hang the way a
/// request inside a wedged driver does, and a state read that needs nothing
/// that call holds — as Engine::state() needs nothing a stuck output holds.
struct FakeEngine {
  std::mutex mutex;
  std::condition_variable signal;
  bool wedge = false;
  bool inside = false;
  std::atomic<int> reads{0};

  void wedgedCall() {
    std::unique_lock<std::mutex> lock(mutex);
    inside = true;
    signal.notify_all();
    signal.wait(lock, [&] { return !wedge; });
    inside = false;
  }

  void state() { reads.fetch_add(1); }

  void arm() {
    std::lock_guard<std::mutex> lock(mutex);
    wedge = true;
  }
  void waitInside() {
    std::unique_lock<std::mutex> lock(mutex);
    signal.wait(lock, [&] { return inside; });
  }
  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      wedge = false;
    }
    signal.notify_all();
  }
};

using Table = SourceTable<FakeEngine>;

int64_t millisSince(Clock::time_point start) {
  return std::chrono::duration_cast<milliseconds>(Clock::now() - start).count();
}

}  // namespace

WEBLINKED_TEST(source_table_a_wedged_call_holds_up_nothing_but_its_own_removal) {
  Table table;
  auto a = std::make_shared<FakeEngine>();
  auto b = std::make_shared<FakeEngine>();
  CHECK(table.insert("a", a));
  CHECK(table.insert("b", b));
  CHECK(!table.insert("a", std::make_shared<FakeEngine>()));

  // A request inside source a that never comes out: a POST /api/output stuck
  // in a DeckLink stop().
  a->arm();
  std::thread stuck([&] {
    table.with("a", [](FakeEngine& engine) { engine.wedgedCall(); });
  });
  a->waitInside();

  // A state read of the very source it is stuck in still answers, and so does
  // every other source, the list and the count the tray reads.
  auto start = Clock::now();
  CHECK(table.with("a", [](FakeEngine& engine) { engine.state(); }) ==
        Table::Access::kDone);
  CHECK(table.with("b", [](FakeEngine& engine) { engine.state(); }) ==
        Table::Access::kDone);
  CHECK_EQ(table.size(), size_t{2});
  CHECK_EQ(table.ids().size(), size_t{2});
  CHECK(table.firstId() == "a");
  CHECK(millisSince(start) < 500);
  CHECK_EQ(a->reads.load(), 1);
  CHECK_EQ(b->reads.load(), 1);

  // Adding and removing other sources goes ahead: under the old shared_mutex
  // both would have waited for good, and every reader with them.
  start = Clock::now();
  std::shared_ptr<FakeEngine> removed;
  CHECK(table.remove("b", std::chrono::seconds(5), &removed) == Table::Removal::kRemoved);
  CHECK(removed == b);
  CHECK(table.insert("c", std::make_shared<FakeEngine>()));
  CHECK(millisSince(start) < 500);

  // Removing the stuck source gives up at its deadline and changes nothing:
  // the source stays, open, rather than being stopped under the call or
  // dropped from the list while still running.
  start = Clock::now();
  removed.reset();
  CHECK(table.remove("a", milliseconds(200), &removed) == Table::Removal::kBusy);
  const int64_t gaveUp = millisSince(start);
  CHECK(gaveUp >= 190);
  CHECK(gaveUp < 2'000);
  CHECK(removed == nullptr);
  CHECK(table.has("a"));
  CHECK(table.with("a", [](FakeEngine& engine) { engine.state(); }) ==
        Table::Access::kDone);

  // While a removal is waiting, new calls to that source are turned away at
  // once rather than queued behind it.
  std::atomic<int> outcome{-1};
  std::thread remover([&] {
    std::shared_ptr<FakeEngine> taken;
    outcome.store(static_cast<int>(
        table.remove("a", std::chrono::seconds(10), &taken)));
  });
  bool closing = false;
  for (int i = 0; i < 200 && !closing; ++i) {
    closing = table.with("a", [](FakeEngine& engine) { engine.state(); }) ==
              Table::Access::kClosing;
    if (!closing) {
      std::this_thread::sleep_for(milliseconds(5));
    }
  }
  CHECK(closing);
  CHECK(table.with("c", [](FakeEngine& engine) { engine.state(); }) ==
        Table::Access::kDone);

  // And once the stuck call comes out, the removal completes.
  a->release();
  stuck.join();
  remover.join();
  CHECK_EQ(outcome.load(), static_cast<int>(Table::Removal::kRemoved));
  CHECK(!table.has("a"));
  CHECK(table.with("a", [](FakeEngine&) {}) == Table::Access::kMissing);
}

WEBLINKED_TEST(source_table_removal_waits_for_the_calls_inside) {
  // The guarantee rule 12 exists for: nothing is handed back to be stopped
  // while a call is still inside it.
  Table table;
  auto engine = std::make_shared<FakeEngine>();
  table.insert("main", engine);
  std::atomic<bool> callFinished{false};
  std::thread caller([&] {
    table.with("main", [&](FakeEngine&) {
      std::this_thread::sleep_for(milliseconds(150));
      callFinished.store(true);
    });
  });
  std::this_thread::sleep_for(milliseconds(30));
  std::shared_ptr<FakeEngine> removed;
  CHECK(table.remove("main", std::chrono::seconds(5), &removed) ==
        Table::Removal::kRemoved);
  CHECK(callFinished.load());
  CHECK(removed == engine);
  caller.join();
}

WEBLINKED_TEST(source_table_two_removals_of_one_source_stop_it_once) {
  Table table;
  table.insert("x", std::make_shared<FakeEngine>());
  std::thread caller([&] {
    table.with("x", [](FakeEngine&) { std::this_thread::sleep_for(milliseconds(100)); });
  });
  std::this_thread::sleep_for(milliseconds(20));
  std::atomic<int> removedCount{0};
  std::vector<std::thread> removers;
  for (int i = 0; i < 2; ++i) {
    removers.emplace_back([&] {
      std::shared_ptr<FakeEngine> taken;
      if (table.remove("x", std::chrono::seconds(5), &taken) ==
          Table::Removal::kRemoved) {
        removedCount.fetch_add(1);
      }
    });
  }
  for (auto& thread : removers) {
    thread.join();
  }
  caller.join();
  // One stops it; the other is told it is already being removed (or, if it
  // looked after the first had finished, that there is no such source).
  CHECK_EQ(removedCount.load(), 1);
  CHECK(!table.has("x"));
}

WEBLINKED_TEST(source_table_shutdown_leaves_a_stuck_source_alone) {
  Table table;
  auto stuckEngine = std::make_shared<FakeEngine>();
  auto idleEngine = std::make_shared<FakeEngine>();
  table.insert("stuck", stuckEngine);
  table.insert("idle", idleEngine);
  stuckEngine->arm();
  std::thread stuck([&] {
    table.with("stuck", [](FakeEngine& engine) { engine.wedgedCall(); });
  });
  stuckEngine->waitInside();

  const auto start = Clock::now();
  auto taken = table.takeAll(Clock::now() + milliseconds(100));
  CHECK(millisSince(start) < 2'000);
  CHECK_EQ(taken.idle.size(), size_t{1});
  CHECK(taken.idle.size() == 1 && taken.idle.front() == idleEngine);
  // Handed back, not dropped: dropping it would leave the straggler holding
  // the last reference, and the engine destroyed on its thread, late.
  CHECK_EQ(taken.busy.size(), size_t{1});
  CHECK(taken.busy.size() == 1 && taken.busy.front().first == "stuck" &&
        taken.busy.front().second == stuckEngine);
  CHECK_EQ(table.size(), size_t{0});

  stuckEngine->release();
  stuck.join();
}

WEBLINKED_TEST(source_table_a_removal_overtaken_by_shutdown_hands_it_over) {
  // A removal is waiting on a source when shutdown takes everything. Whatever
  // happens next, the source has one owner: the removal neither reopens an
  // entry the table no longer has nor stops it a second time.
  Table table;
  auto engine = std::make_shared<FakeEngine>();
  table.insert("s", engine);
  engine->arm();
  std::thread stuck([&] {
    table.with("s", [](FakeEngine& e) { e.wedgedCall(); });
  });
  engine->waitInside();

  std::atomic<int> outcome{-1};
  std::shared_ptr<FakeEngine> removedByRemove;
  std::thread remover([&] {
    outcome.store(static_cast<int>(
        table.remove("s", milliseconds(400), &removedByRemove)));
  });
  std::this_thread::sleep_for(milliseconds(50));
  auto taken = table.takeAll(Clock::now() + milliseconds(100));
  CHECK_EQ(taken.busy.size(), size_t{1});
  remover.join();
  CHECK(outcome.load() == static_cast<int>(Table::Removal::kBusy) ||
        outcome.load() == static_cast<int>(Table::Removal::kRemoving));
  CHECK(removedByRemove == nullptr);
  CHECK(!table.has("s"));
  CHECK(table.with("s", [](FakeEngine&) {}) == Table::Access::kMissing);

  engine->release();
  stuck.join();
}
