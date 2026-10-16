#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace weblinked {

/// A small blocking HTTP/1.1 server.
///
/// Hand-rolled rather than vendored, because the whole requirement is: serve one
/// HTML page, answer a handful of JSON endpoints, and hand over a preview buffer
/// a few times a second. That is a few hundred lines, against a dependency that
/// would have to be built for five platforms.
///
/// Thread per connection. At the traffic this sees — one operator, one browser
/// tab — that is the right trade: no event loop to get wrong, and a slow client
/// cannot stall the others.
///
/// But capped. Uncapped, a handler that cannot finish — an engine whose output
/// driver had deadlocked, in issue #12 — let a client polling every 200 ms park
/// a thread per poll, and 1,410 of them were waiting when the process was
/// finally killed. Past the cap a new connection is answered 503 and closed, and
/// every socket has receive and send timeouts, so no handler can wait on a
/// client for ever either.
///
/// Deliberately *not* a general-purpose server: no TLS, no chunked encoding, no
/// file serving from disk. Bind it to loopback unless you have a reason not to,
/// and set a token if you do.
class HttpServer {
 public:
  struct Request {
    std::string method;
    std::string path;                             ///< no query string
    std::string query;                            ///< raw, after '?'
    std::map<std::string, std::string> headers;   ///< keys lowercased
    std::string body;

    /// Decoded query parameter, or `fallback`.
    std::string param(const std::string& key, const std::string& fallback = {}) const;
  };

  struct Response {
    int status = 200;
    std::string contentType = "application/json";
    std::string body;
    std::vector<uint8_t> binaryBody;
    bool useBinary = false;
    std::map<std::string, std::string> extraHeaders;

    void json(const std::string& text) {
      contentType = "application/json";
      body = text;
    }
    void text(const std::string& value) {
      contentType = "text/plain; charset=utf-8";
      body = value;
    }
    void error(int code, const std::string& message);
  };

  using Handler = std::function<void(const Request&, Response&)>;

  /// Connections served at once. The control page holds one keep-alive
  /// connection per tab and a browser opens at most six per host; Companion, a
  /// supervisor and a few operators come to a dozen or two. 64 is several times
  /// that, and its worst case — 64 threads at 512 KB of stack — is 32 MB,
  /// against roughly 700 MB for the 1,410 threads #12 left behind.
  static constexpr int kDefaultMaxConnections = 64;

  HttpServer();
  ~HttpServer();

  /// Both before start(). The idle timeout bounds a wait for a request; the
  /// send timeout bounds a wait for a client to read its response.
  void setMaxConnections(int maxConnections);
  void setTimeouts(std::chrono::milliseconds idle, std::chrono::milliseconds send);

  /// `token`, if set, must appear as ?token=... or an Authorization: Bearer
  /// header on every request. The only protection there is — see the class note.
  bool start(const std::string& bindAddress, int port, const std::string& token,
             Handler handler, std::string& error);
  void stop();

  bool isRunning() const { return running_.load(); }
  /// The port actually bound — the one the system chose, if start() was asked
  /// for port 0.
  int port() const { return port_; }
  int activeConnections() const { return activeConnections_.load(); }
  /// Connections turned away with a 503 because the cap was reached.
  int64_t refusedConnections() const { return refusedConnections_.load(); }
  const std::string& bindAddress() const { return bindAddress_; }

  /// Whether `bindAddress:port` can be listened on right now.
  ///
  /// Advisory, and inherently racy — something can take the port a microsecond
  /// later. It exists so that the far more common case, a second WebLinked
  /// started while the first is still running, is caught before CefInitialize
  /// rather than after: past that point the clash surfaces as a Chromium
  /// profile-error dialog, which says nothing about ports.
  static bool portAvailable(const std::string& bindAddress, int port);

  /// Percent-decoding, exposed for the OSC path which shares the same encoding
  /// habits when a URL arrives from Companion.
  static std::string urlDecode(const std::string& text);

 private:
  void acceptLoop();
  void serveConnection(int socket);
  /// Answers 503 and closes, on the accept thread, in bounded time.
  void refuse(int socket);
  bool authorised(const Request& request) const;

  std::atomic<bool> running_{false};
  int listenSocket_ = -1;
  int port_ = 0;
  std::string bindAddress_;
  std::string token_;
  Handler handler_;
  std::thread acceptThread_;
  std::atomic<int> activeConnections_{0};
  std::atomic<int64_t> refusedConnections_{0};
  int maxConnections_ = kDefaultMaxConnections;
  std::chrono::milliseconds idleTimeout_{15'000};
  std::chrono::milliseconds sendTimeout_{15'000};

  /// Every connection still open, so stop() can end the keep-alive ones rather
  /// than leave them serving requests into a process that is shutting down.
  std::mutex connectionsMutex_;
  std::set<int> connections_;
};

}  // namespace weblinked
