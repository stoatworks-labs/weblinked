#pragma once

#include <optional>
#include <string>
#include <vector>

#include "core/json.h"
#include "core/pixel_convert.h"
#include "core/video_format.h"

namespace weblinked {

/// What fills the parts of the raster the page has not painted, on the way to
/// one output.
///
/// Per output rather than per source, because the same page usually wants to
/// leave here twice: transparent down an SDI keyer or an NDI feed with alpha,
/// and composited over a flat colour for a switcher that only has a chroma
/// keyer. The browser is left painting transparent either way — the composite
/// happens in the frame path, so one paint still serves every output.
///
/// The colour is kept even while `opaque` is false, so an operator who toggles
/// back to transparent to check a key does not lose the green they picked.
struct OutputBackground {
  /// False keeps the page's own transparency, which is what every output did
  /// before this existed and remains the default.
  bool opaque = false;
  /// Chroma green (#00b140) — the value most switchers' keyers are set up for,
  /// so an operator who turns the background on and changes nothing else gets
  /// something usable rather than black.
  uint8_t red = 0x00;
  uint8_t green = 0xb1;
  uint8_t blue = 0x40;

  bool operator==(const OutputBackground& other) const {
    return opaque == other.opaque && red == other.red && green == other.green &&
           blue == other.blue;
  }
  bool operator!=(const OutputBackground& other) const {
    return !(*this == other);
  }

  /// 0x00RRGGBB. The engine keys its composited frames on this, so two outputs
  /// asking for the same green share one composite.
  uint32_t packed() const {
    return (static_cast<uint32_t>(red) << 16) |
           (static_cast<uint32_t>(green) << 8) | blue;
  }

  /// "#rrggbb", lower case — what an <input type="color"> both emits and
  /// expects, so the control page needs no conversion of its own.
  std::string toHex() const;

  /// Accepts "#rrggbb" or "rrggbb", either case. Returns false and leaves the
  /// colour untouched on anything else, so a typo in a settings file does not
  /// silently turn an output black.
  bool setHex(const std::string& text);
};

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
  /// Not in `options`: `options` is backend-specific and a change to it
  /// reopens the device, whereas the background is composited by the engine and
  /// must be changeable on air without dropping a frame.
  OutputBackground background;

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
