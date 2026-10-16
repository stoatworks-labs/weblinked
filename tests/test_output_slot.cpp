// The output slot — the other half of issue #12.
//
// On a real rig the DeckLink driver deadlocked inside a submit() while the
// clock thread held Engine::mutex_, and every GET /api/state then waited on
// that mutex for ever. The engine now holds each output in an OutputSlot and
// never holds its own mutex across a call into one; state() asks each slot for
// a status with a deadline and takes the last one that completed if the gate
// is held.
//
// Engine itself cannot be built here — it owns a CEF browser, and these tests
// link weblinked_core only — so this drives the mechanism the engine uses, with
// a fake output whose submit() can be made to hang the way the driver did.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include "outputs/output_slot.h"
#include "test_support.h"

using namespace weblinked;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

namespace {

/// Shared between a fake output and the test, so the test can still see it
/// once the slot has destroyed the output.
struct FakeControl {
  std::mutex mutex;
  std::condition_variable signal;
  bool wedge = false;     ///< submit() hangs until this is cleared
  bool inSubmit = false;
  std::atomic<int> submits{0};
  std::atomic<int> destroyed{0};
};

class FakeOutput final : public IOutput {
 public:
  FakeOutput(std::shared_ptr<FakeControl> control, PixelFormat format)
      : IOutput("fake"), control_(std::move(control)), format_(format) {}
  ~FakeOutput() override { control_->destroyed.fetch_add(1); }

  std::string kind() const override { return "fake"; }
  PixelFormat pixelFormat() const override { return format_; }
  bool start(const VideoFormat&, std::string&) override {
    running_ = true;
    return true;
  }
  void stop() override { running_ = false; }

  void submit(const VideoFrame&, const AudioBlock&) override {
    std::unique_lock<std::mutex> lock(control_->mutex);
    control_->inSubmit = true;
    control_->signal.notify_all();
    control_->signal.wait(lock, [&] { return !control_->wedge; });
    control_->inSubmit = false;
    control_->submits.fetch_add(1);
  }

  json::Value status() const override {
    json::Value value = json::Value::object();
    value.set("kind", json::Value("fake"));
    value.set("frames", json::Value(control_->submits.load()));
    return value;
  }

 private:
  std::shared_ptr<FakeControl> control_;
  PixelFormat format_;
};

VideoFramePtr testFrame() {
  VideoFormat format;
  format.width = 16;
  format.height = 16;
  return std::make_shared<VideoFrame>(format, PixelFormat::kBGRA);
}

/// Starts a submit() on another thread and returns once it is inside the
/// driver, wedged.
std::thread wedgeInSubmit(const std::shared_ptr<OutputSlot>& slot,
                          const std::shared_ptr<FakeControl>& control,
                          const VideoFramePtr& frame) {
  {
    std::lock_guard<std::mutex> lock(control->mutex);
    control->wedge = true;
  }
  const uint64_t generation = slot->description().generation;
  std::thread clock([slot, frame, generation] {
    slot->call(Clock::now() + std::chrono::hours(1), generation,
               [&](IOutput& output) { output.submit(*frame, AudioBlock{}); });
  });
  std::unique_lock<std::mutex> lock(control->mutex);
  control->signal.wait(lock, [&] { return control->inSubmit; });
  return clock;
}

void unwedge(const std::shared_ptr<FakeControl>& control) {
  {
    std::lock_guard<std::mutex> lock(control->mutex);
    control->wedge = false;
  }
  control->signal.notify_all();
}

int64_t millisSince(Clock::time_point start) {
  return std::chrono::duration_cast<milliseconds>(Clock::now() - start).count();
}

}  // namespace

WEBLINKED_TEST(slot_status_is_fresh_when_nothing_holds_it) {
  auto control = std::make_shared<FakeControl>();
  auto slot = std::make_shared<OutputSlot>(
      std::make_unique<FakeOutput>(control, PixelFormat::kUYVY));
  const auto frame = testFrame();
  const uint64_t generation = slot->description().generation;
  CHECK(slot->call(Clock::now() + milliseconds(100), generation,
                   [&](IOutput& output) { output.submit(*frame, AudioBlock{}); }));

  const json::Value status = slot->status(Clock::now() + milliseconds(100));
  CHECK(!status["stale"].asBool(true));
  CHECK_EQ(status["frames"].asInt(-1), 1);
}

