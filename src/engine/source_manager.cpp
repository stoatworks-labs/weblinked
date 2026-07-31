#include "engine/source_manager.h"

#include <algorithm>
#include <utility>

#include "diag/diag.h"

namespace weblinked {

SourceManager::SourceManager() = default;

SourceManager::~SourceManager() { stop(); }

const SourceManager::Entry* SourceManager::findLocked(
    const std::string& id) const {
  for (const auto& entry : sources_) {
    if (entry.id == id) {
      return &entry;
    }
  }
  return nullptr;
}

bool SourceManager::start(const AppConfig& config, const Options& options,
                          std::string& error) {
  {
    std::unique_lock<std::shared_mutex> lock(mutex_);
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
  std::vector<Entry> taken;
  {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    taken = std::move(sources_);
    sources_.clear();
  }
  // Outside the lock: each stop() joins a clock thread, and nothing can reach
  // these engines any more — they are no longer in the map.
  for (auto& entry : taken) {
    if (entry.engine != nullptr) {
      entry.engine->stop();
    }
  }
}

bool SourceManager::add(const SourceConfig& config, std::string& error) {
  SourceConfig prepared = config;
  prepared.ensurePreview();
  if (!prepared.validate(&error)) {
    return false;
  }

  std::string cachePath;
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (findLocked(prepared.id) != nullptr) {
      error = "a source called '" + prepared.id + "' already exists";
      return false;
    }
    cachePath = options_.cachePath;
  }

  // Started outside the lock. Creating a browser is the slowest thing this class
  // does, and doing it under the exclusive lock would stall every other source's
  // control surface for the duration.
  auto engine = std::make_shared<Engine>();
  if (!engine->start(engineConfigFromSource(prepared, cachePath), error)) {
    engine->stop();
    return false;
  }

  {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    // Re-checked: the shared lock was dropped while the browser started, so
    // another thread may have claimed this id in the meantime.
    if (findLocked(prepared.id) != nullptr) {
      lock.unlock();
      engine->stop();
      error = "a source called '" + prepared.id + "' already exists";
      return false;
    }
    sources_.push_back(Entry{prepared.id, std::move(engine)});
  }

  diag::info("source '%s' started: %s at %s", prepared.id.c_str(),
             prepared.url.c_str(), prepared.format.toString().c_str());
  return true;
}

bool SourceManager::remove(const std::string& id, std::string& error) {
  std::shared_ptr<Engine> taken;
  {
    std::unique_lock<std::shared_mutex> lock(mutex_);
    const auto it = std::find_if(
        sources_.begin(), sources_.end(),
        [&](const Entry& entry) { return entry.id == id; });
    if (it == sources_.end()) {
      error = "no source called '" + id + "'";
      return false;
    }
    taken = std::move(it->engine);
    sources_.erase(it);
  }
  // Outside the lock, and safe: the entry is gone from the map, so withSource()
  // can no longer hand this engine to anyone, and every call that was already
  // inside it finished before the exclusive lock above was granted.
  if (taken != nullptr) {
    taken->stop();
  }
  diag::info("source '%s' removed", id.c_str());
  return true;
}

bool SourceManager::withSource(const std::string& id,
                               const std::function<void(Engine&)>& fn) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  const Entry* entry = findLocked(id);
  if (entry == nullptr || entry->engine == nullptr) {
    return false;
  }
  fn(*entry->engine);
  return true;
}

std::string SourceManager::primaryId() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return sources_.empty() ? std::string() : sources_.front().id;
}

std::vector<std::string> SourceManager::ids() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  std::vector<std::string> result;
  result.reserve(sources_.size());
  for (const auto& entry : sources_) {
    result.push_back(entry.id);
  }
  return result;
}

bool SourceManager::has(const std::string& id) const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return findLocked(id) != nullptr;
}

size_t SourceManager::size() const {
  std::shared_lock<std::shared_mutex> lock(mutex_);
  return sources_.size();
}

json::Value SourceManager::state() const {
  json::Value root = json::Value::object();
  json::Value list = json::Value::array();
  {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& entry : sources_) {
      if (entry.engine != nullptr) {
        list.push(entry.engine->state());
      }
    }
    root.set("primary", json::Value(sources_.empty()
                                        ? std::string()
                                        : sources_.front().id));
  }
  root.set("sources", list);
  return root;
}

AppConfig SourceManager::configuration() const {
  AppConfig config;
  std::shared_lock<std::shared_mutex> lock(mutex_);
  config.sources.reserve(sources_.size());
  for (const auto& entry : sources_) {
    if (entry.engine != nullptr) {
      config.sources.push_back(entry.engine->configuration());
    }
  }
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
      withSource(source.id, [&](Engine& engine) {
        if (!engine.applyConfiguration(source, applyError)) {
          note(applyError);
        }
      });
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
