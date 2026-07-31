#include "core/source_config.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace weblinked {

const char* colourMatrixToString(ColourMatrix matrix) {
  switch (matrix) {
    case ColourMatrix::kBt601: return "601";
    case ColourMatrix::kBt709: return "709";
    case ColourMatrix::kAuto:  return "auto";
  }
  return "auto";
}

ColourMatrix colourMatrixFromString(const std::string& text) {
  if (text == "601" || text == "bt601") return ColourMatrix::kBt601;
  if (text == "709" || text == "bt709") return ColourMatrix::kBt709;
  return ColourMatrix::kAuto;
}

// --- OutputConfig -----------------------------------------------------------

json::Value OutputConfig::toJson() const {
  json::Value value = json::Value::object();
  value.set("kind", json::Value(kind));
  value.set("name", json::Value(name));
  if (kind == "decklink" || kind == "aja") {
    value.set("device_index", json::Value(deviceIndex));
  }
  if (options.isObject() && options.size() > 0) {
    value.set("options", options);
  }
  return value;
}

std::optional<OutputConfig> OutputConfig::fromJson(const json::Value& value,
                                                   std::string* error) {
  if (!value.isObject()) {
    if (error != nullptr) *error = "an output must be an object";
    return std::nullopt;
  }

  OutputConfig out;
  out.kind = value["kind"].asString();
  if (out.kind.empty()) {
    if (error != nullptr) *error = "an output needs a \"kind\"";
    return std::nullopt;
  }

  // Defaulting the name to the kind means a single-output source can be written
  // as {"kind":"ndi"} and still be addressable.
  out.name = value["name"].asString(out.kind);
  out.deviceIndex = value["device_index"].asInt(0);
  if (value["options"].isObject()) {
    out.options = value["options"];
  }
  return out;
}

// --- SourceConfig -----------------------------------------------------------

SourceConfig::SourceConfig() {
  // 1080p50 rather than a zeroed struct, so a config that omits the format is
  // still something a broadcast engineer would recognise.
  if (const auto parsed = VideoFormat::parse("1080p50")) {
    format = *parsed;
  }
}

json::Value SourceConfig::toJson() const {
  json::Value value = json::Value::object();
  value.set("id", json::Value(id));
  value.set("url", json::Value(url));
  value.set("format", json::Value(format.toString()));
  value.set("audio", json::Value(audioEnabled));
  value.set("matrix", json::Value(colourMatrixToString(matrix)));
  value.set("pacing", json::Value(externalPacing ? "external" : "internal"));
  value.set("interactive", json::Value(interactiveByDefault));
  value.set("popups", json::Value(popupPolicy));

  json::Value list = json::Value::array();
  for (const auto& output : outputs) {
    list.push(output.toJson());
  }
  value.set("outputs", list);
  return value;
}

std::optional<SourceConfig> SourceConfig::fromJson(const json::Value& value,
                                                   std::string* error) {
  if (!value.isObject()) {
    if (error != nullptr) *error = "a source must be an object";
    return std::nullopt;
  }

  SourceConfig out;
  out.id = value["id"].asString();
  out.url = value["url"].asString(out.url);

  if (value.has("format")) {
    const std::string text = value["format"].asString();
    const auto parsed = VideoFormat::parse(text);
    if (!parsed) {
      if (error != nullptr) {
        *error = "cannot parse format '" + text +
                 "' — try 1080p50, 720p59.94 or 1920x1080i25";
      }
      return std::nullopt;
    }
    out.format = *parsed;
  }

  if (value.has("audio")) {
    out.audioEnabled = value["audio"].asBool(true);
  }
  if (value.has("matrix")) {
    out.matrix = colourMatrixFromString(value["matrix"].asString());
  }
  if (value.has("pacing")) {
    out.externalPacing = value["pacing"].asString("external") != "internal";
  }
  if (value.has("interactive")) {
    out.interactiveByDefault = value["interactive"].asBool(true);
  }
  if (value.has("popups")) {
    out.popupPolicy = value["popups"].asString("navigate");
  }
  if (value.has("preview")) {
    out.wantPreview = value["preview"].asBool(true);
  }

  if (value.has("outputs")) {
    const json::Value& list = value["outputs"];
    if (!list.isArray()) {
      if (error != nullptr) *error = "\"outputs\" must be an array";
      return std::nullopt;
    }
    for (size_t i = 0; i < list.size(); ++i) {
      std::string outputError;
      const auto output = OutputConfig::fromJson(list.at(i), &outputError);
      if (!output) {
        if (error != nullptr) {
          *error = "outputs[" + std::to_string(i) + "]: " + outputError;
        }
        return std::nullopt;
      }
      out.outputs.push_back(*output);
    }
  }

  return out;
}

