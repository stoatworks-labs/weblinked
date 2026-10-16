#pragma once

#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/source_config.h"
#include "core/source_table.h"
#include "engine/engine.h"

namespace weblinked {

/// Owns several independent pipelines and hands them out by id.
///
/// Each source is a whole Engine — its own browser, its own clock thread, its
/// own outputs — and they share nothing but this map and Chromium's process.
/// That independence is the point: a page that hangs, a card that will not open
/// or a raster change on one source must not touch the others, and none of them
/// can, because no engine ever learns that the others exist.
///
/// **Lifetime is the hard part.** Engine reads `browser_` without a lock and
/// `Engine::stop()` destroys it, which was perfectly safe while there was
/// exactly one engine torn down at exit and nothing else running. A source that
/// can be removed from a running show breaks that assumption: an HTTP request
/// holding a bare `Engine*` would be walking into a destructor. So every call
/// that reaches into an engine goes through withSource(), which holds that
/// source open for the duration, and a removal waits for the calls already
/// inside to come out before it stops anything.
///
/// It used to be one shared_mutex held across every call, and that is how a
/// wedged driver in one source could stop them all (issue #12): see
/// SourceTable, which now holds the map. Its own lock is only ever held to copy
/// a pointer, so nothing one source does can make a request to another wait.
///
/// The expensive halves of add() and remove() — starting a browser, joining a
/// clock thread — deliberately happen outside the lock. Holding it across those
/// would stall every other source's control surface for as long as CEF took, and
/// an operator adding a fourth source must not freeze the panel driving the
/// three that are already on air.
class SourceManager {
 public:
  struct Options {
    /// Chromium's cache directory, shared by every source. Not per-source: one
    /// process gets one profile, and CEF will not have it otherwise.
    std::string cachePath;
  };

  SourceManager();
  ~SourceManager();

  SourceManager(const SourceManager&) = delete;
  SourceManager& operator=(const SourceManager&) = delete;

  /// Starts every source in `config`, in order.
  ///
  /// A source that fails to start is logged and skipped rather than aborting the
  /// rest, for the same reason a failed output does not stop an engine: on site,
  /// the feeds that *can* come up should. Returns false only if none came up at
  /// all, with `error` naming the first failure.
  bool start(const AppConfig& config, const Options& options, std::string& error);

  /// Stops and destroys every source. Safe to call twice.
  void stop();

  /// Engine::halt() on every source: clocks parked, outputs closed, browsers
  /// left alone and every source left in the map. The first half of a
  /// shutdown, run while the message loop is still up — see beginShutdown() in
  /// main.cpp. stop() still has to follow.
  void halt();

  /// Adds and starts one source. Fails on a duplicate or invalid id.
  bool add(const SourceConfig& config, std::string& error);

  /// Stops and removes one source. Fails if there is no such id, or if a
  /// request inside it has not come out within kRemoveWait — then nothing is
  /// changed, and the source stays.
  bool remove(const std::string& id, std::string& error);

  using Access = SourceTable<Engine>::Access;

  /// Runs `fn` against the named source, holding it open for the call.
  ///
  /// This is the only sanctioned way to touch an engine. kMissing if there is
  /// no such source, kClosing if it is being removed; `fn` is not called in
  /// either case.
  ///
  /// `fn` must not remove its own source: the removal would wait for `fn`.
  Access withSource(const std::string& id,
                    const std::function<void(Engine&)>& fn) const;

  /// The id every un-addressed request falls back to — the first source, which
  /// for a command-line launch is the only one. This is what keeps the
  /// single-source API working unchanged.
  std::string primaryId() const;

  std::vector<std::string> ids() const;
  bool has(const std::string& id) const;
  size_t size() const;

  /// `{ "sources": [ ... ], "primary": "main" }`, each entry an Engine::state()
  /// with its id already in it.
  json::Value state() const;

  /// Every source as saveable data, plus nothing else — the control surface's
  /// own settings belong to ControlApi and are merged in by the caller.
  AppConfig configuration() const;

  /// Reconciles the running set against `config`: sources that have gone are
  /// stopped, new ones started, and the ones that remain are handed to
  /// Engine::applyConfiguration so an unchanged output keeps running.
  ///
  /// Returns false with the first failure in `error`, having applied everything
  /// else — a settings file that half-applies and says so is more use than one
  /// that refuses wholesale because a single card is missing.
  bool applyConfiguration(const AppConfig& config, std::string& error);

 private:
  SourceTable<Engine> sources_;

  mutable std::mutex optionsMutex_;
  Options options_;
};

}  // namespace weblinked
