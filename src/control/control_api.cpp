#include "control/control_api.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "control/about_assets.h"
#include "control/web_assets.h"
#include "core/json.h"
#include "core/settings_store.h"
#include "diag/diag.h"
#include "engine/engine.h"
#include "engine/source_manager.h"
#include "outputs/output.h"
#include "outputs/preview_output.h"

namespace weblinked {
namespace {

/// Parses a request body as a JSON object, or reports why not.
bool parseBody(const HttpServer::Request& request, json::Value& out,
               HttpServer::Response& response) {
  if (request.body.empty()) {
    out = json::Value::object();
    return true;
  }
  std::string error;
  auto parsed = json::parse(request.body, &error);
  if (!parsed) {
    response.error(400, "malformed JSON body: " + error);
    return false;
  }
  out = *parsed;
  return true;
}

void ok(HttpServer::Response& response) { response.json("{\"ok\":true}"); }

/// The level as the API spells it: lower case, no padding.
///
/// diag pads its names to five characters so log columns line up, which is
/// right for a log file and wrong for JSON — "INFO " would never match the
/// control page's <option value="info">, so the selector would sit on the wrong
/// entry and the first change would look like it had done nothing.
std::string levelName(diag::Level level) {
  std::string text = diag::levelToString(level);
  while (!text.empty() && text.back() == ' ') {
    text.pop_back();
  }
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

/// Parses and validates a source configuration from a request body, reporting
/// the reason to the client if it will not do.
///
/// `defaultId` fills in an absent id. The single-source settings page never
/// sends one — it is editing the source it is already looking at — so the caller
/// supplies which that is. The collection routes pass nothing, because an
/// unnamed source in a set of them would silently overwrite whichever one
/// happened to share the default.
std::optional<SourceConfig> readSourceConfig(const json::Value& value,
                                             HttpServer::Response& response,
                                             const std::string& defaultId = "") {
  std::string error;
  auto config = SourceConfig::fromJson(value, &error);
  if (!config) {
    response.error(400, error);
    return std::nullopt;
  }
  if (config->id.empty()) {
    config->id = defaultId;
  }
  if (!config->validate(&error)) {
    response.error(400, error);
    return std::nullopt;
  }
  return config;
}

/// Builds an InputEvent from the control page's JSON.
///
/// Positions arrive normalised (0..1) because only the page knows how big its
/// preview canvas is; the engine scales them to the current raster.
bool parseInputEvent(const json::Value& value, InputEvent& out,
                     HttpServer::Response& response) {
  const std::string type = value["type"].asString();

  out.nx = value["nx"].asDouble(0.0);
  out.ny = value["ny"].asDouble(0.0);
  out.modifiers = static_cast<uint32_t>(value["modifiers"].asInt64(0));

  if (type == "move") {
    out.type = InputEvent::Type::kMove;
    out.leaving = value["leaving"].asBool(false);
    return true;
  }
  if (type == "down" || type == "up") {
    out.type = InputEvent::Type::kButton;
    out.down = type == "down";
    const int button = value["button"].asInt(0);
    out.button = button == 1   ? InputEvent::Button::kMiddle
                 : button == 2 ? InputEvent::Button::kRight
                               : InputEvent::Button::kLeft;
    // Chromium uses the click count to distinguish single from double clicks;
    // clamped because a page that receives clickCount 900 behaves oddly.
    out.clickCount = std::clamp(value["clicks"].asInt(1), 1, 3);
    return true;
  }
  if (type == "wheel") {
    out.type = InputEvent::Type::kWheel;
    out.deltaX = value["dx"].asInt(0);
    out.deltaY = value["dy"].asInt(0);
    return true;
  }
  if (type == "key") {
    out.type = InputEvent::Type::kKey;
    const std::string action = value["action"].asString("down");
    if (action == "up") {
      out.keyAction = InputEvent::KeyAction::kKeyUp;
    } else if (action == "char") {
      out.keyAction = InputEvent::KeyAction::kChar;
    } else {
      out.keyAction = InputEvent::KeyAction::kRawKeyDown;
    }
    out.keyCode = value["key_code"].asInt(0);
    out.character = value["character"].asInt(0);
    return true;
  }
  if (type == "focus") {
    out.type = InputEvent::Type::kFocus;
    out.focused = value["focused"].asBool(true);
    return true;
  }

  response.error(400, "unknown input event type '" + type +
                          "' — expected move, down, up, wheel, key or focus");
  return false;
}

}  // namespace

ControlApi::ControlApi(SourceManager* sources) : sources_(sources) {}

bool ControlApi::withRequestSource(const HttpServer::Request& request,
                                   HttpServer::Response& response,
                                   const std::function<void(Engine&)>& fn) {
  const std::string requested = request.param("source", "");
  const std::string id = requested.empty() ? sources_->primaryId() : requested;
  if (id.empty()) {
    response.error(503, "no sources are running");
    return false;
  }
  if (!sources_->withSource(id, fn)) {
    response.error(404, "no source called '" + id + "'");
    return false;
  }
  return true;
}

ControlApi::~ControlApi() { stop(); }

bool ControlApi::start(const Config& config, std::string& error) {
  config_ = config;
  settingsPath_ = config.settingsPath.empty() ? settings::defaultPath()
                                              : config.settingsPath;

  if (!http_.start(config.httpBind, config.httpPort, config.httpToken,
                   [this](const HttpServer::Request& request,
                          HttpServer::Response& response) {
                     handleHttp(request, response);
                   },
                   error)) {
    return false;
  }

  if (config.oscEnabled) {
    std::string oscError;
    if (!osc_.start(config.oscBind, config.oscPort,
                    [this](const OscServer::Message& message) {
                      handleOsc(message);
                    },
                    oscError)) {
      // Not fatal: losing OSC leaves the HTTP surface, and the usual cause is
      // another instance already holding the port.
      diag::warn("control: OSC unavailable: %s", oscError.c_str());
    }
  }

  // Last, and only once the sockets above are actually listening: the whole
  // point of the record is that something answers at the address it publishes.
  startAdvertising();
  return true;
}

void ControlApi::startAdvertising() {
  if (!config_.mdnsEnabled) {
    diag::info("control: mDNS advertisement disabled");
    return;
  }

  std::string reason;
  if (!mdns::bindIsAdvertisable(config_.httpBind, &reason)) {
    // Deliberately a warning rather than silence. An operator who asked for
    // discovery and is not getting it needs the reason on the first line they
    // look at, not a service that appears in a fleet list and then refuses
    // every connection from it.
    diag::warn("control: not advertising over mDNS — %s", reason.c_str());
    return;
  }

  const std::string host = mdns::hostName();
  const auto port = static_cast<uint16_t>(config_.httpPort);

  mdns::TxtInputs inputs;
  inputs.name = mdns::instanceNameFor(config_.instanceName, host, port);
  inputs.version = WEBLINKED_VERSION;
  inputs.id = mdns::instanceId(host, port);
  inputs.httpPort = port;
  inputs.tokenRequired = !config_.httpToken.empty();
  inputs.oscEnabled = config_.oscEnabled && osc_.isRunning();
  inputs.oscPort = static_cast<uint16_t>(config_.oscPort);
  inputs.oscPrefix = config_.oscPrefix;

  mdns::Advertisement advertisement;
  advertisement.instanceName = inputs.name;
  advertisement.port = port;
  advertisement.txt = mdns::buildTxt(inputs);

  std::string error;
  if (!mdns_.start(advertisement, error)) {
    // Never fatal. Discovery is a convenience on top of an API that works
    // perfectly well when an operator types the address, and a fleet
    // controller falls back to sweeping the subnet.
    diag::warn("control: mDNS unavailable: %s", error.c_str());
  }
}

void ControlApi::stop() {
  // Withdrawn first, and before the sockets close: a record still on the
  // network pointing at a port that has stopped answering is the one state
  // worse than never having advertised at all.
  mdns_.stop();
  osc_.stop();
  http_.stop();
}

std::string ControlApi::controlUrl() const {
  std::string host = config_.httpBind;
  if (host.empty() || host == "0.0.0.0") {
    host = "127.0.0.1";
  }
  std::string url = "http://" + host + ":" + std::to_string(config_.httpPort) + "/";
  if (!config_.httpToken.empty()) {
    url += "?token=" + config_.httpToken;
  }
  return url;
}

void ControlApi::handleHttp(const HttpServer::Request& request,
                            HttpServer::Response& response) {
  const std::string& path = request.path;

  if (path == "/" || path == "/index.html") {
    response.contentType = "text/html; charset=utf-8";
    response.body = assets::kControlPage;
    return;
  }

  // The About dialog, compiled in for the same reason the control page is: this
  // tool must be able to open its own front end with no network beyond the box
  // it runs on. See about_assets.h, generated from stoatworks-backend/about.
  if (path == "/about.js" || path == "/about-data.js") {
    response.contentType = "application/javascript; charset=utf-8";
    response.body = path == "/about.js" ? assets::kAboutScript : assets::kAboutData;
    return;
  }

  if (path == "/api/state") {
    // Deliberately still one source's state, not the collection: this is the
    // endpoint every v0.3.0 client polls, and widening its shape would break
    // each of them at once. /api/sources is where the collection lives.
    withRequestSource(request, response, [&](Engine& engine) {
      response.json(engine.state().serialize());
    });
    return;
  }

  if (path == "/api/sources") {
    response.json(sources_->state().serialize());
    return;
  }

  if (path == "/api/preview") {
    // Captured under withSource, because the snapshot has to be taken while the
    // engine is guaranteed alive — a bare PreviewOutput* would outlive a source
    // removed between the lookup and the copy.
    bool configured = true;
    bool empty = false;
    withRequestSource(request, response, [&](Engine& engine) {
      auto* preview = engine.preview();
      if (preview == nullptr) {
        configured = false;
        return;
      }
      auto snapshot = preview->snapshot();
      if (snapshot.pixels.empty()) {
        empty = true;
        return;
      }
      response.contentType = "application/octet-stream";
      response.useBinary = true;
      response.binaryBody = std::move(snapshot.pixels);
      // Dimensions travel in headers so the body stays a bare pixel buffer.
      response.extraHeaders["X-Frame-Width"] = std::to_string(snapshot.width);
      response.extraHeaders["X-Frame-Height"] = std::to_string(snapshot.height);
      response.extraHeaders["X-Frame-Sequence"] =
          std::to_string(snapshot.sequence);
    });
    if (!configured) {
      response.error(404, "no preview output is configured");
    } else if (empty) {
      response.error(503, "preview has produced no frames yet");
    }
    return;
  }

  if (path == "/api/diagnostics") {
    // Deliberately a GET: "open this link and send me the file it names" is one
    // instruction, and works from a phone.
    const std::string bundle = diag::collectBundle();
    json::Value value = json::Value::object();
    value.set("bundle", json::Value(bundle));
    value.set("log", json::Value(diag::logFilePath()));
    value.set("log_directory", json::Value(diag::logDirectory()));
    response.json(value.serialize());
    return;
  }

  if (path == "/api/diagnostics/bundle") {
    // The same bundle, but as the file itself rather than a path to it. The
    // path is no use when the operator is on a laptop and the machine that has
    // the fault is in a rack two floors down.
    const std::string bundle = diag::collectBundle();
    std::ifstream file(bundle, std::ios::binary);
    if (!file) {
      response.error(500, "the diagnostics bundle could not be written to " + bundle);
      return;
    }
    std::ostringstream text;
    text << file.rdbuf();
    response.contentType = "application/json";
    response.body = text.str();
    response.extraHeaders["Content-Disposition"] =
        "attachment; filename=\"" + std::filesystem::path(bundle).filename().string() + "\"";
    return;
  }

  if (path == "/api/log") {
    const int requested = std::atoi(request.param("lines", "200").c_str());
    const size_t lines = requested <= 0 ? 200 : static_cast<size_t>(requested);
    json::Value value = json::Value::object();
    value.set("level", json::Value(levelName(diag::level())));
    value.set("path", json::Value(diag::logFilePath()));
    value.set("directory", json::Value(diag::logDirectory()));
    json::Value list = json::Value::array();
    for (const auto& line : diag::tail(lines)) {
      list.push(json::Value(line));
    }
    value.set("lines", list);
    response.json(value.serialize());
    return;
  }

  if (path == "/api/settings") {
    json::Value value = json::Value::object();
    value.set("path", json::Value(settingsPath_));
    value.set("saved",
              json::Value(std::filesystem::exists(std::filesystem::path(settingsPath_))));
    // "source" is the one the request names, kept for the v0.3.0 settings page;
    // "sources" is all of them, which is what the multi-source page edits.
    if (!withRequestSource(request, response, [&](Engine& engine) {
          value.set("source", engine.configuration().toJson());
        })) {
      // Early, or the response.json() below would overwrite the 404 body with a
      // 200-shaped document and the client would believe a missing source was
      // fine.
      return;
    }
    json::Value list = json::Value::array();
    for (const auto& source : sources_->configuration().sources) {
      list.push(source.toJson());
    }
    value.set("sources", list);

    // Process-level, so it goes here rather than in /api/state — that endpoint
    // is one source's state and every v0.3.0 client polls it. Reporting the
    // *live* result rather than the setting is the point: "asked for, bound to
    // loopback, so not advertising" is the answer to "why can rookery not see
    // this machine", and it is not derivable from the config alone.
    json::Value discovery = json::Value::object();
    discovery.set("enabled", json::Value(config_.mdnsEnabled));
    discovery.set("advertising", json::Value(mdns_.isRunning()));
    discovery.set("service_type", json::Value(std::string(mdns::kServiceType)));
    discovery.set("name", json::Value(mdns_.registeredName()));
    if (config_.mdnsEnabled) {
      std::string reason;
      if (!mdns::bindIsAdvertisable(config_.httpBind, &reason)) {
        discovery.set("blocked_because", json::Value(reason));
      }
    }
    value.set("discovery", discovery);

    response.json(value.serialize());
    return;
  }

  // Everything past here mutates state.
  if (request.method != "POST") {
    response.error(405, "use POST for " + path);
    return;
  }

  json::Value body;
  if (!parseBody(request, body, response)) {
    return;
  }

  if (path == "/api/url") {
    const std::string url = body["url"].asString();
    if (url.empty()) {
      response.error(400, "expected {\"url\": \"...\"}");
      return;
    }
    if (withRequestSource(request, response,
                          [&](Engine& engine) { engine.setUrl(url); })) {
      ok(response);
    }
    return;
  }

  if (path == "/api/reload") {
    const bool ignoreCache = body["ignore_cache"].asBool(false);
    if (withRequestSource(request, response, [&](Engine& engine) {
          engine.reload(ignoreCache);
        })) {
      ok(response);
    }
    return;
  }

  if (path == "/api/script") {
    const std::string script = body["script"].asString();
    if (script.empty()) {
      response.error(400, "expected {\"script\": \"...\"}");
      return;
    }
    if (withRequestSource(request, response,
                          [&](Engine& engine) { engine.runScript(script); })) {
      ok(response);
    }
    return;
  }

  if (path == "/api/mute") {
    const bool muted = body["muted"].asBool(true);
    if (withRequestSource(request, response, [&](Engine& engine) {
          engine.setAudioMuted(muted);
        })) {
      ok(response);
    }
    return;
  }

  if (path == "/api/input") {
    // Accepts one event or a batch. A batch matters for pointer moves: the
    // control page coalesces them, so a drag is one request rather than sixty.
    // Parsed in full before any of it is delivered, so a malformed event at the
    // end of a drag cannot leave half the gesture applied to the page.
    std::vector<InputEvent> events;
    const json::Value& batch = body["events"];
    if (batch.isArray()) {
      events.reserve(batch.size());
      for (size_t i = 0; i < batch.size(); ++i) {
        InputEvent event;
        if (!parseInputEvent(batch.at(i), event, response)) {
          return;
        }
        events.push_back(event);
      }
    } else {
      InputEvent event;
      if (!parseInputEvent(body, event, response)) {
        return;
      }
      events.push_back(event);
    }

    if (withRequestSource(request, response, [&](Engine& engine) {
          for (const auto& event : events) {
            engine.sendInput(event);
          }
        })) {
      ok(response);
    }
    return;
  }

  if (path == "/api/format") {
    const std::string text = body["format"].asString();
    const auto format = VideoFormat::parse(text);
    if (!format) {
      response.error(400, "cannot parse format '" + text +
                              "' — try 1080p50, 720p59.94 or 1920x1080i25");
      return;
    }
    std::string error;
    bool applied = true;
    if (!withRequestSource(request, response, [&](Engine& engine) {
          applied = engine.setFormat(*format, error);
        })) {
      return;
    }
    if (!applied) {
      // A partial failure: the format changed but not every output could reopen
      // at it. Reporting 409 rather than 500 because the request was valid and
      // the state did change.
      response.error(409, "format applied, but an output could not restart: " + error);
      return;
    }
    ok(response);
    return;
  }

  if (path == "/api/output") {
    const std::string name = body["name"].asString();
    if (name.empty()) {
      response.error(400, "expected {\"name\": \"...\", \"enabled\": true}");
      return;
    }
    const bool enabled = body["enabled"].asBool(true);
    std::string error;
    bool applied = true;
    if (!withRequestSource(request, response, [&](Engine& engine) {
          applied = engine.setOutputEnabled(name, enabled, error);
        })) {
      return;
    }
    if (!applied) {
      response.error(409, error);
      return;
    }
    ok(response);
    return;
  }

  if (path == "/api/output/background") {
    const std::string name = body["name"].asString();
    if (name.empty()) {
      response.error(400,
                     "expected {\"name\": \"...\", \"background\": "
                     "\"transparent\"|\"colour\", \"colour\": \"#00b140\"}");
      return;
    }
    // Parsed through OutputConfig so the endpoint, the settings file and the
    // update path cannot drift on what counts as a colour.
    json::Value shim = json::Value::object();
    shim.set("kind", json::Value(std::string("preview")));
    shim.set("background", body["background"]);
    if (!body["colour"].isNull()) {
      shim.set("background_colour", body["colour"]);
    } else if (!body["color"].isNull()) {
      shim.set("background_colour", body["color"]);
    }
    std::string parseError;
    const auto parsed = OutputConfig::fromJson(shim, &parseError);
    if (!parsed) {
      response.error(400, parseError);
      return;
    }
    std::string error;
    bool applied = true;
    if (!withRequestSource(request, response, [&](Engine& engine) {
          applied = engine.setOutputBackground(name, parsed->background, error);
        })) {
      return;
    }
    if (!applied) {
      response.error(404, error);
      return;
    }
    ok(response);
    return;
  }

  if (path == "/api/output/add") {
    OutputSpec spec;
    spec.kind = body["kind"].asString();
    spec.name = body["name"].asString();
    spec.deviceIndex = body["device_index"].asInt(0);
    if (body["options"].isObject()) {
      spec.options = body["options"];
    }
    if (spec.kind.empty()) {
      response.error(400, "expected {\"kind\": \"ndi\", \"name\": \"...\"}");
      return;
    }
    // Through OutputConfig rather than by hand, so add, update and the settings
    // file cannot disagree about what counts as a colour.
    std::string backgroundError;
    const auto parsed = OutputConfig::fromJson(body, &backgroundError);
    if (!parsed) {
      response.error(400, backgroundError);
      return;
    }
    spec.background = parsed->background;
    if (spec.name.empty()) {
      spec.name = spec.kind;
    }
    std::string error;
    bool applied = true;
    if (!withRequestSource(request, response, [&](Engine& engine) {
          applied = engine.addOutput(spec, error);
        })) {
      return;
    }
    if (!applied) {
      response.error(409, error);
      return;
    }
    ok(response);
    return;
  }

  if (path == "/api/output/remove") {
    const std::string name = body["name"].asString();
    bool removed = true;
    if (!withRequestSource(request, response, [&](Engine& engine) {
          removed = engine.removeOutput(name);
        })) {
      return;
    }
    if (!removed) {
      response.error(404, "no output named '" + name + "'");
      return;
    }
    ok(response);
    return;
  }

  if (path == "/api/output/update") {
    const std::string name = body["name"].asString();
    if (name.empty()) {
      response.error(400, "expected {\"name\": \"...\", \"output\": { ... }}");
      return;
    }
    std::string parseError;
    const auto config = OutputConfig::fromJson(body["output"], &parseError);
    if (!config) {
      response.error(400, parseError);
      return;
    }
    OutputSpec spec = outputSpecFromConfig(*config);
    if (spec.name.empty()) {
      spec.name = name;
    }
    std::string error;
    bool applied = true;
    if (!withRequestSource(request, response, [&](Engine& engine) {
          applied = engine.updateOutput(name, spec, error);
        })) {
      return;
    }
    if (!applied) {
      response.error(409, error);
      return;
    }
    ok(response);
    return;
  }

  if (path == "/api/pacing") {
    const std::string mode = body["pacing"].asString("external");
    if (mode != "external" && mode != "internal") {
      response.error(400, "pacing must be \"external\" or \"internal\"");
      return;
    }
    if (withRequestSource(request, response, [&](Engine& engine) {
          engine.setPacing(mode == "internal"
                               ? BrowserSource::Pacing::kInternalTimer
                               : BrowserSource::Pacing::kExternalBeginFrame);
        })) {
      ok(response);
    }
    return;
  }

  if (path == "/api/settings/apply") {
    const std::string requested = request.param("source", "");
    const auto config = readSourceConfig(
        body["source"], response,
        requested.empty() ? sources_->primaryId() : requested);
    if (!config) {
      return;
    }
    std::string error;
    bool applied = true;
    if (!withRequestSource(request, response, [&](Engine& engine) {
          applied = engine.applyConfiguration(*config, error);
        })) {
      return;
    }
    if (!applied) {
      // 409, not 500: the settings were valid and most of them are now live.
      // The page shows the message against the output that refused.
      response.error(409, "settings applied, except: " + error);
      return;
    }
    ok(response);
    return;
  }

  // The whole set at once, which is what the multi-source page saves: sources
  // that have gone are stopped, new ones started, the rest reconciled in place.
  if (path == "/api/sources/apply") {
    const json::Value& list = body["sources"];
    if (!list.isArray()) {
      response.error(400, "expected {\"sources\": [ ... ]}");
      return;
    }
    AppConfig wanted;
    for (size_t i = 0; i < list.size(); ++i) {
      const auto config = readSourceConfig(list.at(i), response);
      if (!config) {
        return;
      }
      wanted.sources.push_back(*config);
    }
    std::string error;
    if (!sources_->applyConfiguration(wanted, error)) {
      response.error(409, "sources applied, except: " + error);
      return;
    }
    ok(response);
    return;
  }

  if (path == "/api/sources/add") {
    const auto config = readSourceConfig(body["source"], response);
    if (!config) {
      return;
    }
    std::string error;
    if (!sources_->add(*config, error)) {
      response.error(409, error);
      return;
    }
    json::Value value = json::Value::object();
    value.set("ok", json::Value(true));
    value.set("id", json::Value(config->id));
    response.json(value.serialize());
    return;
  }

  if (path == "/api/sources/remove") {
    const std::string id = body["id"].asString();
    if (id.empty()) {
      response.error(400, "expected {\"id\": \"...\"}");
      return;
    }
    // Refused rather than allowed: a manager with no sources has no primary, so
    // every un-addressed request afterwards would 503 and the page would look
    // broken. Removing the last source is a stop, and stopping is what closing
    // the window is for.
    if (sources_->size() <= 1) {
      response.error(409, "'" + id + "' is the only source — stop WebLinked instead");
      return;
    }
    std::string error;
    if (!sources_->remove(id, error)) {
      response.error(404, error);
      return;
    }
    ok(response);
    return;
  }

  if (path == "/api/settings/save") {
    // Saving what the engines are actually doing, not what the request says, so
    // the file can never claim an output that failed to open.
    AppConfig file = sources_->configuration();
    file.httpBind = config_.httpBind;
    file.httpPort = config_.httpPort;
    file.httpToken = config_.httpToken;
    file.oscEnabled = config_.oscEnabled;
    file.oscBind = config_.oscBind;
    file.oscPort = config_.oscPort;
    file.mdnsEnabled = config_.mdnsEnabled;
    file.instanceName = config_.instanceName;

    std::string error;
    if (!settings::save(file, settingsPath_, &error)) {
      response.error(500, error);
      return;
    }
    diag::info("settings saved to %s", settingsPath_.c_str());
    json::Value value = json::Value::object();
    value.set("ok", json::Value(true));
    value.set("path", json::Value(settingsPath_));
    response.json(value.serialize());
    return;
  }

  if (path == "/api/settings/reload") {
    std::string error;
    const auto file = settings::load(settingsPath_, &error);
    if (!file || file->sources.empty()) {
      response.error(404, file ? "the settings file has no sources" : error);
      return;
    }
    std::string applyError;
    if (!sources_->applyConfiguration(*file, applyError)) {
      response.error(409, "settings loaded, except: " + applyError);
      return;
    }
    ok(response);
    return;
  }

  if (path == "/api/log/level") {
    const std::string text = body["level"].asString();
    if (text.empty()) {
      response.error(400, "expected {\"level\": \"debug\"}");
      return;
    }
    diag::setLevel(diag::levelFromString(text));
    json::Value value = json::Value::object();
    value.set("ok", json::Value(true));
    // Echoed back because levelFromString falls back rather than failing: a
    // typo would otherwise look like it worked.
    value.set("level", json::Value(levelName(diag::level())));
    response.json(value.serialize());
    return;
  }

  if (path == "/api/diagnostics/report") {
    const std::string reason =
        body["reason"].asString("requested from the control page");
    const std::string report = diag::writeReport(reason);
    json::Value value = json::Value::object();
    value.set("ok", json::Value(true));
    value.set("report", json::Value(report));
    response.json(value.serialize());
    return;
  }

  response.error(404, "no such endpoint: " + path);
}

void ControlApi::handleOsc(const OscServer::Message& message) {
  const std::string& prefix = config_.oscPrefix;
  if (message.address.rfind(prefix, 0) != 0) {
    return;
  }
  std::string action = message.address.substr(prefix.size());
  if (!action.empty() && action[0] == '/') {
    action = action.substr(1);
  }

  // /weblinked/source/<id>/<verb> addresses one source; a bare /weblinked/<verb>
  // means the primary. Companion sends a fixed address per button, so putting
  // the id in the path — rather than in an argument — lets one button be bound
  // to one feed and stay bound, which is how an operator expects a physical
  // button to behave.
  std::string target;
  const std::string sourcePrefix = "source/";
  if (action.rfind(sourcePrefix, 0) == 0) {
    const std::string rest = action.substr(sourcePrefix.size());
    const size_t slash = rest.find('/');
    if (slash == std::string::npos || slash == 0) {
      diag::warn("osc: %s names no verb after the source id",
                 message.address.c_str());
      return;
    }
    target = rest.substr(0, slash);
    action = rest.substr(slash + 1);
  } else {
    target = sources_->primaryId();
  }

  if (target.empty()) {
    diag::warn("osc: no sources are running, ignoring %s",
               message.address.c_str());
    return;
  }

  // One lookup for the whole message: every branch below needs the engine, and
  // holding it open once means a removal cannot land between the dispatch and
  // the call.
  const bool delivered = sources_->withSource(target, [&](Engine& engine) {
    handleOscForSource(engine, action, message);
  });
  if (!delivered) {
    diag::warn("osc: no source called '%s' for %s", target.c_str(),
               message.address.c_str());
  }
}

void ControlApi::handleOscForSource(Engine& engine, const std::string& action,
                                    const OscServer::Message& message) {
  if (action == "url") {
    const std::string url = message.firstString();
    if (!url.empty()) {
      engine.setUrl(url);
    }
    return;
  }

  if (action == "reload") {
    // A bare /reload with no arguments is a plain reload; an argument selects
    // whether to bypass the cache, which is what a "refresh graphics" button
    // on a Companion page wants.
    engine.reload(message.firstBool(false));
    return;
  }

  if (action == "script") {
    const std::string script = message.firstString();
    if (!script.empty()) {
      engine.runScript(script);
    }
    return;
  }

  if (action == "mute") {
    engine.setAudioMuted(message.firstBool(true));
    return;
  }

  if (action == "format") {
    const auto format = VideoFormat::parse(message.firstString());
    if (format) {
      std::string error;
      engine.setFormat(*format, error);
    } else {
      diag::warn("osc: cannot parse format '%s'", message.firstString().c_str());
    }
    return;
  }

  // /weblinked/output/<name> with 0 or 1, which is the shape a Companion
  // toggle button produces most naturally.
  const std::string outputPrefix = "output/";
  if (action.rfind(outputPrefix, 0) == 0) {
    const std::string name = action.substr(outputPrefix.size());
    if (name.empty()) {
      return;
    }
    std::string error;
    if (!engine.setOutputEnabled(name, message.firstBool(true), error)) {
      diag::warn("osc: %s", error.c_str());
    }
    return;
  }

  diag::debug("osc: ignoring %s", message.address.c_str());
}

}  // namespace weblinked
