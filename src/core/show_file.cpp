#include "core/show_file.h"

#include <algorithm>
#include <set>

namespace weblinked::show {
namespace {

bool fail(std::string* error, const std::string& message) {
  if (error != nullptr) {
    *error = message;
  }
  return false;
}

json::Value tabJson(const SourceConfig& source) {
  json::Value value = json::Value::object();
  value.set("id", json::Value(source.id));
  value.set("url", json::Value(source.url));
  value.set("interactive", json::Value(source.interactiveByDefault));
  value.set("popups", json::Value(source.popupPolicy));
  return value;
}

json::Value outputsJson(const SourceConfig& source) {
  json::Value value = json::Value::object();
  value.set("tab", json::Value(source.id));
  value.set("format", json::Value(source.format.toString()));
  value.set("matrix", json::Value(colourMatrixToString(source.matrix)));
  value.set("pacing", json::Value(source.externalPacing ? "external" : "internal"));
  json::Value list = json::Value::array();
  for (const auto& output : source.outputs) {
    list.push(output.toJson());
  }
  value.set("outputs", list);
  return value;
}

/// Lays one "tabs" entry over `source`. Only the fields the entry carries, so a
/// hand-written file that gives just an id and a URL leaves the rest alone.
void applyTab(const json::Value& entry, SourceConfig& source) {
  if (entry.has("url")) {
    source.url = entry["url"].asString();
    if (source.url.empty()) {
      source.url = "about:blank";
    }
  }
  if (entry.has("interactive")) {
    source.interactiveByDefault = entry["interactive"].asBool(true);
  }
  if (entry.has("popups")) {
    source.popupPolicy = entry["popups"].asString("navigate");
  }
}

/// Lays one "outputs" entry over `source`. The output list is replaced whole
/// when present — a rig is recalled, not merged into whatever happens to be
/// running, or an output removed since the save would survive the recall.
bool applyOutputs(const json::Value& entry, SourceConfig& source,
                  std::string* error) {
  if (entry.has("format")) {
    const std::string text = entry["format"].asString();
    const auto parsed = VideoFormat::parse(text);
    if (!parsed) {
      return fail(error, "tab '" + source.id + "': cannot parse format '" +
                             text + "'");
    }
    source.format = *parsed;
  }
  if (entry.has("matrix")) {
    source.matrix = colourMatrixFromString(entry["matrix"].asString());
  }
  if (entry.has("pacing")) {
    source.externalPacing = entry["pacing"].asString("external") != "internal";
  }
  if (entry.has("outputs")) {
    const json::Value& list = entry["outputs"];
    if (!list.isArray()) {
      return fail(error, "tab '" + source.id + "': \"outputs\" must be a list");
    }
    std::vector<OutputConfig> outputs;
    for (size_t i = 0; i < list.size(); ++i) {
      std::string outputError;
      auto output = OutputConfig::fromJson(list.at(i), &outputError);
      if (!output) {
        return fail(error, "tab '" + source.id + "' output " +
                               std::to_string(i + 1) + ": " + outputError);
      }
      outputs.push_back(*output);
    }
    source.outputs = std::move(outputs);
  }
  return true;
}

const json::Value* findOutputs(const json::Value& list, const std::string& id) {
  if (!list.isArray()) {
    return nullptr;
  }
  for (const auto& entry : list.elements()) {
    if (entry["tab"].asString() == id) {
      return &entry;
    }
  }
  return nullptr;
}

const SourceConfig* findSource(const AppConfig& config, const std::string& id) {
  for (const auto& source : config.sources) {
    if (source.id == id) {
      return &source;
    }
  }
  return nullptr;
}

}  // namespace

json::Value capture(const AppConfig& running, Parts parts) {
  json::Value document = json::Value::object();
  document.set("weblinked_show", json::Value(kFormatVersion));
  if (parts.tabs) {
    json::Value tabs = json::Value::array();
    for (const auto& source : running.sources) {
      tabs.push(tabJson(source));
    }
    document.set("tabs", tabs);
  }
  if (parts.outputs) {
    json::Value outputs = json::Value::array();
    for (const auto& source : running.sources) {
      outputs.push(outputsJson(source));
    }
    document.set("outputs", outputs);
  }
  return document;
}

Parts present(const json::Value& document) {
  Parts parts;
  parts.tabs = document["tabs"].isArray();
  parts.outputs = document["outputs"].isArray();
  return parts;
}

std::optional<AppConfig> recall(const AppConfig& running,
                                const json::Value& document, Parts parts,
                                std::string* error,
                                std::vector<std::string>* notes) {
  if (!document.isObject() || !document.has("weblinked_show")) {
    fail(error, "not a WebLinked show file");
    return std::nullopt;
  }
  if (document["weblinked_show"].asInt(0) > kFormatVersion) {
    fail(error, "this show was saved by a newer WebLinked");
    return std::nullopt;
  }
  if (!parts.tabs && !parts.outputs) {
    fail(error, "choose tabs, outputs or both");
    return std::nullopt;
  }
  const Parts available = present(document);
  if (parts.tabs && !available.tabs) {
    fail(error, "this show has no tabs in it");
    return std::nullopt;
  }
  if (parts.outputs && !available.outputs) {
    fail(error, "this show has no outputs in it");
    return std::nullopt;
  }

  const json::Value& outputList = document["outputs"];
  AppConfig wanted = running;

  if (parts.tabs) {
    const json::Value& tabs = document["tabs"];
    if (tabs.size() == 0) {
      // Every tab closed is a stopped process, and the manager refuses to be
      // left with nothing to render.
      fail(error, "this show has no tabs in it");
      return std::nullopt;
    }
    // A new tab borrows the running raster — the same reasoning as the page's
    // own "+ tab" form, which pre-fills it — and nothing else.
    SourceConfig templateSource;
    if (!running.sources.empty()) {
      templateSource.format = running.sources.front().format;
      templateSource.matrix = running.sources.front().matrix;
      templateSource.externalPacing = running.sources.front().externalPacing;
    }

    std::set<std::string> seen;
    wanted.sources.clear();
    for (size_t i = 0; i < tabs.size(); ++i) {
      const json::Value& entry = tabs.at(i);
      const std::string id = entry["id"].asString();
      if (id.empty()) {
        fail(error, "tab " + std::to_string(i + 1) + " has no id");
        return std::nullopt;
      }
      if (!seen.insert(id).second) {
        fail(error, "two tabs share the id '" + id + "'");
        return std::nullopt;
      }
      SourceConfig source;
      if (const SourceConfig* existing = findSource(running, id)) {
        source = *existing;
      } else {
        source = templateSource;
        source.id = id;
        source.outputs.clear();
      }
      applyTab(entry, source);
      wanted.sources.push_back(source);
    }
  }

  if (parts.outputs) {
    std::set<std::string> applied;
    for (auto& source : wanted.sources) {
      const json::Value* entry = findOutputs(outputList, source.id);
      if (entry == nullptr) {
        continue;
      }
      if (!applyOutputs(*entry, source, error)) {
        return std::nullopt;
      }
      applied.insert(source.id);
    }
    if (notes != nullptr) {
      for (const auto& entry : outputList.elements()) {
        const std::string id = entry["tab"].asString();
        if (applied.count(id) == 0) {
          notes->push_back("no tab '" + id + "' is open, so its outputs were skipped");
        }
      }
    }
  }

  for (auto& source : wanted.sources) {
    // The preview is permanent, and the control page is blind without it, so a
    // show that omits one — or a hand-written file — still gets one.
    source.wantPreview = true;
    source.ensurePreview();
    if (!source.validate(error)) {
      return std::nullopt;
    }
  }
  return wanted;
}

}  // namespace weblinked::show