bool SourceConfig::validate(std::string* error) const {
  const auto fail = [&](const std::string& message) {
    if (error != nullptr) *error = message;
    return false;
  };

  if (id.empty()) {
    return fail("a source needs an \"id\"");
  }
  // The id appears in URLs and OSC addresses, so keep it to characters that
  // survive both without escaping.
  for (const char c : id) {
    const bool allowed = std::isalnum(static_cast<unsigned char>(c)) || c == '-' ||
                         c == '_' || c == '.';
    if (!allowed) {
      return fail("source id '" + id +
                  "' may only contain letters, digits, '-', '_' and '.'");
    }
  }

  if (format.width <= 0 || format.height <= 0) {
    return fail("source '" + id + "' has an empty raster");
  }
  if (format.width % 2 != 0) {
    return fail("source '" + id + "' has an odd width, which 4:2:2 cannot carry");
  }
  if (format.rate.numerator <= 0 || format.rate.denominator <= 0) {
    return fail("source '" + id + "' has an invalid frame rate");
  }

  if (popupPolicy != "navigate" && popupPolicy != "block") {
    return fail("source '" + id + "' has an unknown popup policy '" +
                popupPolicy + "' — expected \"navigate\" or \"block\"");
  }

  std::set<std::string> names;
  for (const auto& output : outputs) {
    if (output.name.empty()) {
      return fail("source '" + id + "' has an output with no name");
    }
    if (!names.insert(output.name).second) {
      return fail("source '" + id + "' has two outputs called '" + output.name +
                  "'");
    }
  }
  return true;
}

void SourceConfig::ensurePreview() {
  if (!wantPreview) {
    return;
  }
  const bool present = std::any_of(
      outputs.begin(), outputs.end(),
      [](const OutputConfig& output) { return output.kind == "preview"; });
  if (present) {
    return;
  }
  OutputConfig preview;
  preview.kind = "preview";
  preview.name = "preview";
  preview.options.set("factor", json::Value(4));
  outputs.insert(outputs.begin(), preview);
}

// --- AppConfig --------------------------------------------------------------

json::Value AppConfig::toJson() const {
  json::Value value = json::Value::object();

  json::Value control = json::Value::object();
  control.set("http_bind", json::Value(httpBind));
  control.set("http_port", json::Value(httpPort));
  if (!httpToken.empty()) {
    control.set("http_token", json::Value(httpToken));
  }
  control.set("osc_enabled", json::Value(oscEnabled));
  control.set("osc_bind", json::Value(oscBind));
  control.set("osc_port", json::Value(oscPort));
  value.set("control", control);

  json::Value list = json::Value::array();
  for (const auto& source : sources) {
    list.push(source.toJson());
  }
  value.set("sources", list);
  return value;
}

std::optional<AppConfig> AppConfig::fromJson(const json::Value& value,
                                             std::string* error) {
  if (!value.isObject()) {
    if (error != nullptr) *error = "the configuration must be an object";
    return std::nullopt;
  }

  AppConfig out;

  if (value.has("control")) {
    const json::Value& control = value["control"];
    out.httpBind = control["http_bind"].asString(out.httpBind);
    out.httpPort = control["http_port"].asInt(out.httpPort);
    out.httpToken = control["http_token"].asString(out.httpToken);
    if (control.has("osc_enabled")) {
      out.oscEnabled = control["osc_enabled"].asBool(true);
    }
    out.oscBind = control["osc_bind"].asString(out.oscBind);
    out.oscPort = control["osc_port"].asInt(out.oscPort);
  }

  if (!value.has("sources")) {
    if (error != nullptr) *error = "the configuration needs a \"sources\" array";
    return std::nullopt;
  }
  const json::Value& list = value["sources"];
  if (!list.isArray()) {
    if (error != nullptr) *error = "\"sources\" must be an array";
    return std::nullopt;
  }

  std::set<std::string> ids;
  for (size_t i = 0; i < list.size(); ++i) {
    std::string sourceError;
    auto source = SourceConfig::fromJson(list.at(i), &sourceError);
    if (!source) {
      if (error != nullptr) {
        *error = "sources[" + std::to_string(i) + "]: " + sourceError;
      }
      return std::nullopt;
    }
    // An id is optional in the file and generated when missing, so a quick
    // hand-written config does not need to invent names.
    if (source->id.empty()) {
      source->id = "source" + std::to_string(i + 1);
    }
    if (!source->validate(&sourceError)) {
      if (error != nullptr) {
        *error = "sources[" + std::to_string(i) + "]: " + sourceError;
      }
      return std::nullopt;
    }
    if (!ids.insert(source->id).second) {
      if (error != nullptr) {
        *error = "two sources share the id '" + source->id + "'";
      }
      return std::nullopt;
    }
    source->ensurePreview();
    out.sources.push_back(*source);
  }

  return out;
}

std::optional<AppConfig> AppConfig::parse(const std::string& text,
                                          std::string* error) {
  std::string parseError;
  const auto value = json::parse(text, &parseError);
  if (!value) {
    if (error != nullptr) *error = "not valid JSON: " + parseError;
    return std::nullopt;
  }
  return fromJson(*value, error);
}

}  // namespace weblinked
