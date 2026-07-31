#pragma once

#include <functional>
#include <memory>
#include <string>

#include "control/http_server.h"
#include "control/osc_server.h"

namespace weblinked {

class Engine;
class SourceManager;

/// The control surface: an HTTP/JSON API, the embedded control page, and an OSC
/// listener, mapped onto every source the manager holds.
///
/// Two protocols for the same verbs, on purpose. HTTP is what the operator's
/// page and any scripting uses; OSC is what a Companion button or a show-control
/// cue sends. Neither is a superset of the other in practice, and a graphic that
/// can only be changed by someone with a browser open is no use in a running
/// show.
///
/// **Which source a request means.** Every per-source verb takes an optional
/// `?source=<id>`; without it the request goes to the primary source, which for
/// a command-line launch is the only one there is. That is what keeps every
/// v0.3.0 client — the shipped control page, a Companion config, a curl in
/// somebody's runbook — working unchanged against a multi-source build. The
/// selector is a query parameter rather than a body field because `/api/state`
/// and `/api/preview` are GETs with no body, and one mechanism that works
/// everywhere beats two that each work half the time.
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

  explicit ControlApi(SourceManager* sources);
  ~ControlApi();

  bool start(const Config& config, std::string& error);
  void stop();

  /// The address the control page is reachable at, printed at startup and used
  /// by the tray launcher's "Launch GUI" button.
  std::string controlUrl() const;

  const HttpServer& http() const { return http_; }
  const OscServer& osc() const { return osc_; }

 private:
  void handleHttp(const HttpServer::Request& request, HttpServer::Response& response);
  void handleOsc(const OscServer::Message& message);

  /// The verbs themselves, once handleOsc has worked out which source the
  /// address means and is holding it open.
  void handleOscForSource(Engine& engine, const std::string& action,
                          const OscServer::Message& message);

  /// Runs `fn` against the source the request names, or the primary if it names
  /// none. Answers 404 and returns false when there is no such source, so a
  /// typo in an id is a clear error rather than a silent no-op on the wrong
  /// feed — which on air is the more expensive of the two.
  bool withRequestSource(const HttpServer::Request& request,
                         HttpServer::Response& response,
                         const std::function<void(Engine&)>& fn);

  SourceManager* sources_;
  HttpServer http_;
  OscServer osc_;
  Config config_;
  /// Resolved once at start(), so every endpoint agrees on which file it means.
  std::string settingsPath_;
};

}  // namespace weblinked
