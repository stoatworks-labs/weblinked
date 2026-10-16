#include "control/http_server.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <sstream>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
using socket_t = SOCKET;
#define WL_INVALID_SOCKET INVALID_SOCKET
#define WL_CLOSE_SOCKET closesocket
#define WL_SHUT_BOTH SD_BOTH
#define WL_SHUT_SEND SD_SEND
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
#define WL_INVALID_SOCKET (-1)
#define WL_CLOSE_SOCKET ::close
#define WL_SHUT_BOTH SHUT_RDWR
#define WL_SHUT_SEND SHUT_WR
#endif

#include "core/socket_inherit.h"
#include "diag/diag.h"

namespace weblinked {
namespace {

#if defined(_WIN32)
/// Winsock needs initialising exactly once per process.
struct WinsockGuard {
  WinsockGuard() {
    WSADATA data;
    WSAStartup(MAKEWORD(2, 2), &data);
  }
  ~WinsockGuard() { WSACleanup(); }
};
void ensureWinsock() { static WinsockGuard guard; }
#else
void ensureWinsock() {}
#endif

std::string lowercase(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return text;
}

std::string trim(const std::string& text) {
  const auto begin = text.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) {
    return {};
  }
  const auto end = text.find_last_not_of(" \t\r\n");
  return text.substr(begin, end - begin + 1);
}

const char* statusText(int status) {
  switch (status) {
    case 200: return "OK";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 409: return "Conflict";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "OK";
  }
}

/// Winsock's recv/send take an `int` length where POSIX takes `size_t`, so a
/// bare size_t argument is a narrowing conversion — C4267, which is an error
/// under CEF's /WX. Clamping here rather than casting at each call site keeps
/// the intent visible: a single read or write is bounded, and the loops around
/// them already handle a short transfer.
int toSocketLength(size_t bytes) {
  constexpr size_t kMax = static_cast<size_t>((std::numeric_limits<int>::max)());
  return static_cast<int>(bytes < kMax ? bytes : kMax);
}

/// Reads until the header terminator, returning everything read (which may
/// include the start of the body).
bool readHeaders(socket_t socket, std::string& buffer) {
  char chunk[4096];
  while (buffer.find("\r\n\r\n") == std::string::npos) {
    if (buffer.size() > 64 * 1024) {
      return false;  // no legitimate request header is this large
    }
    const auto received = ::recv(socket, chunk, toSocketLength(sizeof(chunk)), 0);
    if (received <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(received));
  }
  return true;
}

bool readExactly(socket_t socket, std::string& buffer, size_t wanted) {
  char chunk[4096];
  while (buffer.size() < wanted) {
    const auto received =
        ::recv(socket, chunk,
               toSocketLength(std::min(sizeof(chunk), wanted - buffer.size())), 0);
    if (received <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(received));
  }
  return true;
}

/// Bounds every blocking recv() and send() on `socket`. Without the send half a
/// client that stopped reading a response parked its thread in send() for good.
void setSocketTimeouts(socket_t socket, std::chrono::milliseconds receive,
                       std::chrono::milliseconds send) {
#if defined(_WIN32)
  const DWORD receiveMs = static_cast<DWORD>(receive.count());
  const DWORD sendMs = static_cast<DWORD>(send.count());
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&receiveMs), sizeof(receiveMs));
  ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO,
               reinterpret_cast<const char*>(&sendMs), sizeof(sendMs));
#else
  const auto toTimeval = [](std::chrono::milliseconds ms) {
    timeval value{};
    value.tv_sec = static_cast<decltype(value.tv_sec)>(ms.count() / 1000);
    value.tv_usec = static_cast<decltype(value.tv_usec)>((ms.count() % 1000) * 1000);
    return value;
  };
  const timeval receiveTime = toTimeval(receive);
  const timeval sendTime = toTimeval(send);
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &receiveTime, sizeof(receiveTime));
  ::setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &sendTime, sizeof(sendTime));
#endif
#if defined(__APPLE__)
  // The app ignores SIGPIPE process-wide (main.cpp); a library should not
  // depend on its host having done so. Linux has no socket option for this.
  int noSigPipe = 1;
  ::setsockopt(socket, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
#endif
}

/// Whether the recv() or send() that just failed did so because its timeout
/// expired, rather than because the connection is gone.
bool lastCallTimedOut() {
#if defined(_WIN32)
  return ::WSAGetLastError() == WSAETIMEDOUT;
#else
  return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

bool sendAll(socket_t socket, const char* data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    const auto wrote = ::send(socket, data + sent, toSocketLength(length - sent), 0);
    if (wrote <= 0) {
      return false;
    }
    sent += static_cast<size_t>(wrote);
  }
  return true;
}

}  // namespace

