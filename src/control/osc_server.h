#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace weblinked {

/// A minimal OSC 1.0 receiver over UDP.
///
/// Exists because this is how the rest of the kit is driven: Companion, a
/// lighting desk, or a show-control system sends a message, the graphic changes.
/// Waiting for an HTTP client to be written is not an option at 10 minutes to
/// doors.
///
/// Implements what a control surface actually sends: address pattern, type tag
/// string, and int32/float32/string arguments, plus #bundle unwrapping. No
/// pattern matching against wildcards, no blobs, no timetag scheduling — a
/// bundle's contents are dispatched immediately, because a control surface that
/// wanted them later would not have sent them now.
class OscServer {
 public:
  /// One decoded message. Arguments are kept in their sent type; the handler
  /// coerces, because a desk that sends 1 where a float was expected should
  /// still work.
  struct Message {
    std::string address;
    std::vector<std::string> stringArgs;
    std::vector<int32_t> intArgs;
    std::vector<float> floatArgs;

    /// First string argument, or empty.
    std::string firstString() const {
      return stringArgs.empty() ? std::string{} : stringArgs.front();
    }
    /// First numeric argument as a bool, treating any non-zero as true. Returns
    /// `fallback` when the message carried no arguments at all — which is how a
    /// bare trigger address is distinguished from an explicit zero.
    bool firstBool(bool fallback) const {
      if (!intArgs.empty()) return intArgs.front() != 0;
      if (!floatArgs.empty()) return floatArgs.front() != 0.0f;
      return fallback;
    }
  };

  using Handler = std::function<void(const Message&)>;

  OscServer();
  ~OscServer();

  bool start(const std::string& bindAddress, int port, Handler handler,
             std::string& error);
  void stop();

  bool isRunning() const { return running_.load(); }
  int port() const { return port_; }
  int64_t messageCount() const { return messages_.load(); }

  /// Exposed for testing: decodes one packet, dispatching each message it holds.
  static void decodePacket(const uint8_t* data, size_t length,
                           const Handler& handler);

 private:
  void receiveLoop();

  std::atomic<bool> running_{false};
  int socket_ = -1;
  int port_ = 0;
  Handler handler_;
  std::thread thread_;
  std::atomic<int64_t> messages_{0};
};

}  // namespace weblinked
