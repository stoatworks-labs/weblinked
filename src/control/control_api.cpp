#include "control/control_api.h"

#include "control/web_assets.h"
#include "core/json.h"
#include "diag/diag.h"
#include "engine/engine.h"
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

}  // namespace

ControlApi::ControlApi(Engine* engine) : engine_(engine) {}

ControlApi::~ControlApi() { stop(); }

bool ControlApi::start(const Config& config, std::string& error) {
  config_ = config;

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
  return true;
}

void ControlApi::stop() {
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

  if (path == "/api/state") {
    response.json(engine_->state().serialize());
    return;
  }

  if (path == "/api/preview") {
    auto* preview = engine_->preview();
    if (preview == nullptr) {
      response.error(404, "no preview output is configured");
      return;
    }
    auto snapshot = preview->snapshot();
    if (snapshot.pixels.empty()) {
      response.error(503, "preview has produced no frames yet");
      return;
    }
    response.contentType = "application/octet-stream";
    response.useBinary = true;
    response.binaryBody = std::move(snapshot.pixels);
    // Dimensions travel in headers so the body stays a bare pixel buffer.
    response.extraHeaders["X-Frame-Width"] = std::to_string(snapshot.width);
    response.extraHeaders["X-Frame-Height"] = std::to_string(snapshot.height);
    response.extraHeaders["X-Frame-Sequence"] = std::to_string(snapshot.sequence);
    return;
  }

  if (path == "/api/diagnostics") {
    // Deliberately a GET: "open this link and send me the file it names" is one
    // instruction, and works from a phone.
    const std::string bundle = diag::collectBundle();
    json::Value value = json::Value::object();
    value.set("bundle", json::Value(bundle));
    value.set("log", json::Value(diag::logFilePath()));
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
    engine_->setUrl(url);
    ok(response);
    return;
  }

  if (path == "/api/reload") {
    engine_->reload(body["ignore_cache"].asBool(false));
    ok(response);
    return;
  }

  if (path == "/api/script") {
    const std::string script = body["script"].asString();
    if (script.empty()) {
      response.error(400, "expected {\"script\": \"...\"}");
      return;
    }
    engine_->runScript(script);
    ok(response);
    return;
  }

  if (path == "/api/mute") {
    engine_->setAudioMuted(body["muted"].asBool(true));
    ok(response);
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
    if (!engine_->setFormat(*format, error)) {
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
    std::string error;
    if (!engine_->setOutputEnabled(name, body["enabled"].asBool(true), error)) {
      response.error(409, error);
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
    if (spec.name.empty()) {
      spec.name = spec.kind;
    }
    std::string error;
    if (!engine_->addOutput(spec, error)) {
      response.error(409, error);
      return;
    }
    ok(response);
    return;
  }

  if (path == "/api/output/remove") {
    const std::string name = body["name"].asString();
    if (!engine_->removeOutput(name)) {
      response.error(404, "no output named '" + name + "'");
      return;
    }
    ok(response);
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

  if (action == "url") {
    const std::string url = message.firstString();
    if (!url.empty()) {
      engine_->setUrl(url);
    }
    return;
  }

  if (action == "reload") {
    // A bare /reload with no arguments is a plain reload; an argument selects
    // whether to bypass the cache, which is what a "refresh graphics" button
    // on a Companion page wants.
    engine_->reload(message.firstBool(false));
    return;
  }

  if (action == "script") {
    const std::string script = message.firstString();
    if (!script.empty()) {
      engine_->runScript(script);
    }
    return;
  }

  if (action == "mute") {
    engine_->setAudioMuted(message.firstBool(true));
    return;
  }

  if (action == "format") {
    const auto format = VideoFormat::parse(message.firstString());
    if (format) {
      std::string error;
      engine_->setFormat(*format, error);
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
    if (!engine_->setOutputEnabled(name, message.firstBool(true), error)) {
      diag::warn("osc: %s", error.c_str());
    }
    return;
  }

  diag::debug("osc: ignoring %s", message.address.c_str());
}

}  // namespace weblinked
