// mdns_probe — browses for WebLinked advertisements and checks they are true.
//
// A verification tool, not part of the application, and deliberately not built
// from any of WebLinked's own code: the TXT record is decoded with the
// platform's parser, so an encoder bug cannot cancel itself out the way it
// would if both ends shared a helper. That is the same rule tools/ndi_probe
// follows, and the reason rookery's over-the-wire OSC test is driven by an
// independent Python encoder.
//
// `dns-sd -B` and `avahi-browse` already show that a record exists. What
// neither does — and what actually matters — is connect to the address the
// record publishes and confirm something answers there. An advertisement is
// only useful if it is honest, and the interesting failure is precisely the
// one that browses perfectly: an instance bound to loopback publishing a host
// name that resolves, on the browsing machine, to the browsing machine.
//
// Build (macOS; dns_sd is in libSystem, so there is nothing to link):
//   clang++ -std=c++20 tools/mdns_probe.cpp -o mdns_probe
//
// On Linux the equivalent browse is `avahi-browse -r _weblinked._tcp`; the
// reachability check below is then `curl -sS http://<host>:<port>/api/state`.
//
// Usage:
//   mdns_probe [--timeout <seconds>] [--no-connect]

#include <arpa/inet.h>
#include <dns_sd.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace {

struct Found {
  std::string name;
  std::string host;
  uint16_t port = 0;
  std::string txt;
  bool resolved = false;
};

/// A deque, not a vector: a reference to an element is handed to the resolver
/// callback, and a vector's reallocation on the next push would leave that
/// reference dangling.
std::deque<Found> g_found;

/// Runs a DNSServiceRef until it stops producing results or the deadline
/// passes.
void pump(DNSServiceRef service, double seconds) {
  const int socket = DNSServiceRefSockFD(service);
  if (socket < 0) {
    return;
  }
  const double step = 0.25;
  for (double waited = 0; waited < seconds; waited += step) {
    fd_set readable;
    FD_ZERO(&readable);
    FD_SET(socket, &readable);
    timeval timeout{};
    timeout.tv_sec = 0;
    timeout.tv_usec = static_cast<int>(step * 1000000);
    if (::select(socket + 1, &readable, nullptr, nullptr, &timeout) > 0) {
      if (DNSServiceProcessResult(service) != kDNSServiceErr_NoError) {
        return;
      }
    }
  }
}

void DNSSD_API resolveReply(DNSServiceRef /*service*/, DNSServiceFlags /*flags*/,
                            uint32_t /*interfaceIndex*/, DNSServiceErrorType error,
                            const char* fullname, const char* hosttarget, uint16_t port,
                            uint16_t txtLen, const unsigned char* txtRecord,
                            void* context) {
  auto* found = static_cast<Found*>(context);
  if (error != kDNSServiceErr_NoError) {
    return;
  }
  found->resolved = true;
  found->host = hosttarget != nullptr ? hosttarget : "";
  found->port = ntohs(port);
  (void)fullname;

  // Decoded with Apple's parser rather than by hand: this is the half of the
  // check that has to be foreign to the code under test.
  std::string rendered;
  const uint16_t count = TXTRecordGetCount(txtLen, txtRecord);
  for (uint16_t index = 0; index < count; ++index) {
    char key[256] = {};
    uint8_t valueLen = 0;
    const void* value = nullptr;
    if (TXTRecordGetItemAtIndex(txtLen, txtRecord, index, sizeof(key), key, &valueLen,
                                &value) != kDNSServiceErr_NoError) {
      continue;
    }
    if (!rendered.empty()) {
      rendered += " ";
    }
    rendered += key;
    rendered += "=";
    rendered.append(static_cast<const char*>(value), valueLen);
  }
  found->txt = rendered;
}

void DNSSD_API browseReply(DNSServiceRef /*service*/, DNSServiceFlags flags,
                           uint32_t interfaceIndex, DNSServiceErrorType error,
                           const char* serviceName, const char* regtype,
                           const char* replyDomain, void* /*context*/) {
  if (error != kDNSServiceErr_NoError || (flags & kDNSServiceFlagsAdd) == 0) {
    return;
  }
  // The same instance is announced once per interface. Collapsing on the name
  // keeps a machine with Wi-Fi and Ethernet from looking like two machines,
  // which is exactly how it looks in `dns-sd -B` output.
  for (const auto& existing : g_found) {
    if (existing.name == serviceName) {
      return;
    }
  }

  Found found;
  found.name = serviceName != nullptr ? serviceName : "";
  g_found.push_back(found);

  DNSServiceRef resolver = nullptr;
  if (DNSServiceResolve(&resolver, 0, interfaceIndex, serviceName, regtype, replyDomain,
                        &resolveReply, &g_found.back()) == kDNSServiceErr_NoError) {
    pump(resolver, 2.0);
    DNSServiceRefDeallocate(resolver);
  }
}

