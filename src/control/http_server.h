#pragma once

#include <atomic>
#include <functional>
#include <map>
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

  HttpServer();
  ~HttpServer();

  /// `token`, if set, must appear as ?token=... or an Authorization: Bearer
  /// header on every request. The only protection there is — see the class note.
  bool start(const std::string& bindAddress, int port, const std::string& token,
             Handler handler, std::string& error);
  void stop();

  bool isRunning() const { return running_.load(); }
  int port() const { return port_; }
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
  bool authorised(const Request& request) const;

  std::atomic<bool> running_{false};
  int listenSocket_ = -1;
  int port_ = 0;
  std::string bindAddress_;
  std::string token_;
  Handler handler_;
  std::thread acceptThread_;
  std::atomic<int> activeConnections_{0};
};

}  // namespace weblinked