void HttpServer::Response::error(int code, const std::string& message) {
  status = code;
  contentType = "application/json";
  // Escape just enough for a message that may contain a quote from a URL.
  std::string escaped;
  for (char c : message) {
    if (c == '"' || c == '\\') {
      escaped += '\\';
    }
    if (c == '\n') {
      escaped += "\\n";
      continue;
    }
    escaped += c;
  }
  body = "{\"error\":\"" + escaped + "\"}";
}

std::string HttpServer::Request::param(const std::string& key,
                                       const std::string& fallback) const {
  size_t position = 0;
  while (position < query.size()) {
    const auto ampersand = query.find('&', position);
    const std::string pair = query.substr(
        position, ampersand == std::string::npos ? std::string::npos
                                                 : ampersand - position);
    const auto equals = pair.find('=');
    if (equals != std::string::npos && pair.substr(0, equals) == key) {
      return urlDecode(pair.substr(equals + 1));
    }
    if (equals == std::string::npos && pair == key) {
      return "";
    }
    if (ampersand == std::string::npos) {
      break;
    }
    position = ampersand + 1;
  }
  return fallback;
}

std::string HttpServer::urlDecode(const std::string& text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '+') {
      out += ' ';
    } else if (text[i] == '%' && i + 2 < text.size()) {
      const auto hex = text.substr(i + 1, 2);
      char* end = nullptr;
      const long value = std::strtol(hex.c_str(), &end, 16);
      if (end != nullptr && *end == '\0') {
        out += static_cast<char>(value);
        i += 2;
      } else {
        out += text[i];
      }
    } else {
      out += text[i];
    }
  }
  return out;
}

HttpServer::HttpServer() = default;

HttpServer::~HttpServer() { stop(); }

void HttpServer::setMaxConnections(int maxConnections) {
  maxConnections_ = std::max(1, maxConnections);
}

void HttpServer::setTimeouts(std::chrono::milliseconds idle,
                             std::chrono::milliseconds send) {
  idleTimeout_ = idle;
  sendTimeout_ = send;
}

