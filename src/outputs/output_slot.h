#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "core/call_gate.h"
#include "core/json.h"
#include "outputs/output.h"

namespace weblinked {

/// One output, the gate every call into it goes through, and the last status it
/// reported.
///
/// The engine holds each output in one of these, by shared_ptr, so that:
///
/// - The engine's own mutex is never held across a call into a driver. The
///   clock thread and the control paths copy the slot out under that mutex and
///   call through the gate after letting it go. A driver that wedges inside a
///   call (issue #12) then holds one gate, not the mutex every control request
///   needs.
/// - Nobody waits on a gate for ever (see CallGate). The state report falls
///   back to the last status that completed, and marks it stale once the call
///   in the way has been running long enough to count as stuck.
/// - The output is only ever replaced or destroyed under the gate, by a control
///   path, through replace(), so no call is ever in flight on an object being
///   destroyed. A copy of the slot can outlive the entry it came from; by then
///   the slot is empty, and call() on an empty slot does nothing.
///
/// Header-only and free of CEF so that the tests can drive it with a fake
/// output whose calls block.
class OutputSlot {
 public:
  using Clock = CallGate::Clock;

  /// What the clock thread needs to know about the output without calling it,
  /// captured under the gate whenever a control path has had the output. The
  /// engine caches this under its own mutex; `generation` is how the clock
  /// knows the object it planned a tick for is still the one in the slot.
  struct Description {
    uint64_t generation = 0;
    bool present = false;
    PixelFormat pixelFormat = PixelFormat::kBGRA;
    bool straightAlpha = false;
    bool wantsAudio = false;
  };

  /// A deadline shared by several calls, but never less than `minimum` from
  /// now for this one. The clock and state() each give all their outputs one
  /// budget; without the floor, slow calls early in the list used it up and
  /// the later outputs got a single try — which a status() holding a gate for
  /// a millisecond was enough to lose.
  static Clock::time_point atLeast(Clock::time_point shared,
                                   Clock::duration minimum) {
    const auto floor = Clock::now() + minimum;
    return shared > floor ? shared : floor;
  }

  explicit OutputSlot(std::unique_ptr<IOutput> output,
                      Clock::duration wedgedAfter = std::chrono::seconds(1))
      : gate_(wedgedAfter), output_(std::move(output)) {
    refreshDescription();
  }

  OutputSlot(const OutputSlot&) = delete;
  OutputSlot& operator=(const OutputSlot&) = delete;

  /// Calls fn(IOutput&) holding the gate, if the gate can be had by `deadline`
  /// and the slot still holds the object `generation` was taken from. Returns
  /// whether fn ran.
  template <typename Fn>
  bool call(Clock::time_point deadline, uint64_t generation, Fn&& fn) {
    bool ran = false;
    gate_.tryRun(deadline, [&] {
      if (output_ != nullptr && generation_ == generation) {
        fn(*output_);
        ran = true;
      }
    });
    return ran;
  }

  enum class Result { kRan, kBusy, kEmpty };

  /// For the control paths that start or stop the output in the slot:
  /// fn(IOutput&) holding the gate. The object stays the same, so the
  /// generation does not move and a tick the clock has already planned for it
  /// still goes out — re-enabling an output that is already running costs no
  /// frame.
  template <typename Fn>
  Result exclusive(Clock::time_point deadline, Fn&& fn) {
    bool empty = false;
    if (!gate_.tryRun(deadline, [&] {
          if (output_ == nullptr) {
            empty = true;
            return;
          }
          fn(*output_);
        })) {
      return Result::kBusy;
    }
    return empty ? Result::kEmpty : Result::kRan;
  }

  /// For the control paths that replace or destroy the output:
  /// fn(std::unique_ptr<IOutput>&) holding the gate, the only place the pointer
  /// is handed out. The generation always moves on afterwards, whatever fn did:
  /// a replacement can land at the address of the object it replaced, so
  /// comparing pointers could not tell. Returns whether fn ran.
  template <typename Fn>
  bool replace(Clock::time_point deadline, Fn&& fn) {
    return gate_.tryRun(deadline, [&] {
      fn(output_);
      ++generation_;
      refreshDescription();
    });
  }

  /// A fresh status() if the gate can be had by `deadline`, or else the last
  /// one that completed. That is marked "stale", with how long the call in the
  /// way has been running, only once the call counts as stuck (see
  /// CallGate::wedged): an output that was merely busy for a moment — opening a
  /// card, or another reader got there first — is not "not answering", and
  /// saying so would flicker the control page red. An empty slot reports an
  /// empty object.
  json::Value status(Clock::time_point deadline) {
    json::Value fresh;
    bool got = false;
    bool empty = false;
    gate_.tryRun(deadline, [&] {
      if (output_ == nullptr) {
        empty = true;
        return;
      }
      fresh = output_->status();
      got = true;
    });
    std::lock_guard<std::mutex> lock(infoMutex_);
    if (got) {
      fresh.set("stale", json::Value(false));
      lastStatus_ = fresh;
      haveStatus_ = true;
      return fresh;
    }
    if (empty) {
      return json::Value::object();
    }
    json::Value value = haveStatus_ ? lastStatus_ : json::Value::object();
    const bool stuck = gate_.wedged();
    value.set("stale", json::Value(stuck));
    if (stuck) {
      value.set("driver_busy_ms", json::Value(busyMillis()));
    }
    return value;
  }

  Description description() const {
    std::lock_guard<std::mutex> lock(infoMutex_);
    return description_;
  }

  /// How long the call holding the gate has been running, in milliseconds.
  int64_t busyMillis() const { return gate_.busyNanos() / 1'000'000; }

  /// Whether that call has been running long enough to count as stuck.
  bool wedged() const { return gate_.wedged(); }

  /// How many callers gave up waiting for the gate.
  int64_t refusals() const { return gate_.refusals(); }

 private:
  /// Under the gate (or before the slot is shared). The description is fixed
  /// for an object's life, so only replace() needs to refresh it.
  void refreshDescription() {
    Description next;
    next.generation = generation_;
    next.present = output_ != nullptr;
    if (output_ != nullptr) {
      next.pixelFormat = output_->pixelFormat();
      next.straightAlpha = output_->wantsStraightAlpha();
      next.wantsAudio = output_->wantsAudio();
    }
    std::lock_guard<std::mutex> lock(infoMutex_);
    description_ = next;
  }

  CallGate gate_;
  std::unique_ptr<IOutput> output_;   ///< replaced or destroyed only under gate_
  uint64_t generation_ = 1;           ///< under gate_

  /// Guards the two copies below, which are read without the gate — that is
  /// their whole point. Never held across a call into the output.
  mutable std::mutex infoMutex_;
  Description description_;
  json::Value lastStatus_;
  bool haveStatus_ = false;
};

}  // namespace weblinked
