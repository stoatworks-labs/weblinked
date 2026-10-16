#pragma once

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace weblinked {

/// Named things that requests reach into, and that can be removed while those
/// requests are running: SourceManager's engines.
///
/// It replaced a std::shared_mutex held for the whole of every call into an
/// engine. That kept a removal from destroying an engine with a request inside
/// it (CLAUDE.md rule 12), but a request that never came out — one stopping a
/// DeckLink whose driver had deadlocked, issue #12 — held the lock for good,
/// the next add or remove queued for the exclusive side, and libc++'s
/// shared_mutex then queues every new reader behind a waiting writer. Every
/// source's /api/state, every OSC message and the tray's source count on the UI
/// thread stopped with it. One wedged call in one source took them all.
///
/// So the table's own mutex is held only to copy a pointer, never across a
/// call, and each entry counts the calls inside it instead:
///
/// - with() finds the entry, lets go of the table, enters the entry and calls
///   fn. Nothing it does can make another source wait.
/// - remove() closes the entry to new calls, waits up to a deadline for the
///   calls inside to come out, and only then takes it out of the table and
///   hands it back to be stopped — so no thing is stopped with a call inside
///   it. If the calls do not come out in time the entry is reopened and the
///   removal reports busy, having changed nothing. Taking it out anyway would
///   leave something running — its NDI sender advertised, its card claimed —
///   that nothing could reach to stop.
///
/// Header-only and free of CEF so the tests can drive it with a fake engine.
template <typename T>
class SourceTable {
 public:
  using Clock = std::chrono::steady_clock;

  enum class Access { kDone, kMissing, kClosing };
  enum class Removal { kRemoved, kMissing, kBusy };

  /// False, changing nothing, if `id` is already present.
  bool insert(const std::string& id, std::shared_ptr<T> value) {
    auto entry = std::make_shared<Entry>();
    entry->id = id;
    entry->value = std::move(value);
    std::lock_guard<std::mutex> lock(mutex_);
    if (findLocked(id) != nullptr) {
      return false;
    }
    entries_.push_back(std::move(entry));
    return true;
  }

  /// Calls fn on the entry called `id`, holding it open but not the table.
  /// kClosing while a removal of it is under way.
  Access with(const std::string& id, const std::function<void(T&)>& fn) const {
    std::shared_ptr<Entry> entry;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entry = findLocked(id);
    }
    if (entry == nullptr || entry->value == nullptr) {
      return Access::kMissing;
    }
    if (!entry->enter()) {
      return Access::kClosing;
    }
    Use use(*entry);
    fn(*entry->value);
    return Access::kDone;
  }

  /// fn(id, value) for every entry not being removed, in order, each held open
  /// as with() holds it and none under the table's lock.
  void forEach(const std::function<void(const std::string&, T&)>& fn) const {
    for (const auto& entry : snapshot()) {
      if (entry->value == nullptr || !entry->enter()) {
        continue;
      }
      Use use(*entry);
      fn(entry->id, *entry->value);
    }
  }

  /// Takes `id` out, once no call is inside it, and hands it back through
  /// `removed` for the caller to stop. See the class note for kBusy.
  Removal remove(const std::string& id, Clock::duration wait,
                 std::shared_ptr<T>* removed) {
    std::shared_ptr<Entry> entry;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entry = findLocked(id);
    }
    if (entry == nullptr) {
      return Removal::kMissing;
    }
    switch (entry->close(Clock::now() + wait)) {
      case Closed::kAlready:
        return Removal::kBusy;  // another removal has it; not ours to reopen
      case Closed::kTimedOut:
        entry->reopen();
        return Removal::kBusy;
      case Closed::kEmpty:
        break;
    }
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entries_.erase(std::remove(entries_.begin(), entries_.end(), entry),
                     entries_.end());
    }
    if (removed != nullptr) {
      *removed = entry->value;
    }
    return Removal::kRemoved;
  }

  /// Takes everything out, for shutdown: each entry is closed and given until
  /// `deadline` for its calls to come out. Those that came out are returned to
  /// be stopped; the ids of any that did not are put in `busy`, and those are
  /// left alone rather than stopped under a call.
  std::vector<std::shared_ptr<T>> takeAll(Clock::time_point deadline,
                                          std::vector<std::string>* busy) {
    std::vector<std::shared_ptr<Entry>> entries;
    {
      std::lock_guard<std::mutex> lock(mutex_);
      entries.swap(entries_);
    }
    std::vector<std::shared_ptr<T>> idle;
    for (const auto& entry : entries) {
      if (entry->close(deadline) == Closed::kEmpty) {
        idle.push_back(entry->value);
      } else if (busy != nullptr) {
        busy->push_back(entry->id);
      }
    }
    return idle;
  }

  std::vector<std::string> ids() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::string> result;
    result.reserve(entries_.size());
    for (const auto& entry : entries_) {
      result.push_back(entry->id);
    }
    return result;
  }

  bool has(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return findLocked(id) != nullptr;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
  }

  /// The first entry's id, or empty.
  std::string firstId() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.empty() ? std::string() : entries_.front()->id;
  }

 private:
  enum class Closed { kEmpty, kTimedOut, kAlready };

  struct Entry {
    std::string id;
    std::shared_ptr<T> value;

    mutable std::mutex mutex;
    mutable std::condition_variable idle;
    mutable int users = 0;
    mutable bool closing = false;

    bool enter() const {
      std::lock_guard<std::mutex> lock(mutex);
      if (closing) {
        return false;
      }
      ++users;
      return true;
    }
    void leave() const {
      {
        std::lock_guard<std::mutex> lock(mutex);
        --users;
      }
      idle.notify_all();
    }
    /// Closes to new calls and waits for the ones inside to come out.
    Closed close(Clock::time_point deadline) const {
      std::unique_lock<std::mutex> lock(mutex);
      if (closing) {
        return Closed::kAlready;
      }
      closing = true;
      return idle.wait_until(lock, deadline, [this] { return users == 0; })
                 ? Closed::kEmpty
                 : Closed::kTimedOut;
    }
    void reopen() const {
      std::lock_guard<std::mutex> lock(mutex);
      closing = false;
    }
  };

  /// Leaves the entry however fn does.
  struct Use {
    const Entry& entry;
    explicit Use(const Entry& e) : entry(e) {}
    ~Use() { entry.leave(); }
    Use(const Use&) = delete;
    Use& operator=(const Use&) = delete;
  };

  std::shared_ptr<Entry> findLocked(const std::string& id) const {
    for (const auto& entry : entries_) {
      if (entry->id == id) {
        return entry;
      }
    }
    return nullptr;
  }

  std::vector<std::shared_ptr<Entry>> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_;
  }

  /// Guards entries_ and nothing else; held only to copy or edit the list.
  mutable std::mutex mutex_;
  std::vector<std::shared_ptr<Entry>> entries_;
};

}  // namespace weblinked