bool HttpServer::portAvailable(const std::string& bindAddress, int port) {
  ensureWinsock();

  const socket_t probe = ::socket(AF_INET, SOCK_STREAM, 0);
  if (probe == WL_INVALID_SOCKET) {
    // Cannot tell, so do not block a launch on it.
    return true;
  }

  // Matching start()'s options, or the probe would answer a different question
  // from the one the real bind is about to ask.
  int reuse = 1;
  ::setsockopt(probe, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (bindAddress.empty() || bindAddress == "0.0.0.0") {
    address.sin_addr.s_addr = INADDR_ANY;
  } else if (::inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) {
    // A malformed address is start()'s error to report, with its own message.
    WL_CLOSE_SOCKET(probe);
    return true;
  }

  const bool bound =
      ::bind(probe, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0;
  WL_CLOSE_SOCKET(probe);
  return bound;
}

bool HttpServer::start(const std::string& bindAddress, int port,
                       const std::string& token, Handler handler,
                       std::string& error) {
  if (running_.load()) {
    error = "http server already running";
    return false;
  }
  ensureWinsock();

  bindAddress_ = bindAddress;
  port_ = port;
  token_ = token;
  handler_ = std::move(handler);

  const socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listener == WL_INVALID_SOCKET) {
    error = "socket() failed";
    return false;
  }
  preventSocketInheritance(static_cast<std::intptr_t>(listener));

  int reuse = 1;
  ::setsockopt(listener, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (bindAddress.empty() || bindAddress == "0.0.0.0") {
    address.sin_addr.s_addr = INADDR_ANY;
  } else if (::inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) {
    WL_CLOSE_SOCKET(listener);
    error = "not a valid bind address: " + bindAddress;
    return false;
  }

  if (::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    WL_CLOSE_SOCKET(listener);
    error = "cannot bind " + bindAddress + ":" + std::to_string(port) +
            " (already in use?)";
    return false;
  }
  if (::listen(listener, 16) != 0) {
    WL_CLOSE_SOCKET(listener);
    error = "listen() failed";
    return false;
  }

  // Port 0 asks the system to choose; report the one it chose.
  sockaddr_in bound{};
#if defined(_WIN32)
  int boundLength = sizeof(bound);
#else
  socklen_t boundLength = sizeof(bound);
#endif
  if (::getsockname(listener, reinterpret_cast<sockaddr*>(&bound), &boundLength) == 0) {
    port_ = ntohs(bound.sin_port);
  }

  listenSocket_ = static_cast<int>(listener);
  running_.store(true);
  acceptThread_ = std::thread([this] { acceptLoop(); });

  diag::info("control: HTTP listening on %s:%d%s", bindAddress.c_str(), port_,
             token.empty() ? "" : " (token required)");
  return true;
}

void HttpServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (listenSocket_ >= 0) {
    // shutdown() before close(), because close() alone does not wake a thread
    // blocked in accept() on Linux: the join below then waited for a connection
    // that never came, and SIGTERM never finished. macOS and Windows happen to
    // wake on the close; shutdown() is what every platform agrees on.
    ::shutdown(static_cast<socket_t>(listenSocket_), WL_SHUT_BOTH);
    WL_CLOSE_SOCKET(static_cast<socket_t>(listenSocket_));
    listenSocket_ = -1;
  }
  if (acceptThread_.joinable()) {
    acceptThread_.join();
  }

  // The listener is shut, but a keep-alive connection is not: the control page
  // polls on one, and its thread would go on handing requests to engines that
  // are being torn down. Shut each one — that wakes the recv() it is parked in
  // — then give a request already inside the handler a moment to finish.
  // Bounded, because a handler waiting on the thread calling this (a screen or
  // shared output marshalling to the main thread) would otherwise never finish.
  {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    for (const int connection : connections_) {
      ::shutdown(static_cast<socket_t>(connection), WL_SHUT_BOTH);
    }
  }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  while (activeConnections_.load() > 0 &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  if (activeConnections_.load() > 0) {
    diag::warn("control: %d HTTP connection(s) still busy after stop",
               activeConnections_.load());
  }
}

void HttpServer::acceptLoop() {
  while (running_.load()) {
    sockaddr_in peer{};
#if defined(_WIN32)
    int peerLength = sizeof(peer);
#else
    socklen_t peerLength = sizeof(peer);
#endif
    const socket_t client =
        ::accept(static_cast<socket_t>(listenSocket_),
                 reinterpret_cast<sockaddr*>(&peer), &peerLength);
    if (client == WL_INVALID_SOCKET) {
      if (!running_.load()) {
        break;  // the expected path through stop()
      }
      continue;
    }
    // An accepted connection needs this as much as the listener does: a child
    // process that inherits one holds the client's connection open, so the
    // browser at the other end waits for a response nobody will send.
    preventSocketInheritance(static_cast<std::intptr_t>(client));

    // The cap. Checked here, before a thread exists, because a thread per
    // connection with no limit is how one wedged engine became 1,410 parked
    // threads (#12).
    if (activeConnections_.load() >= maxConnections_) {
      refuse(static_cast<int>(client));
      continue;
    }

    // Detached, with a counter so shutdown can at least report stragglers. A
    // join-all would mean waiting on a client that has wandered off.
    ++activeConnections_;
    {
      std::lock_guard<std::mutex> lock(connectionsMutex_);
      connections_.insert(static_cast<int>(client));
    }
    std::thread([this, client]() {
      serveConnection(static_cast<int>(client));
      --activeConnections_;
    }).detach();
  }
}

bool HttpServer::authorised(const Request& request) const {
  if (token_.empty()) {
    return true;
  }
  if (request.param("token") == token_) {
    return true;
  }
  const auto header = request.headers.find("authorization");
  if (header != request.headers.end()) {
    const std::string prefix = "Bearer ";
    if (header->second.rfind(prefix, 0) == 0 &&
        header->second.substr(prefix.size()) == token_) {
      return true;
    }
  }
  return false;
}

void HttpServer::refuse(int rawSocket) {
  const auto socket = static_cast<socket_t>(rawSocket);
  const int64_t refused = ++refusedConnections_;

  // Read whatever of the request has arrived first, for at most 30 ms: closing
  // a socket with its request unread makes the kernel answer with a reset, and
  // the client then reports "connection reset" instead of the 503 that says
  // why. A real client's request is there within a millisecond of connecting;
  // the bound is for one that never sends, because this runs on the accept
  // thread and every connection behind it waits.
  setSocketTimeouts(socket, std::chrono::milliseconds(10),
                    std::chrono::milliseconds(100));
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
  std::string request;
  char chunk[4096];
  while (request.find("\r\n\r\n") == std::string::npos &&
         request.size() < 64 * 1024 && std::chrono::steady_clock::now() < deadline) {
    const auto received = ::recv(socket, chunk, toSocketLength(sizeof(chunk)), 0);
    if (received == 0 || (received < 0 && !lastCallTimedOut())) {
      break;  // the client has gone, or the socket has failed
    }
    if (received > 0) {
      request.append(chunk, static_cast<size_t>(received));
    }
  }

  const std::string body = "{\"error\":\"too many control connections (" +
                           std::to_string(maxConnections_) +
                           " open); try again shortly\"}";
  std::ostringstream head;
  head << "HTTP/1.1 503 " << statusText(503) << "\r\n"
       << "Content-Type: application/json\r\n"
       << "Content-Length: " << body.size() << "\r\n"
       << "Retry-After: 1\r\n"
       << "Cache-Control: no-store\r\n"
       << "Connection: close\r\n\r\n"
       << body;
  const std::string response = head.str();
  sendAll(socket, response.data(), response.size());
  ::shutdown(socket, WL_SHUT_SEND);
  WL_CLOSE_SOCKET(socket);

  // The first of a burst, then every hundredth, so a client hammering a full
  // server cannot fill the log as well.
  if (refused == 1 || refused % 100 == 0) {
    diag::warn("control: %d HTTP connections open, the most allowed; refused "
               "another with 503 (%lld so far). Either a client is opening "
               "connections faster than requests finish, or requests are not "
               "finishing — check /api/state for a stale output",
               maxConnections_, static_cast<long long>(refused));
  }
}

void HttpServer::serveConnection(int rawSocket) {
  const auto socket = static_cast<socket_t>(rawSocket);

  // An idle timeout, so a connection that opens and says nothing does not hold
  // a thread for the life of the process, and a send timeout, so a client that
  // stops reading does not either.
  setSocketTimeouts(socket, idleTimeout_, sendTimeout_);
#if !defined(_WIN32)
  int nodelay = 1;
  ::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
               reinterpret_cast<const char*>(&nodelay), sizeof(nodelay));
#endif

  std::string buffer;
  int servedRequests = 0;

  while (running_.load() && servedRequests < 200) {
    if (!readHeaders(socket, buffer)) {
      break;
    }
    const auto headerEnd = buffer.find("\r\n\r\n");
    const std::string headerBlock = buffer.substr(0, headerEnd);
    std::string remainder = buffer.substr(headerEnd + 4);

    Request request;
    std::istringstream stream(headerBlock);
    std::string line;
    if (!std::getline(stream, line)) {
      break;
    }
    {
      std::istringstream requestLine(line);
      std::string target;
      requestLine >> request.method >> target;
      const auto questionMark = target.find('?');
      if (questionMark == std::string::npos) {
        request.path = target;
      } else {
        request.path = target.substr(0, questionMark);
        request.query = target.substr(questionMark + 1);
      }
    }
    bool keepAlive = true;
    while (std::getline(stream, line)) {
      const auto colon = line.find(':');
      if (colon == std::string::npos) {
        continue;
      }
      const std::string key = lowercase(trim(line.substr(0, colon)));
      const std::string value = trim(line.substr(colon + 1));
      request.headers[key] = value;
      if (key == "connection" && lowercase(value).find("close") != std::string::npos) {
        keepAlive = false;
      }
    }

    size_t contentLength = 0;
    const auto lengthHeader = request.headers.find("content-length");
    if (lengthHeader != request.headers.end()) {
      contentLength = static_cast<size_t>(std::strtoul(lengthHeader->second.c_str(),
                                                       nullptr, 10));
    }
    if (contentLength > 8 * 1024 * 1024) {
      Response tooBig;
      tooBig.error(400, "request body too large");
      keepAlive = false;
      contentLength = 0;
      remainder.clear();
    }
    if (contentLength > 0) {
      if (!readExactly(socket, remainder, contentLength)) {
        break;
      }
      request.body = remainder.substr(0, contentLength);
      buffer = remainder.substr(contentLength);
    } else {
      buffer = remainder;
    }

    Response response;
    if (!authorised(request)) {
      response.error(401, "missing or incorrect token");
    } else if (handler_) {
      handler_(request, response);
    } else {
      response.error(503, "no handler installed");
    }

    const size_t bodyLength =
        response.useBinary ? response.binaryBody.size() : response.body.size();

    std::ostringstream head;
    head << "HTTP/1.1 " << response.status << ' ' << statusText(response.status)
         << "\r\n"
         << "Content-Type: " << response.contentType << "\r\n"
         << "Content-Length: " << bodyLength << "\r\n"
         // The control page polls the preview; without this it would serve a
         // stale frame from the browser's cache forever.
         << "Cache-Control: no-store\r\n"
         << "Connection: " << (keepAlive ? "keep-alive" : "close") << "\r\n";
    for (const auto& [key, value] : response.extraHeaders) {
      head << key << ": " << value << "\r\n";
    }
    head << "\r\n";

    const std::string headText = head.str();
    if (!sendAll(socket, headText.data(), headText.size())) {
      break;
    }
    if (bodyLength > 0 && request.method != "HEAD") {
      const char* data = response.useBinary
                             ? reinterpret_cast<const char*>(response.binaryBody.data())
                             : response.body.data();
      if (!sendAll(socket, data, bodyLength)) {
        break;
      }
    }

    ++servedRequests;
    if (!keepAlive) {
      break;
    }
  }

  // Out of the set before the close, so stop() can never shut a descriptor
  // number that has already been handed to something else.
  {
    std::lock_guard<std::mutex> lock(connectionsMutex_);
    connections_.erase(rawSocket);
  }
  WL_CLOSE_SOCKET(socket);
}

}  // namespace weblinked
