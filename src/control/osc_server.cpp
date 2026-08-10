#include "control/osc_server.h"

#include <cstring>

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
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
using socket_t = int;
#define WL_INVALID_SOCKET (-1)
#define WL_CLOSE_SOCKET ::close
#endif

#include "core/socket_inherit.h"
#include "diag/diag.h"

namespace weblinked {
namespace {

/// The wire size of an OSC string of `textLength` characters.
///
/// OSC strings are NUL-terminated and then padded to a multiple of four, and the
/// terminator is not optional: a string whose length is already a multiple of
/// four still gets four NULs after it. So this is the text, plus at least one
/// NUL, rounded up — and the "+ 4" is what supplies that mandatory NUL, which is
/// why the caller must pass the text length and *not* the length including it.
size_t padded(size_t textLength) {
  return (textLength + 4) & ~static_cast<size_t>(3);
}

/// Reads an OSC string: NUL-terminated, then padded to 4 bytes.
bool readString(const uint8_t* data, size_t length, size_t& offset,
                std::string& out) {
  if (offset >= length) {
    return false;
  }
  const auto* start = reinterpret_cast<const char*>(data + offset);
  const size_t available = length - offset;
  const size_t textLength = ::strnlen(start, available);
  if (textLength == available) {
    return false;  // unterminated
  }
  out.assign(start, textLength);
  // padded() already counts the terminator. Passing textLength + 1 here counted
  // it twice, which over-advanced by four bytes for any string whose length was
  // 3 mod 4 — the offset then ran past the end, this returned false, and the
  // whole message was dropped without a word. A quarter of all URLs and scripts
  // are that length, so /weblinked/url worked for most addresses and silently
  // did nothing for the rest.
  offset += padded(textLength);
  return offset <= length;
}

bool readInt32(const uint8_t* data, size_t length, size_t& offset, int32_t& out) {
  if (offset + 4 > length) {
    return false;
  }
  uint32_t network = 0;
  std::memcpy(&network, data + offset, 4);
  out = static_cast<int32_t>(ntohl(network));
  offset += 4;
  return true;
}

bool readFloat32(const uint8_t* data, size_t length, size_t& offset, float& out) {
  int32_t bits = 0;
  if (!readInt32(data, length, offset, bits)) {
    return false;
  }
  std::memcpy(&out, &bits, 4);
  return true;
}

}  // namespace

OscServer::OscServer() = default;

OscServer::~OscServer() { stop(); }

void OscServer::decodePacket(const uint8_t* data, size_t length,
                             const Handler& handler) {
  if (data == nullptr || length < 4) {
    return;
  }

  // A bundle: "#bundle", a timetag, then length-prefixed sub-packets.
  if (length >= 8 && std::memcmp(data, "#bundle", 7) == 0) {
    size_t offset = 16;  // "#bundle\0" plus the 8-byte timetag
    while (offset + 4 <= length) {
      int32_t size = 0;
      size_t sizeOffset = offset;
      if (!readInt32(data, length, sizeOffset, size) || size <= 0) {
        return;
      }
      offset = sizeOffset;
      if (offset + static_cast<size_t>(size) > length) {
        return;
      }
      // Timetags are ignored: see the class note.
      decodePacket(data + offset, static_cast<size_t>(size), handler);
      offset += static_cast<size_t>(size);
    }
    return;
  }

  size_t offset = 0;
  Message message;
  if (!readString(data, length, offset, message.address)) {
    return;
  }
  if (message.address.empty() || message.address[0] != '/') {
    return;
  }

  // The type tag string is technically optional in OSC 1.0; a message without
  // one carries no arguments, which is a valid trigger.
  std::string typeTags;
  if (offset < length && data[offset] == ',') {
    if (!readString(data, length, offset, typeTags)) {
      return;
    }
  }

  for (size_t i = 1; i < typeTags.size(); ++i) {
    switch (typeTags[i]) {
      case 'i': {
        int32_t value = 0;
        if (!readInt32(data, length, offset, value)) return;
        message.intArgs.push_back(value);
        break;
      }
      case 'f': {
        float value = 0.0f;
        if (!readFloat32(data, length, offset, value)) return;
        message.floatArgs.push_back(value);
        break;
      }
      case 's':
      case 'S': {
        std::string value;
        if (!readString(data, length, offset, value)) return;
        message.stringArgs.push_back(value);
        break;
      }
      case 'T':
        message.intArgs.push_back(1);  // OSC true carries no payload
        break;
      case 'F':
        message.intArgs.push_back(0);
        break;
      case 'N':
      case 'I':
        break;  // null and infinitum have no payload
      case 'b': {
        // Blob: skip it rather than fail the whole message, so an unrelated
        // argument does not discard a valid command.
        int32_t size = 0;
        if (!readInt32(data, length, offset, size) || size < 0) return;
        offset += padded(static_cast<size_t>(size));
        if (offset > length) return;
        break;
      }
      case 'd':
      case 'h':
      case 't':
        offset += 8;  // 64-bit types we do not use
        if (offset > length) return;
        break;
      default:
        // An unknown tag means the rest of the payload can no longer be located.
        return;
    }
  }

  if (handler) {
    handler(message);
  }
}

bool OscServer::start(const std::string& bindAddress, int port, Handler handler,
                      std::string& error) {
  if (running_.load()) {
    error = "osc server already running";
    return false;
  }

#if defined(_WIN32)
  WSADATA wsaData;
  WSAStartup(MAKEWORD(2, 2), &wsaData);
#endif

  handler_ = std::move(handler);
  port_ = port;

  const socket_t sock = ::socket(AF_INET, SOCK_DGRAM, 0);
  if (sock == WL_INVALID_SOCKET) {
    error = "socket() failed for OSC";
    return false;
  }
  preventSocketInheritance(static_cast<std::intptr_t>(sock));

  int reuse = 1;
  ::setsockopt(sock, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char*>(&reuse), sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_port = htons(static_cast<uint16_t>(port));
  if (bindAddress.empty() || bindAddress == "0.0.0.0") {
    address.sin_addr.s_addr = INADDR_ANY;
  } else if (::inet_pton(AF_INET, bindAddress.c_str(), &address.sin_addr) != 1) {
    WL_CLOSE_SOCKET(sock);
    error = "not a valid OSC bind address: " + bindAddress;
    return false;
  }

  if (::bind(sock, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
    WL_CLOSE_SOCKET(sock);
    error = "cannot bind UDP " + bindAddress + ":" + std::to_string(port);
    return false;
  }

  // A receive timeout is what lets the loop notice it has been asked to stop.
#if defined(_WIN32)
  DWORD timeout = 250;
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
               reinterpret_cast<const char*>(&timeout), sizeof(timeout));
#else
  timeval timeout{};
  timeout.tv_usec = 250000;
  ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
#endif

  socket_ = static_cast<int>(sock);
  running_.store(true);
  thread_ = std::thread([this] { receiveLoop(); });

  diag::info("control: OSC listening on %s:%d",
             bindAddress.empty() ? "0.0.0.0" : bindAddress.c_str(), port);
  return true;
}

void OscServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (thread_.joinable()) {
    thread_.join();
  }
  if (socket_ >= 0) {
    WL_CLOSE_SOCKET(static_cast<socket_t>(socket_));
    socket_ = -1;
  }
}

void OscServer::receiveLoop() {
  std::vector<uint8_t> buffer(65536);
  while (running_.load()) {
    const auto received = ::recv(static_cast<socket_t>(socket_),
                                 reinterpret_cast<char*>(buffer.data()),
                                 static_cast<int>(buffer.size()), 0);
    if (received <= 0) {
      continue;  // timeout, or a socket closed under us during shutdown
    }
    messages_.fetch_add(1, std::memory_order_relaxed);
    decodePacket(buffer.data(), static_cast<size_t>(received), handler_);
  }
}

}  // namespace weblinked
