// The control API's HTTP server — the third part of issue #12.
//
// On a real rig a DeckLink driver deadlock left every GET /api/state waiting
// for ever, and with a thread per connection and no cap, a client polling every
// 200 ms had parked 1,410 threads by the time someone killed the process. The
// engine half of the fix stops requests waiting for ever; this half makes sure
// that if they ever do again, the server stays bounded and says so.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "control/http_server.h"
#include "test_support.h"

using namespace weblinked;
using std::chrono::milliseconds;
using Clock = std::chrono::steady_clock;

namespace {

#if defined(_WIN32)
using Socket = SOCKET;
const Socket kNoSocket = INVALID_SOCKET;
void closeSocket(Socket socket) { ::closesocket(socket); }
#else
using Socket = int;
const Socket kNoSocket = -1;
void closeSocket(Socket socket) { ::close(socket); }
#endif

void ignoreSigpipe() {
#if !defined(_WIN32)
  // A client closing while the server writes must not kill the test binary.
  std::signal(SIGPIPE, SIG_IGN);
#endif
}

Socket connectTo(int port) {
  const Socket socket = ::socket(AF_INET, SOCK_STREAM, 0);
  if (socket == kNoSocket) {
    return kNoSocket;
  }
  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  ::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
  if (::connect(socket, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    closeSocket(socket);
    return kNoSocket;
  }
  // Never let the test itself hang on a server that misbehaves.
#if defined(_WIN32)
  const DWORD timeout = 5000;
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  timeval timeout{};
  timeout.tv_sec = 5;
  ::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif
  return socket;
}

bool sendText(Socket socket, const std::string& text) {
  return ::send(socket, text.data(), static_cast<int>(text.size()), 0) ==
         static_cast<int>(text.size());
}

std::string get(const std::string& path) {
  return "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
}

/// One response: headers, then Content-Length bytes of body. Empty if the
/// connection closed or timed out first.
std::string readResponse(Socket socket) {
  std::string data;
  char chunk[4096];
  size_t wanted = std::string::npos;
  while (wanted == std::string::npos || data.size() < wanted) {
    const auto received = ::recv(socket, chunk, static_cast<int>(sizeof(chunk)), 0);
    if (received <= 0) {
      break;
    }
    data.append(chunk, static_cast<size_t>(received));
    const auto headerEnd = data.find("\r\n\r\n");
    if (wanted == std::string::npos && headerEnd != std::string::npos) {
      const auto lengthAt = data.find("Content-Length: ");
      const size_t length =
          lengthAt == std::string::npos
              ? 0
              : static_cast<size_t>(std::stoul(data.substr(lengthAt + 16)));
      wanted = headerEnd + 4 + length;
    }
  }
  return data;
}

bool startsWith(const std::string& text, const std::string& prefix) {
  return text.rfind(prefix, 0) == 0;
}

/// A handler that parks on /hang until released — standing in for an engine
/// whose output driver has stopped answering.
struct ParkingHandler {
  std::mutex mutex;
  std::condition_variable signal;
  bool released = false;
  int parked = 0;

  HttpServer::Handler handler() {
    return [this](const HttpServer::Request& request, HttpServer::Response& response) {
      if (request.path == "/hang") {
        std::unique_lock<std::mutex> lock(mutex);
        ++parked;
        signal.notify_all();
        signal.wait(lock, [this] { return released; });
      }
      response.json("{\"ok\":true}");
    };
  }

  bool waitForParked(int count) {
    std::unique_lock<std::mutex> lock(mutex);
    return signal.wait_for(lock, std::chrono::seconds(5),
                           [&] { return parked >= count; });
  }

  void release() {
    {
      std::lock_guard<std::mutex> lock(mutex);
      released = true;
    }
    signal.notify_all();
  }
};

bool waitUntil(const std::function<bool()>& condition, milliseconds limit) {
  const auto deadline = Clock::now() + limit;
  while (Clock::now() < deadline) {
    if (condition()) {
      return true;
    }
    std::this_thread::sleep_for(milliseconds(10));
  }
  return condition();
}

}  // namespace

WEBLINKED_TEST(http_refuses_with_503_beyond_the_connection_cap) {
  ignoreSigpipe();
  ParkingHandler parking;
  HttpServer server;
  server.setMaxConnections(3);
  std::string error;
  CHECK(server.start("127.0.0.1", 0, "", parking.handler(), error));
  CHECK(server.port() > 0);

  // Three requests park in the handler, as #12's did in Engine::state().
  std::vector<Socket> parked;
  for (int i = 0; i < 3; ++i) {
    const Socket socket = connectTo(server.port());
    CHECK(socket != kNoSocket);
    CHECK(sendText(socket, get("/hang")));
    parked.push_back(socket);
  }
  CHECK(parking.waitForParked(3));
  CHECK_EQ(server.activeConnections(), 3);

  // The fourth is answered at once, with a reason, rather than given a thread
  // of its own to park in.
  const auto start = Clock::now();
  const Socket extra = connectTo(server.port());
  CHECK(extra != kNoSocket);
  CHECK(sendText(extra, get("/api/state")));
  const std::string refused = readResponse(extra);
  const auto took =
      std::chrono::duration_cast<milliseconds>(Clock::now() - start).count();
  closeSocket(extra);
  CHECK(startsWith(refused, "HTTP/1.1 503"));
  CHECK(refused.find("Retry-After: 1") != std::string::npos);
  CHECK(refused.find("Connection: close") != std::string::npos);
  CHECK(refused.find("too many control connections") != std::string::npos);
  CHECK(took < 2'000);
  CHECK_EQ(server.refusedConnections(), int64_t{1});
  CHECK_EQ(server.activeConnections(), 3);

  // A burst stays bounded: every one refused, none given a thread.
  for (int i = 0; i < 20; ++i) {
    const Socket socket = connectTo(server.port());
    if (socket == kNoSocket) {
      continue;
    }
    sendText(socket, get("/api/state"));
    CHECK(startsWith(readResponse(socket), "HTTP/1.1 503"));
    closeSocket(socket);
    CHECK(server.activeConnections() <= 3);
  }
  CHECK_EQ(server.refusedConnections(), int64_t{21});

  // Once the handlers finish, the parked requests complete and the slots free.
  parking.release();
  for (const Socket socket : parked) {
    CHECK(startsWith(readResponse(socket), "HTTP/1.1 200"));
    closeSocket(socket);
  }
  CHECK(waitUntil([&] { return server.activeConnections() == 0; },
                  std::chrono::seconds(5)));

  const Socket after = connectTo(server.port());
  CHECK(after != kNoSocket);
  CHECK(sendText(after, get("/api/state")));
  CHECK(startsWith(readResponse(after), "HTTP/1.1 200"));
  closeSocket(after);

  server.stop();
}

WEBLINKED_TEST(http_closes_a_connection_that_never_sends_a_request) {
  ignoreSigpipe();
  ParkingHandler parking;
  HttpServer server;
  server.setTimeouts(milliseconds(200), milliseconds(200));
  std::string error;
  CHECK(server.start("127.0.0.1", 0, "", parking.handler(), error));

  const Socket silent = connectTo(server.port());
  CHECK(silent != kNoSocket);
  CHECK(waitUntil([&] { return server.activeConnections() == 1; },
                  std::chrono::seconds(2)));

  // Says nothing; the server gives up on it rather than holding a thread for
  // the life of the process.
  const auto start = Clock::now();
  char byte = 0;
  const auto received = ::recv(silent, &byte, 1, 0);
  const auto took =
      std::chrono::duration_cast<milliseconds>(Clock::now() - start).count();
  CHECK_EQ(static_cast<int>(received), 0);  // orderly close from the server
  CHECK(took < 3'000);
  closeSocket(silent);
  CHECK(waitUntil([&] { return server.activeConnections() == 0; },
                  std::chrono::seconds(2)));

  server.stop();
}
