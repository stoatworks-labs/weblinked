#pragma once

#include <optional>
#include <string>

#include "core/source_config.h"

namespace weblinked {

/// Where the settings page's changes are kept between runs.
///
/// Separate from `diag`'s log directory on purpose: logs are disposable and go
/// to the platform's log location, whereas this is the show's configuration and
/// belongs where a user would expect to back it up.
///
///   macOS   ~/Library/Application Support/WebLinked/settings.json
///   Windows %APPDATA%\WebLinked\settings.json
///   Linux   $XDG_CONFIG_HOME/WebLinked/settings.json, else ~/.config/...
///
/// The file holds an AppConfig, the same shape the API serves — so a settings
/// file can be written by hand, and what the page saves can be read back by a
/// person. Nothing here links CEF, so all of it is unit-testable.
namespace settings {

/// The directory the settings file lives in. Not created by this call.
std::string directory();

/// `directory()/settings.json`. Overridden by $WEBLINKED_SETTINGS.
std::string defaultPath();

/// Reads and parses `path`. Returns nothing — with `error` set — for a missing
/// file, malformed JSON, or a configuration that fails validation. A missing
/// file is deliberately not distinguished: every caller treats it the same way,
/// which is to carry on with the command line's own settings.
std::optional<AppConfig> load(const std::string& path, std::string* error = nullptr);

/// Writes `config` to `path`, creating the directory if needed.
///
/// Writes to a temporary file and renames, so an interrupted save cannot leave
/// a truncated settings file behind — which would otherwise mean the next
/// launch comes up with no outputs at all.
bool save(const AppConfig& config, const std::string& path, std::string* error = nullptr);

}  // namespace settings
}  // namespace weblinked