/// Connects to the advertised address and asks for /api/state.
///
/// A 200 is a WebLinked answering. A 401 is a WebLinked answering that wants a
/// token — just as identifying, and it must not be treated as a failure.
std::string checkReachable(const std::string& host, uint16_t port) {
  std::string target = host;
  if (!target.empty() && target.back() == '.') {
    target.pop_back();
  }

  addrinfo hints{};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  addrinfo* results = nullptr;
  const std::string service = std::to_string(port);
  if (::getaddrinfo(target.c_str(), service.c_str(), &hints, &results) != 0 ||
      results == nullptr) {
    return "the advertised host does not resolve";
  }

  int handle = -1;
  for (addrinfo* entry = results; entry != nullptr; entry = entry->ai_next) {
    handle = ::socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (handle < 0) {
      continue;
    }
    if (::connect(handle, entry->ai_addr, entry->ai_addrlen) == 0) {
      break;
    }
    ::close(handle);
    handle = -1;
  }
  ::freeaddrinfo(results);

  if (handle < 0) {
    return "NOTHING ANSWERS at the advertised address";
  }

  const std::string request = "GET /api/state HTTP/1.1\r\nHost: " + target +
                              "\r\nConnection: close\r\n\r\n";
  ::send(handle, request.data(), request.size(), 0);

  char buffer[512] = {};
  const ssize_t received = ::recv(handle, buffer, sizeof(buffer) - 1, 0);
  ::close(handle);
  if (received <= 0) {
    return "connected, but the control API said nothing";
  }

  const std::string reply(buffer, static_cast<size_t>(received));
  if (reply.find("200") != std::string::npos &&
      reply.find("compiled_backends") != std::string::npos) {
    return "OK — answered /api/state";
  }
  if (reply.find("401") != std::string::npos) {
    return "OK — answered 401, so it wants its token (TXT should say token=1)";
  }
  return "answered, but not like a WebLinked: " + reply.substr(0, 40);
}

}  // namespace

int main(int argc, char** argv) {
  double timeout = 5.0;
  bool connectCheck = true;
  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if (argument == "--timeout" && index + 1 < argc) {
      timeout = std::atof(argv[++index]);
    } else if (argument == "--no-connect") {
      connectCheck = false;
    } else {
      std::printf("usage: mdns_probe [--timeout <seconds>] [--no-connect]\n");
      return 1;
    }
  }

  std::printf("browsing for _weblinked._tcp for %.1f s...\n\n", timeout);

  DNSServiceRef browser = nullptr;
  if (DNSServiceBrowse(&browser, 0, kDNSServiceInterfaceIndexAny, "_weblinked._tcp",
                       nullptr, &browseReply, nullptr) != kDNSServiceErr_NoError) {
    std::fprintf(stderr, "could not start a browse\n");
    return 1;
  }
  pump(browser, timeout);
  DNSServiceRefDeallocate(browser);

  if (g_found.empty()) {
    std::printf("no WebLinked instances found.\n\n");
    std::printf(
        "If one is running, the usual causes are: it is bound to loopback (it\n"
        "refuses to advertise, and says so in its log), it was started with\n"
        "--no-mdns, or this machine is on a different subnet — mDNS does not\n"
        "cross a router.\n");
    return 1;
  }

  int unreachable = 0;
  for (const auto& found : g_found) {
    std::printf("%s\n", found.name.c_str());
    if (!found.resolved) {
      std::printf("  did not resolve\n\n");
      ++unreachable;
      continue;
    }
    std::printf("  address : %s:%u\n", found.host.c_str(), found.port);
    std::printf("  txt     : %s\n", found.txt.c_str());
    if (connectCheck) {
      const std::string verdict = checkReachable(found.host, found.port);
      std::printf("  reached : %s\n", verdict.c_str());
      if (verdict.rfind("OK", 0) != 0) {
        ++unreachable;
      }
    }
    std::printf("\n");
  }

  std::printf("%zu instance%s, %d unreachable\n", g_found.size(),
              g_found.size() == 1 ? "" : "s", unreachable);
  return unreachable == 0 ? 0 : 1;
}