WEBLINKED_TEST(slot_status_returns_promptly_while_submit_is_wedged) {
  auto control = std::make_shared<FakeControl>();
  // Presumed wedged after 200 ms rather than the engine's second, to keep the
  // test quick; the behaviour is the same.
  auto slot = std::make_shared<OutputSlot>(
      std::make_unique<FakeOutput>(control, PixelFormat::kUYVY), milliseconds(200));
  const auto frame = testFrame();

  // One good status first, so there is a last-known value to fall back to.
  slot->status(Clock::now() + milliseconds(100));

  std::thread clock = wedgeInSubmit(slot, control, frame);

  // The state poll: bounded by its own deadline, not by the driver. Busy for
  // less than the wedge threshold is only busy — the last figures, and not
  // "stale", which would flicker the control page red for every card opening.
  auto start = Clock::now();
  json::Value status = slot->status(start + milliseconds(100));
  const int64_t firstWait = millisSince(start);
  CHECK(!status["stale"].asBool(true));
  CHECK(!status.has("driver_busy_ms"));
  CHECK_EQ(status["frames"].asInt(-1), 0);  // the last value that completed
  CHECK(firstWait >= 90);
  CHECK(firstWait < 2'000);

  // Once the call has been in the driver for longer than the wedge threshold,
  // nobody waits for it at all: Daniel's client polled every 200 ms with a
  // 300 ms timeout, and should not spend the timeout learning nothing.
  std::this_thread::sleep_for(milliseconds(250));
  start = Clock::now();
  status = slot->status(start + std::chrono::seconds(5));
  CHECK(status["stale"].asBool(false));
  CHECK(status["driver_busy_ms"].asInt(0) >= 250);
  CHECK(millisSince(start) < 1'000);

  // A control request is refused, not parked.
  start = Clock::now();
  bool stopped = false;
  CHECK(slot->exclusive(start + std::chrono::seconds(5), [&](IOutput& output) {
          output.stop();
          stopped = true;
        }) == OutputSlot::Result::kBusy);
  CHECK(!stopped);
  CHECK(millisSince(start) < 1'000);
  CHECK(slot->refusals() >= 3);

  unwedge(control);
  clock.join();
  status = slot->status(Clock::now() + milliseconds(100));
  CHECK(!status["stale"].asBool(true));
  CHECK_EQ(status["frames"].asInt(-1), 1);
}

WEBLINKED_TEST(slot_never_destroys_an_output_with_a_call_in_flight) {
  auto control = std::make_shared<FakeControl>();
  auto slot = std::make_shared<OutputSlot>(
      std::make_unique<FakeOutput>(control, PixelFormat::kUYVY), milliseconds(200));
  const auto frame = testFrame();
  std::thread clock = wedgeInSubmit(slot, control, frame);

  // Removal while the driver is inside submit(): the gate is held, so the
  // output cannot be destroyed under the call — removal is refused instead.
  CHECK(!slot->replace(Clock::now() + milliseconds(100),
                       [](std::unique_ptr<IOutput>& output) { output.reset(); }));
  CHECK_EQ(control->destroyed.load(), 0);

  unwedge(control);
  clock.join();

  // Now it goes, under the gate. A copy of the slot held elsewhere — the
  // clock thread's list for this tick — finds it empty and does nothing.
  std::shared_ptr<OutputSlot> heldByClock = slot;
  const uint64_t oldGeneration = slot->description().generation;
  CHECK(slot->replace(Clock::now() + milliseconds(100),
                      [](std::unique_ptr<IOutput>& output) {
                        output->stop();
                        output.reset();
                      }));
  CHECK_EQ(control->destroyed.load(), 1);
  slot.reset();
  bool ran = false;
  CHECK(!heldByClock->call(Clock::now() + milliseconds(100), oldGeneration,
                           [&](IOutput&) { ran = true; }));
  CHECK(!ran);
  CHECK(!heldByClock->description().present);
}

WEBLINKED_TEST(slot_generation_stops_a_tick_planned_for_the_old_output) {
  // The clock decides which conversion each output gets under the engine's
  // mutex, then calls it after letting go. If the output was replaced in
  // between — UYVY by BGRA, say — the planned payload is the wrong size for
  // it. The generation is how the call finds out and does nothing.
  auto control = std::make_shared<FakeControl>();
  auto slot = std::make_shared<OutputSlot>(
      std::make_unique<FakeOutput>(control, PixelFormat::kUYVY));
  const auto planned = slot->description();
  CHECK(planned.present);
  CHECK(planned.pixelFormat == PixelFormat::kUYVY);

  CHECK(slot->replace(Clock::now() + milliseconds(100),
                      [&](std::unique_ptr<IOutput>& output) {
                        output = std::make_unique<FakeOutput>(control,
                                                              PixelFormat::kBGRA);
                      }));
  const auto now = slot->description();
  CHECK(now.generation != planned.generation);
  CHECK(now.pixelFormat == PixelFormat::kBGRA);

  bool ran = false;
  CHECK(!slot->call(Clock::now() + milliseconds(100), planned.generation,
                    [&](IOutput&) { ran = true; }));
  CHECK(!ran);
  CHECK(slot->call(Clock::now() + milliseconds(100), now.generation,
                   [&](IOutput&) { ran = true; }));
  CHECK(ran);

  // A replace() moves the generation on even when it leaves the same object:
  // a replacement can be allocated at the address of what it replaced, so the
  // slot cannot tell by looking.
  CHECK(slot->replace(Clock::now() + milliseconds(100),
                      [](std::unique_ptr<IOutput>&) {}));
  const auto afterReplace = slot->description();
  CHECK(afterReplace.generation != now.generation);

  // But starting or stopping the same object does not: re-enabling an output
  // that is already running must not cost the tick the clock planned for it.
  CHECK(slot->exclusive(Clock::now() + milliseconds(100), [](IOutput& output) {
          output.stop();
          std::string error;
          output.start(VideoFormat{}, error);
        }) == OutputSlot::Result::kRan);
  CHECK(slot->description().generation == afterReplace.generation);
  ran = false;
  CHECK(slot->call(Clock::now() + milliseconds(100), afterReplace.generation,
                   [&](IOutput&) { ran = true; }));
  CHECK(ran);

  // And an empty slot says so, rather than "busy".
  CHECK(slot->replace(Clock::now() + milliseconds(100),
                      [](std::unique_ptr<IOutput>& output) { output.reset(); }));
  CHECK(slot->exclusive(Clock::now() + milliseconds(100), [](IOutput&) {}) ==
        OutputSlot::Result::kEmpty);
}

WEBLINKED_TEST(slot_a_spent_budget_still_gives_each_output_a_fair_wait) {
  // Issue #12 review: the clock gave all its outputs one 5 ms deadline, so by
  // the time a DeckLink late in the list came up after slow submits, the
  // deadline had passed and it got a single try. A state poll holding its gate
  // for a moment then cost it a frame. Here the fake's status() holds the gate
  // for 20 ms, and the "clock" arrives with its shared budget already spent.
  class SlowStatus final : public IOutput {
   public:
    SlowStatus() : IOutput("slow") { running_ = true; }
    std::string kind() const override { return "slow"; }
    PixelFormat pixelFormat() const override { return PixelFormat::kUYVY; }
    bool start(const VideoFormat&, std::string&) override { return true; }
    void stop() override {}
    void submit(const VideoFrame&, const AudioBlock&) override {}
    json::Value status() const override {
      std::this_thread::sleep_for(milliseconds(20));
      return json::Value::object();
    }
  };
  auto slot = std::make_shared<OutputSlot>(std::make_unique<SlowStatus>());
  const uint64_t generation = slot->description().generation;

  const auto pollWhile = [&](const std::function<bool()>& callFromClock) {
    std::thread poll([&] { slot->status(Clock::now() + milliseconds(500)); });
    std::this_thread::sleep_for(milliseconds(5));  // the poll has the gate
    const bool ran = callFromClock();
    poll.join();
    return ran;
  };

  const auto spent = Clock::now() - milliseconds(1);
  // Before: the spent deadline alone — one try, and the tick is lost.
  CHECK(!pollWhile([&] {
    return slot->call(spent, generation, [](IOutput&) {});
  }));
  // After: never less than a floor of its own, which outlasts the poll.
  CHECK(pollWhile([&] {
    return slot->call(OutputSlot::atLeast(spent, milliseconds(50)), generation,
                      [](IOutput&) {});
  }));
}
