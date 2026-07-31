#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/pixel_convert.h"
#include "core/video_format.h"

namespace weblinked {

/// How a single output is configured, as pure data.
///
/// Mirrors OutputSpec, which lives in the outputs layer. Kept separate because
/// this one has to be parseable and testable without linking CEF or any vendor
/// SDK; the engine converts between the two.
struct OutputConfig {
  std::string kind;              ///< "ndi" | "omt" | "decklink" | "aja" | "preview"
  std::string name;
  int deviceIndex = 0;
  json::Value options = json::Value::object();

  json::Value toJson() const;
  static std::optional<OutputConfig> fromJson(const json::Value& value,
                                              std::string* error = nullptr);
};

/// One complete pipeline: a page, a format, and where it goes.
///
/// This is the unit the source manager multiplies. It is deliberately a plain
/// value type in `core` with no engine dependency, so a configuration file can
/// be parsed, validated and reported on before anything expensive is created —
/// and so all of that is unit-testable in a build with no browser in it.
struct SourceConfig {
  /// Stable handle used by the API and OSC. Must be unique within a session.
  std::string id;
  std::string url = "about:blank";
  VideoFormat format;
  std::vector<OutputConfig> outputs;
  bool audioEnabled = true;
  ColourMatrix matrix = ColourMatrix::kAuto;
  /// True for clock-driven frames (the default), false for Chromium's own timer.
  bool externalPacing = true;
  /// Whether the control page arms its preview for input as soon as it loads.
  bool interactiveByDefault = true;
  /// "navigate" — a link that asks for a new tab loads here instead — or
  /// "block". There is no third option: a windowless browser cannot own a
  /// popup window, which is what used to take the application down.
  std::string popupPolicy = "navigate";
  /// Adds a preview output automatically if the list has none. The control page
  /// needs one to show anything.
  bool wantPreview = true;

  SourceConfig();

  json::Value toJson() const;
  static std::optional<SourceConfig> fromJson(const json::Value& value,
                                              std::string* error = nullptr);

  /// Checks what parsing alone cannot: a usable id, a sane raster, no duplicate
  /// output names. Returns false and sets `error` on the first problem.
  bool validate(std::string* error) const;

  /// Inserts a preview output at the front if `wantPreview` and none is present.
  void ensurePreview();
};

/// A whole configuration file: several sources plus the control surface's
/// settings.
struct AppConfig {
  std::vector<SourceConfig> sources;
  std::string httpBind = "127.0.0.1";
  int httpPort = 7654;
  std::string httpToken;
  bool oscEnabled = true;
  std::string oscBind = "0.0.0.0";
  int oscPort = 7655;

  json::Value toJson() const;
  static std::optional<AppConfig> fromJson(const json::Value& value,
                                           std::string* error = nullptr);
  /// Parses a file's contents. Rejects duplicate source ids.
  static std::optional<AppConfig> parse(const std::string& text,
                                        std::string* error = nullptr);
};

/// Turns "ndi", "601" etc. into their enums and back, so the config file and the
/// HTTP API agree on spelling.
const char* colourMatrixToString(ColourMatrix matrix);
ColourMatrix colourMatrixFromString(const std::string& text);

}  // namespace weblinked
