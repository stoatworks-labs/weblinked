// macOS mDNS registration, through the system responder.
//
// Deliberately `dns_sd.h` rather than a responder of our own. mDNSResponder
// already owns UDP 5353 on every Mac, and it is the same responder the NDI
// runtime registers its senders with — so this adds a service to something
// already running rather than putting a second responder on a machine that is
// live to air. It also costs no dependency: dns_sd is in libSystem, so there is
// nothing to link, nothing to vendor and nothing to ship.

#include <dns_sd.h>
#include <netinet/in.h>
#include <sys/select.h>

#include <atomic>
#include <cerrno>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "core/mdns_service.h"
#include "diag/diag.h"

namespace weblinked {
namespace mdns {
namespace {

/// How long the poll loop blocks before re-checking the stop flag. Shutdown
/// latency, and nothing else — the socket wakes the loop the moment the
/// responder has anything to say.
constexpr int kPollTimeoutMs = 200;

}  // namespace

struct Responder::Impl {
  DNSServiceRef service = nullptr;
  std::thread poller;
  std::atomic<bool> running{false};
  mutable std::mutex mutex;
  std::string registeredName;

  /// A static member rather than a free function purely so it can see this
  /// type, which is private to Responder.
  static void DNSSD_API registerReply(DNSServiceRef service, DNSServiceFlags flags,
                                      DNSServiceErrorType error, const char* name,
                                      const char* regtype, const char* domain,
                                      void* context);
};

/// Called by the responder once the registration is live — or once it has
/// renamed us out of a conflict, which is why the confirmed name is read from
/// here rather than assumed from what was asked for.
void DNSSD_API Responder::Impl::registerReply(
    DNSServiceRef /*service*/, DNSServiceFlags /*flags*/, DNSServiceErrorType error,
    const char* name, const char* regtype, const char* /*domain*/, void* context) {
  auto* impl = static_cast<Responder::Impl*>(context);
  if (error != kDNSServiceErr_NoError) {
    diag::warn("mdns: registration failed (%d)", static_cast<int>(error));
    return;
  }
  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->registeredName = name != nullptr ? name : "";
  }
  diag::info("mdns: advertising '%s' as %s", name != nullptr ? name : "?",
             regtype != nullptr ? regtype : kServiceType);
}

Responder::Responder() : impl_(std::make_unique<Impl>()) {}

Responder::~Responder() { stop(); }

bool Responder::start(const Advertisement& advertisement, std::string& error) {
  if (impl_->running.load()) {
    error = "already advertising";
    return false;
  }

  const std::vector<uint8_t> txt = encodeTxt(advertisement.txt);
  if (txt.size() > 0xFFFF) {
    error = "TXT record too large";
    return false;
  }

  const DNSServiceErrorType status = DNSServiceRegister(
      &impl_->service, /*flags=*/0, kDNSServiceInterfaceIndexAny,
      advertisement.instanceName.c_str(), advertisement.serviceType.c_str(),
      /*domain=*/nullptr, /*host=*/nullptr,
      // Network byte order, unlike every other port in this codebase. Passing
      // it host-ordered registers a plausible-looking service on the wrong
      // port, which resolves and then refuses every connection.
      htons(advertisement.port), static_cast<uint16_t>(txt.size()), txt.data(),
      &Impl::registerReply, impl_.get());

  if (status != kDNSServiceErr_NoError) {
    error = "DNSServiceRegister failed (" + std::to_string(status) + ")";
    impl_->service = nullptr;
    return false;
  }

  impl_->running.store(true);
  impl_->poller = std::thread([impl = impl_.get()] {
    const int socket = DNSServiceRefSockFD(impl->service);
    if (socket < 0) {
      diag::warn("mdns: no socket for the registration; replies will be missed");
      return;
    }
    while (impl->running.load()) {
      fd_set readable;
      FD_ZERO(&readable);
      FD_SET(socket, &readable);
      timeval timeout{};
      timeout.tv_sec = 0;
      timeout.tv_usec = kPollTimeoutMs * 1000;

      const int ready = ::select(socket + 1, &readable, nullptr, nullptr, &timeout);
      if (ready < 0) {
        if (errno == EINTR) {
          continue;
        }
        diag::warn("mdns: poll failed (%d); stopping the advertisement", errno);
        return;
      }
      if (ready > 0 && FD_ISSET(socket, &readable)) {
        const DNSServiceErrorType result = DNSServiceProcessResult(impl->service);
        if (result != kDNSServiceErr_NoError) {
          diag::warn("mdns: responder connection lost (%d)",
                     static_cast<int>(result));
          return;
        }
      }
    }
  });

  return true;
}

void Responder::stop() {
  if (!impl_->running.exchange(false)) {
    return;
  }
  if (impl_->poller.joinable()) {
    // Joined before deallocating: DNSServiceRefDeallocate must not run while
    // another thread is inside DNSServiceProcessResult on the same ref.
    impl_->poller.join();
  }
  if (impl_->service != nullptr) {
    // Sends the goodbye packet, which is what stops every browser on the
    // subnet from showing this instance for the next couple of minutes.
    DNSServiceRefDeallocate(impl_->service);
    impl_->service = nullptr;
  }
  std::lock_guard<std::mutex> lock(impl_->mutex);
  impl_->registeredName.clear();
}

bool Responder::isRunning() const { return impl_->running.load(); }

std::string Responder::registeredName() const {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  return impl_->registeredName;
}

}  // namespace mdns
}  // namespace weblinked
