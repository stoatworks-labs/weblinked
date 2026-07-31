#pragma once

#include <memory>
#include <string>

#include "control/http_server.h"
#include "control/osc_server.h"

namespace weblinked {

class Engine;

/// The control surface: an HTTP/JSON API, the embedded control page, and an OSC
/// listener, all mapped onto one Engine.
///
/// Two protocols for the same verbs, on purpose. HTTP is what the operator's
/// page and any scripting uses; OSC is what a Companion button or a show-control
/// cue sends. Neither is a superset of the other in practice, and a graphic that
/// can only be changed by someone with a browser open is no use in a running
/// show.
class ControlApi {
 public:
  struct Config {
    std::string httpBind = "127.0.0.1";
    int httpPort = 7654;
    /// Required on every HTTP request when set. Empty means no check, which is
    /// only reasonable on loopback.
    std::string httpToken;
    bool oscEnabled = true;
    std::string oscBind = "0.0.0.0";
    int oscPort = 7655;
    /// Address prefix, so several instances can share a network.
    std::string oscPrefix = "/weblinked";
    /// Where the settings page saves to and reloads from. Empty means the
    /// platform default — see settings::defaultPath().
    std::string settingsPath;
  };

  explicit ControlApi(Engine* engine);
  ~ControlApi();

  bool start(const Config& config, std::string& error);
  void stop();

  /// The URL to open in the operator window.
  std::string controlUrl() const;

  const HttpServer& http() const { return http_; }
  const OscServer& osc() const { return osc_; }

 private:
  void handleHttp(const HttpServer::Request& request, HttpServer::Response& response);
  void handleOsc(const OscServer::Message& message);

  Engine* engine_;
  HttpServer http_;
  OscServer osc_;
  Config config_;
  /// Resolved once at start(), so every endpoint agrees on which file it means.
  std::string settingsPath_;
};

}  // namespace weblinked
