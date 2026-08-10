#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace weblinked {
namespace mdns {

/// TXT record entries, in the order they will appear on the wire.
using TxtRecords = std::vector<std::pair<std::string, std::string>>;

/// The service type every consumer browses for.
///
/// Ours rather than `_http._tcp`: a browser finding an HTTP service learns
/// nothing about whether it speaks WebLinked's API, and a show network with
/// forty web servers on it would drown the list. Advertising both was
/// considered and rejected — the second record is pollution that no consumer
/// in this fleet would read.
inline constexpr const char* kServiceType = "_weblinked._tcp";

/// What to advertise. Everything here is settled at start-up.
struct Advertisement {
  std::string instanceName;
  std::string serviceType = kServiceType;
  uint16_t port = 7654;
  TxtRecords txt;
};

/// The inputs the TXT set is built from.
///
/// Note what is *not* here: source count, current URL, frame counters, output
/// health. TXT carries identity and how to reach the thing; live state stays on
/// HTTP, where every consumer in this fleet is already polling. Putting state
/// in TXT means rewriting the record at poll rate, and every rewrite is a
/// multicast the whole subnet pays for.
struct TxtInputs {
  std::string name;        ///< human-facing instance name
  std::string version;     ///< WEBLINKED_VERSION
  std::string id;          ///< stable across restarts, unique on the host
  uint16_t httpPort = 7654;
  bool tokenRequired = false;
  bool oscEnabled = true;
  uint16_t oscPort = 7655;
  std::string oscPrefix = "/weblinked";
};

/// Builds the TXT set.
///
/// `oscport` and `oscprefix` are the reason this is worth doing at all: nothing
/// in `/api/state` reports either, so a fleet controller that finds an instance
/// over HTTP still has to *assume* 7655 and the default prefix. rookery shipped
/// with exactly that assumption, shown to the operator as an assumption. A
/// custom prefix silently breaks every address sent to it.
TxtRecords buildTxt(const TxtInputs& inputs);

/// Encodes TXT records into the DNS-SD wire format: each `key=value` string
/// preceded by a one-byte length (RFC 6763 §6.1).
///
/// An empty set encodes as a single zero byte, not as nothing — a TXT record of
/// zero length is illegal, and some responders reject the registration rather
/// than fixing it up for you. Entries whose encoded form would exceed 255 bytes
/// are dropped rather than truncated: a half a value is worse than no value,
/// because a consumer cannot tell it was cut.
std::vector<uint8_t> encodeTxt(const TxtRecords& records);

/// The host's short name, without a domain and without a trailing dot.
std::string hostName();

/// A stable identifier for this instance.
///
/// Derived from the host name and control port rather than stored, so it
/// survives a restart, a settings wipe and a copied config directory, and still
/// differs between two instances on one host — which is a supported
/// arrangement here, since the Chromium profile directory is keyed on the
/// control port too.
std::string instanceId(const std::string& host, uint16_t httpPort);

/// The advertised instance name: what an operator sees in a browse list.
///
/// `configured` is `--name` when given. Otherwise the name is built from the
/// host and the control port — the port included *always*, not only on
/// collision, because two instances on one host is normal here and a name that
/// only becomes unique under conflict resolution produces "WebLinked (2)",
/// which tells an operator nothing about which one it is.
std::string instanceNameFor(const std::string& configured, const std::string& host,
                            uint16_t httpPort);

/// Strips what a DNS-SD instance name cannot carry.
///
/// Instance names are UTF-8 and may contain spaces, but a dot would split the
/// name and land the service in a subdomain that nothing browses. Length is
/// capped at the 63 bytes a DNS label allows, on a UTF-8 boundary so the result
/// is still a valid string.
std::string sanitiseInstanceName(const std::string& raw);

/// Whether an advertisement for `bindAddress` would be honest.
///
/// The trap this exists for: `httpBind` defaults to `127.0.0.1`, so the common
/// case is an instance whose control API cannot be reached from another
/// machine at all. Advertising it anyway publishes a record that resolves to
/// the browsing machine's own loopback — the service appears in every fleet
/// list on the subnet and every one of them fails to connect, with nothing
/// anywhere saying why. Refusing, and logging the reason, turns a mystery into
/// one line in the log.
bool bindIsAdvertisable(const std::string& bindAddress, std::string* reason);

/// A live registration. Destroying it withdraws the service.
///
/// One per process, not one per source: a WebLinked with eight sources still
/// has exactly one control API on one port, and eight records pointing at the
/// same socket would be eight rows in a fleet list for one machine.
class Responder {
 public:
  Responder();
  ~Responder();

  Responder(const Responder&) = delete;
  Responder& operator=(const Responder&) = delete;

  /// Registers the service. Returns false and sets `error` if the platform has
  /// no responder to register with, which is not fatal to anything: the
  /// instance runs exactly as before, and a fleet controller falls back to
  /// sweeping the subnet for it.
  bool start(const Advertisement& advertisement, std::string& error);

  /// Withdraws the service and waits for the responder to acknowledge.
  ///
  /// Called on the orderly shutdown path for the same reason the NDI senders
  /// are: a record that outlives its process leaves every consumer on the
  /// network retrying an address that answers nothing, and the *next* run can
  /// then be undiscoverable while the stale entry ages out of the caches.
  void stop();

  bool isRunning() const;

  /// The name the responder settled on, which is not necessarily the one asked
  /// for — a conflict on the network gets renamed under us, and the renamed
  /// value is what consumers will show. Empty until registration is confirmed.
  std::string registeredName() const;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace mdns
}  // namespace weblinked
