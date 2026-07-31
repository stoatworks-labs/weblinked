#include "control/http_server.h"

#include <algorithm>
#include <cstring>
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
#endif

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

/// Reads until the header terminator, returning everything read (which may
/// include the start of the body).
bool readHeaders(socket_t socket, std::string& buffer) {
  char chunk[4096];
  while (buffer.find("\r\n\r\n") == std::string::npos) {
    if (buffer.size() > 64 * 1024) {
      return false;  // no legitimate request header is this large
    }
    const auto received = ::recv(socket, chunk, sizeof(chunk), 0);
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
        ::recv(socket, chunk, std::min(sizeof(chunk), wanted - buffer.size()), 0);
    if (received <= 0) {
      return false;
    }
    buffer.append(chunk, static_cast<size_t>(received));
  }
  return true;
}

bool sendAll(socket_t socket, const char* data, size_t length) {
  size_t sent = 0;
  while (sent < length) {
    const auto wrote = ::send(socket, data + sent, length - sent, 0);
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

  listenSocket_ = static_cast<int>(listener);
  running_.store(true);
  acceptThread_ = std::thread([this] { acceptLoop(); });

  diag::info("control: HTTP listening on %s:%d%s", bindAddress.c_str(), port,
             token.empty() ? "" : " (token required)");
  return true;
}

void HttpServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (listenSocket_ >= 0) {
    // Closing the listener is what breaks the blocking accept().
    WL_CLOSE_SOCKET(static_cast<socket_t>(listenSocket_));
    listenSocket_ = -1;
  }
  if (acceptThread_.joinable()) {
    acceptThread_.join();
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

    // Detached, with a counter so shutdown can at least report stragglers. A
    // join-all would mean waiting on a client that has wandered off.
    ++activeConnections_;
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

void HttpServer::serveConnection(int rawSocket) {
  const auto socket = static_cast<socket_t>(rawSocket);

  // Idle timeout, so a connection that opens and says nothing does not hold a
  // thread for the life of the process.
#if defined(_WIN32)
  DWORD timeout = 15000;
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  timeval timeout{};
  timeout.tv_sec = 15;
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
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

  WL_CLOSE_SOCKET(socket);
}

}  // namespace weblinked
