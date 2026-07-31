#include "core/settings_store.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <system_error>

namespace weblinked::settings {
namespace {

constexpr const char* kAppName = "WebLinked";

std::filesystem::path homeRelative(const char* first, const char* second) {
  const char* home = std::getenv("HOME");
  std::filesystem::path root = home != nullptr
                                   ? std::filesystem::path(home)
                                   : std::filesystem::temp_directory_path();
  root /= first;
  if (second != nullptr) {
    root /= second;
  }
  return root;
}

}  // namespace

std::string directory() {
#if defined(_WIN32)
  // APPDATA rather than LOCALAPPDATA, which is where diag puts logs: settings
  // are the kind of thing a roaming profile should carry to the next machine.
  const char* base = std::getenv("APPDATA");
  std::filesystem::path root = base != nullptr
                                   ? std::filesystem::path(base)
                                   : std::filesystem::temp_directory_path();
  return (root / kAppName).string();
#elif defined(__APPLE__)
  return (homeRelative("Library", "Application Support") / kAppName).string();
#else
  if (const char* xdg = std::getenv("XDG_CONFIG_HOME")) {
    return (std::filesystem::path(xdg) / kAppName).string();
  }
  return (homeRelative(".config", nullptr) / kAppName).string();
#endif
}

std::string profileDirectory(int controlPort) {
  return (std::filesystem::path(directory()) / "profiles" /
          std::to_string(controlPort))
      .string();
}

std::string defaultPath() {
  if (const char* fromEnv = std::getenv("WEBLINKED_SETTINGS")) {
    if (*fromEnv != '\0') {
      return fromEnv;
    }
  }
  return (std::filesystem::path(directory()) / "settings.json").string();
}

std::optional<AppConfig> load(const std::string& path, std::string* error) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    if (error != nullptr) {
      *error = "cannot open " + path;
    }
    return std::nullopt;
  }
  std::ostringstream text;
  text << file.rdbuf();
  return AppConfig::parse(text.str(), error);
}

bool save(const AppConfig& config, const std::string& path, std::string* error) {
  const std::filesystem::path target(path);
  std::error_code code;
  if (target.has_parent_path()) {
    std::filesystem::create_directories(target.parent_path(), code);
    if (code) {
      if (error != nullptr) {
        *error = "cannot create " + target.parent_path().string() + ": " +
                 code.message();
      }
      return false;
    }
  }

  // Write beside the target, then rename over it. A power cut or a kill during
  // the write then costs the *new* settings rather than the old ones — losing
  // an edit is recoverable, coming up with an empty output list is not.
  const std::filesystem::path temporary = target.string() + ".tmp";
  {
    std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
    if (!file) {
      if (error != nullptr) {
        *error = "cannot write " + temporary.string();
      }
      return false;
    }
    file << config.toJson().serialize(true) << "\n";
    if (!file) {
      if (error != nullptr) {
        *error = "failed while writing " + temporary.string();
      }
      return false;
    }
  }

  std::filesystem::rename(temporary, target, code);
  if (code) {
    // Windows will not rename over an existing file on every filesystem.
    std::filesystem::remove(target, code);
    std::filesystem::rename(temporary, target, code);
  }
  if (code) {
    if (error != nullptr) {
      *error = "cannot replace " + target.string() + ": " + code.message();
    }
    std::filesystem::remove(temporary, code);
    return false;
  }
  return true;
}

}  // namespace weblinked::settings
