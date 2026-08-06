#include "Helper.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

extern char** environ;

namespace weblinked {
namespace {

bool fileIsExecutable(const std::string& path) {
  return !path.empty() && ::access(path.c_str(), X_OK) == 0;
}

/// Asks the kernel for a free port by binding to 0 and reading back what it
/// chose, then closes the socket and hands the number to the child.
///
/// This is a race — something else could take the port in the gap — and it is
/// the standard one, accepted because the alternative is worse. WebLinked
/// refuses to start on a port already in use and says so, which turns the race
/// into a clear message rather than two processes quietly sharing a control
/// API. Returns 0 if even this fails, in which case the child gets no --port
/// and falls back to its own default.
int freePort() {
  const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    return 0;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = 0;
  int port = 0;
  if (::bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0) {
    socklen_t length = sizeof(address);
    if (::getsockname(sock, reinterpret_cast<sockaddr*>(&address), &length) == 0) {
      port = ntohs(address.sin_port);
    }
  }
  ::close(sock);
  return port;
}

/// A one-shot HTTP POST to 127.0.0.1, because the only request this plugin
/// ever makes is `/api/url` and linking a HTTP client for it would be absurd.
/// Not a general client: no redirects, no chunked bodies, no keep-alive.
bool postLocal(int port, const std::string& path, const std::string& body) {
  if (port <= 0) {
    return false;
  }
  const int sock = ::socket(AF_INET, SOCK_STREAM, 0);
  if (sock < 0) {
    return false;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  address.sin_port = htons(static_cast<uint16_t>(port));

  // A timeout on both halves, because this runs on the render thread: a helper
  // that is alive but wedged must cost one frame, not the show.
  timeval timeout{};
  timeout.tv_sec = 1;
  ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

  if (::connect(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    ::close(sock);
    return false;
  }

  std::string request = "POST " + path + " HTTP/1.1\r\n";
  request += "Host: 127.0.0.1\r\n";
  request += "Content-Type: application/json\r\n";
  request += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  request += "Connection: close\r\n\r\n";
  request += body;

  const ssize_t written = ::send(sock, request.data(), request.size(), 0);
  bool ok = written == static_cast<ssize_t>(request.size());
  if (ok) {
    char response[256] = {0};
    const ssize_t read = ::recv(sock, response, sizeof(response) - 1, 0);
    ok = read > 0 && std::strstr(response, " 200 ") != nullptr;
  }
  ::close(sock);
  return ok;
}

/// Minimal JSON string escaping. A URL can legitimately contain a quote or a
/// backslash, and building the body by concatenation without this would let one
/// break the request — or inject a second field into it.
std::string jsonEscape(const std::string& text) {
  std::string out;
  out.reserve(text.size() + 8);
  for (const char c : text) {
    switch (c) {
      case '"': out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      default:
        if (static_cast<unsigned char>(c) < 0x20) {
          char buffer[8];
          std::snprintf(buffer, sizeof(buffer), "\\u%04x", c);
          out += buffer;
        } else {
          out += c;
        }
    }
  }
  return out;
}

}  // namespace

std::string Helper::findBinary() {
  // The override comes first and exists for development: pointing at a build
  // tree is the only way to test a plugin against a WebLinked that is not
  // installed.
  if (const char* override = std::getenv("WEBLINKED_BINARY")) {
    if (fileIsExecutable(override)) {
      return override;
    }
  }

  const char* home = std::getenv("HOME");
  std::vector<std::string> candidates = {
      "/Applications/WebLinked.app/Contents/MacOS/WebLinked",
  };
  if (home != nullptr) {
    candidates.push_back(std::string(home) +
                         "/Applications/WebLinked.app/Contents/MacOS/WebLinked");
  }

  for (const auto& candidate : candidates) {
    if (fileIsExecutable(candidate)) {
      return candidate;
    }
  }
  return {};
}

Helper::~Helper() { stop(); }

bool Helper::start(const std::string& url, const std::string& sourceName,
                   std::string& error) {
  stop();

  const std::string binary = findBinary();
  if (binary.empty()) {
    error =
        "WebLinked not found. Install it to /Applications, or set "
        "WEBLINKED_BINARY to the executable.";
    return false;
  }

  port_ = freePort();
  sourceName_ = sourceName;

  const std::string portText = std::to_string(port_);
  std::vector<std::string> arguments = {
      binary,
      "--url", url,
      "--syphon=" + sourceName,
      // Headless because the layer is the output: no preview window, no tray.
      "--headless",
      // Its own settings are not wanted. The composition is the configuration,
      // and a settings file shared between instances would have them fighting
      // over the same outputs.
      "--no-settings",
  };
  if (port_ > 0) {
    arguments.push_back("--port");
    arguments.push_back(portText);
  }

  std::vector<char*> argv;
  argv.reserve(arguments.size() + 1);
  for (auto& argument : arguments) {
    argv.push_back(argument.data());
  }
  argv.push_back(nullptr);

  // POSIX_SPAWN_SETPGROUP with pgroup 0 puts the child in its own process
  // group. Without it the child inherits Resolume's, and a Ctrl-C or a group
  // signal aimed at Resolume would take the browsers with it — or, worse, a
  // signal aimed at a helper would reach Resolume.
  posix_spawnattr_t attributes;
  posix_spawnattr_init(&attributes);
  posix_spawnattr_setflags(&attributes, POSIX_SPAWN_SETPGROUP);
  posix_spawnattr_setpgroup(&attributes, 0);

  pid_t pid = 0;
  const int result =
      ::posix_spawn(&pid, binary.c_str(), nullptr, &attributes, argv.data(), environ);
  posix_spawnattr_destroy(&attributes);

  if (result != 0) {
    error = std::string("could not start WebLinked: ") + std::strerror(result);
    port_ = 0;
    return false;
  }
  pid_ = pid;
  return true;
}

void Helper::stop() {
  if (pid_ == 0) {
    return;
  }
  const pid_t pid = static_cast<pid_t>(pid_);
  pid_ = 0;
  port_ = 0;

  // SIGTERM, not SIGKILL: WebLinked installs a handler for it and shuts
  // Chromium down in order, which retires the Syphon server so no consumer is
  // left holding a dead source. It is given a moment, then killed — a page
  // that will not close must not be able to keep Resolume waiting.
  ::kill(pid, SIGTERM);
  for (int attempt = 0; attempt < 100; ++attempt) {
    int status = 0;
    const pid_t reaped = ::waitpid(pid, &status, WNOHANG);
    if (reaped == pid || reaped < 0) {
      return;
    }
    ::usleep(20 * 1000);
  }
  ::kill(pid, SIGKILL);
  int status = 0;
  ::waitpid(pid, &status, 0);
}

bool Helper::alive() {
  if (pid_ == 0) {
    return false;
  }
  int status = 0;
  const pid_t reaped = ::waitpid(static_cast<pid_t>(pid_), &status, WNOHANG);
  if (reaped == 0) {
    return true;  // still running
  }
  // Exited, or gone. Either way it is reaped now and not a zombie.
  pid_ = 0;
  port_ = 0;
  return false;
}

bool Helper::setUrl(const std::string& url) {
  if (pid_ == 0 || port_ <= 0) {
    return false;
  }
  return postLocal(port_, "/api/url", "{\"url\":\"" + jsonEscape(url) + "\"}");
}

}  // namespace weblinked
