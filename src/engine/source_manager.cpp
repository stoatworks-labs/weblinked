#include "engine/source_manager.h"

#include <algorithm>
#include <utility>

#include "diag/diag.h"

namespace weblinked {

namespace {

/// How long a removal waits for the requests already inside a source. Longer
/// than a control request can legitimately take — up to 30 s queued behind
/// another change on that source (Engine's kControlWait), plus its own gate
/// waits — so that a removal behind ordinary traffic succeeds, and one behind
/// a request that will never come out fails rather than waiting with it.
constexpr auto kRemoveWait = std::chrono::seconds(40);

/// At shutdown the control API has already been stopped, and has waited for
/// its handlers; this is for the straggler that is stuck.
constexpr auto kShutdownWait = std::chrono::seconds(2);

}  // namespace

SourceManager::SourceManager() = default;

SourceManager::~SourceManager() { stop(); }

bool SourceManager::start(const AppConfig& config, const Options& options,
                          std::string& error) {
  {
    std::lock_guard<std::mutex> lock(optionsMutex_);
    options_ = options;
  }

  std::string firstError;
  size_t started = 0;
  for (const auto& source : config.sources) {
    std::string addError;
    if (add(source, addError)) {
      ++started;
      continue;
    }
    diag::error("source '%s' failed to start: %s", source.id.c_str(),
                addError.c_str());
    if (firstError.empty()) {
      firstError = addError;
    }
  }

  if (started == 0) {
    error = firstError.empty() ? "no sources configured" : firstError;
    return false;
  }
  if (!firstError.empty()) {
    // Some came up and some did not. Not a failure — the show goes on with what
    // works — but the caller gets the reason so it can be reported.
    error = firstError;
  }
  return true;
}

void SourceManager::stop() {
  std::vector<std::string> busy;
  auto taken = sources_.takeAll(SourceTable<Engine>::Clock::now() + kShutdownWait,
                                &busy);
  for (const auto& id : busy) {
    // Stopping it would destroy its browser under a request that is still
    // inside. At shutdown the watchdog in main.cpp is the backstop.
    diag::error("source '%s' still has a request inside it; not stopping it",
                id.c_str());
  }
  // Outside any lock: each stop() joins a clock thread, and nothing can reach
  // these engines any more — they are no longer in the table.
  for (auto& engine : taken) {
    if (engine != nullptr) {
      engine->stop();
    }
  }
}

void SourceManager::halt() {
  // Each source held open as withSource() holds it, so a concurrent remove()
  // cannot stop an engine while it is being halted. Nothing is removed.
  sources_.forEach([](const std::string&, Engine& engine) { engine.halt(); });
}

bool SourceManager::add(const SourceConfig& config, std::string& error) {
  SourceConfig prepared = config;
  prepared.ensurePreview();
  if (!prepared.validate(&error)) {
    return false;
  }
  if (sources_.has(prepared.id)) {
    error = "a source called '" + prepared.id + "' already exists";
    return false;
  }
  std::string cachePath;
  {
    std::lock_guard<std::mutex> lock(optionsMutex_);
    cachePath = options_.cachePath;
  }

  // Started before it is in the table. Creating a browser is the slowest thing
  // this class does, and nothing else should wait on it.
  auto engine = std::make_shared<Engine>();
  if (!engine->start(engineConfigFromSource(prepared, cachePath), error)) {
    engine->stop();
    return false;
  }
  // Checked again on the way in: another add may have claimed this id while the
  // browser started.
  if (!sources_.insert(prepared.id, engine)) {
    engine->stop();
    error = "a source called '" + prepared.id + "' already exists";
    return false;
  }

  diag::info("source '%s' started: %s at %s", prepared.id.c_str(),
             prepared.url.c_str(), prepared.format.toString().c_str());
  return true;
}

bool SourceManager::remove(const std::string& id, std::string& error) {
  std::shared_ptr<Engine> taken;
  switch (sources_.remove(id, kRemoveWait, &taken)) {
    case SourceTable<Engine>::Removal::kMissing:
      error = "no source called '" + id + "'";
      return false;
    case SourceTable<Engine>::Removal::kBusy:
      error = "source '" + id + "' was not removed: a request inside it has "
              "not finished (an output's driver may not be answering — see "
              "\"stale\" in /api/state)";
      diag::warn("%s", error.c_str());
      return false;
    case SourceTable<Engine>::Removal::kRemoved:
      break;
  }
  // Safe: it is out of the table, so withSource() can no longer hand it to
  // anyone, and every call that was inside it came out before remove()
  // returned.
  if (taken != nullptr) {
    taken->stop();
  }
  diag::info("source '%s' removed", id.c_str());
  return true;
}

SourceManager::Access SourceManager::withSource(
    const std::string& id, const std::function<void(Engine&)>& fn) const {
  return sources_.with(id, fn);
}

std::string SourceManager::primaryId() const { return sources_.firstId(); }

std::vector<std::string> SourceManager::ids() const { return sources_.ids(); }

bool SourceManager::has(const std::string& id) const { return sources_.has(id); }

size_t SourceManager::size() const { return sources_.size(); }

json::Value SourceManager::state() const {
  json::Value root = json::Value::object();
  json::Value list = json::Value::array();
  // Each engine's state() is bounded (see Engine::state), and none of them is
  // asked holding anything another source needs.
  sources_.forEach(
      [&list](const std::string&, Engine& engine) { list.push(engine.state()); });
  root.set("primary", json::Value(sources_.firstId()));
  root.set("sources", list);
  return root;
}

AppConfig SourceManager::configuration() const {
  AppConfig config;
  sources_.forEach([&config](const std::string&, Engine& engine) {
    config.sources.push_back(engine.configuration());
  });
  return config;
}

bool SourceManager::applyConfiguration(const AppConfig& config,
                                       std::string& error) {
  std::string firstError;
  const auto note = [&firstError](const std::string& message) {
    if (firstError.empty()) {
      firstError = message;
    }
  };

  // Removals first, so a source handing an SDI card or an NDI name to another
  // releases it before the new claimant tries to open it.
  for (const auto& id : ids()) {
    const bool wanted = std::any_of(
        config.sources.begin(), config.sources.end(),
        [&](const SourceConfig& candidate) { return candidate.id == id; });
    if (!wanted) {
      std::string removeError;
      if (!remove(id, removeError)) {
        note(removeError);
      }
    }
  }

  for (const auto& source : config.sources) {
    if (has(source.id)) {
      std::string applyError;
      // Held open for the whole reconcile: applyConfiguration can stop and
      // reopen every output, which must not race a removal.
      const auto access = withSource(source.id, [&](Engine& engine) {
        if (!engine.applyConfiguration(source, applyError)) {
          note(applyError);
        }
      });
      if (access == Access::kClosing) {
        note("source '" + source.id + "' is being removed");
      }
      continue;
    }
    std::string addError;
    if (!add(source, addError)) {
      note(addError);
    }
  }

  if (!firstError.empty()) {
    error = firstError;
    return false;
  }
  return true;
}

}  // namespace weblinked
