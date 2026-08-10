// Linux mDNS registration, through avahi-daemon.
//
// Two decisions worth stating, because both look like extra work until they
// are not:
//
// **Built against avahi's real headers, resolved at run time.** The types and
// enum values come from `<avahi-client/*.h>` so the compiler checks every
// signature, but the library itself is opened with `Dylib` exactly as libndi
// and libomt are. Linking it properly would mean a WebLinked that refuses to
// start at all on a box without libavahi-client3 — which is most containers,
// and plenty of stripped-down show machines — to gain a feature whose entire
// failure mode is "a fleet controller has to sweep for you instead". See the
// note at the top of core/dylib.h; this is the same trade for the same reason.
//
// **No avahi at build time is not an error.** The whole backend compiles out
// to a stub that reports why, so the Linux build does not acquire a new build
// dependency that a contributor has to discover from a link error.

#include <string>

#include "core/mdns_service.h"
#include "diag/diag.h"

#if defined(WEBLINKED_WITH_AVAHI)

#include <avahi-client/client.h>
#include <avahi-client/publish.h>
#include <avahi-common/alternative.h>
#include <avahi-common/error.h>
#include <avahi-common/malloc.h>
#include <avahi-common/strlst.h>
#include <avahi-common/thread-watch.h>

#include <atomic>
#include <mutex>
#include <vector>

#include "core/dylib.h"

namespace weblinked {
namespace mdns {
namespace {

/// Everything reached in libavahi-client. Grouped so a missing symbol is one
/// failed load rather than a crash on first use.
struct AvahiApi {
  AvahiThreadedPoll* (*threaded_poll_new)() = nullptr;
  const AvahiPoll* (*threaded_poll_get)(AvahiThreadedPoll*) = nullptr;
  int (*threaded_poll_start)(AvahiThreadedPoll*) = nullptr;
  int (*threaded_poll_stop)(AvahiThreadedPoll*) = nullptr;
  void (*threaded_poll_free)(AvahiThreadedPoll*) = nullptr;
  void (*threaded_poll_lock)(AvahiThreadedPoll*) = nullptr;
  void (*threaded_poll_unlock)(AvahiThreadedPoll*) = nullptr;

  AvahiClient* (*client_new)(const AvahiPoll*, AvahiClientFlags, AvahiClientCallback,
                             void*, int*) = nullptr;
  void (*client_free)(AvahiClient*) = nullptr;
  int (*client_errno)(AvahiClient*) = nullptr;

  AvahiEntryGroup* (*entry_group_new)(AvahiClient*, AvahiEntryGroupCallback,
                                      void*) = nullptr;
  int (*entry_group_add_service_strlst)(AvahiEntryGroup*, AvahiIfIndex, AvahiProtocol,
                                        AvahiPublishFlags, const char*, const char*,
                                        const char*, const char*, uint16_t,
                                        AvahiStringList*) = nullptr;
  int (*entry_group_commit)(AvahiEntryGroup*) = nullptr;
  int (*entry_group_reset)(AvahiEntryGroup*) = nullptr;
  void (*entry_group_free)(AvahiEntryGroup*) = nullptr;

  AvahiStringList* (*string_list_new_from_array)(const char**, int) = nullptr;
  void (*string_list_free)(AvahiStringList*) = nullptr;

  const char* (*strerror)(int) = nullptr;
  char* (*alternative_service_name)(const char*) = nullptr;
  void (*free_)(void*) = nullptr;

  Dylib library;

  bool load() {
    if (!library.open({"libavahi-client.so.3", "libavahi-client.so"})) {
      return false;
    }
    const bool complete =
        library.symbol("avahi_threaded_poll_new", threaded_poll_new) &&
        library.symbol("avahi_threaded_poll_get", threaded_poll_get) &&
        library.symbol("avahi_threaded_poll_start", threaded_poll_start) &&
        library.symbol("avahi_threaded_poll_stop", threaded_poll_stop) &&
        library.symbol("avahi_threaded_poll_free", threaded_poll_free) &&
        library.symbol("avahi_threaded_poll_lock", threaded_poll_lock) &&
        library.symbol("avahi_threaded_poll_unlock", threaded_poll_unlock) &&
        library.symbol("avahi_client_new", client_new) &&
        library.symbol("avahi_client_free", client_free) &&
        library.symbol("avahi_client_errno", client_errno) &&
        library.symbol("avahi_entry_group_new", entry_group_new) &&
        library.symbol("avahi_entry_group_add_service_strlst",
                       entry_group_add_service_strlst) &&
        library.symbol("avahi_entry_group_commit", entry_group_commit) &&
        library.symbol("avahi_entry_group_reset", entry_group_reset) &&
        library.symbol("avahi_entry_group_free", entry_group_free) &&
        library.symbol("avahi_string_list_new_from_array", string_list_new_from_array) &&
        library.symbol("avahi_string_list_free", string_list_free) &&
        library.symbol("avahi_strerror", strerror) &&
        library.symbol("avahi_alternative_service_name", alternative_service_name) &&
        library.symbol("avahi_free", free_);
    if (!complete) {
      library.close();
      return false;
    }
    return true;
  }
};

}  // namespace

struct Responder::Impl {
  AvahiApi api;
  AvahiThreadedPoll* poll = nullptr;
  AvahiClient* client = nullptr;
  AvahiEntryGroup* group = nullptr;

  Advertisement advertisement;
  /// Mutated on collision, which is why it is not just `advertisement.name`.
  std::string currentName;
  std::atomic<bool> running{false};

  mutable std::mutex mutex;
  std::string registeredName;

  /// Publishes the service into `group`. Must hold avahi's poll lock, which is
  /// true of both callers: the client callback runs on the poll thread, and
  /// start() takes the lock explicitly.
  void publish();

  static void clientCallback(AvahiClient* client, AvahiClientState state, void* userdata);
  static void groupCallback(AvahiEntryGroup* group, AvahiEntryGroupState state,
                            void* userdata);
};

void Responder::Impl::publish() {
  if (group == nullptr) {
    group = api.entry_group_new(client, &Impl::groupCallback, this);
    if (group == nullptr) {
      diag::warn("mdns: could not create an entry group: %s",
                 api.strerror(api.client_errno(client)));
      return;
    }
  }

  // avahi takes "key=value" strings, so the shared encoder's length-prefixed
  // form is not what it wants — the pairs are joined here instead. Same
  // records, different packaging.
  std::vector<std::string> pairs;
  pairs.reserve(advertisement.txt.size());
  for (const auto& entry : advertisement.txt) {
    pairs.push_back(entry.first + "=" + entry.second);
  }
  std::vector<const char*> raw;
  raw.reserve(pairs.size());
  for (const auto& pair : pairs) {
    raw.push_back(pair.c_str());
  }

  AvahiStringList* txt =
      api.string_list_new_from_array(raw.data(), static_cast<int>(raw.size()));

  const int added = api.entry_group_add_service_strlst(
      group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, static_cast<AvahiPublishFlags>(0),
      currentName.c_str(), advertisement.serviceType.c_str(), /*domain=*/nullptr,
      /*host=*/nullptr, advertisement.port, txt);

  if (txt != nullptr) {
    api.string_list_free(txt);
  }

  if (added < 0) {
    if (added == AVAHI_ERR_COLLISION) {
      // Another service already holds the name. Take the alternative and try
      // again from a clean group.
      char* alternative = api.alternative_service_name(currentName.c_str());
      if (alternative != nullptr) {
        currentName = alternative;
        api.free_(alternative);
        api.entry_group_reset(group);
        publish();
        return;
      }
    }
    diag::warn("mdns: could not add the service: %s", api.strerror(added));
    return;
  }

  const int committed = api.entry_group_commit(group);
  if (committed < 0) {
    diag::warn("mdns: could not commit the service: %s", api.strerror(committed));
  }
}

void Responder::Impl::clientCallback(AvahiClient* client, AvahiClientState state,
                                     void* userdata) {
  auto* impl = static_cast<Impl*>(userdata);
  impl->client = client;
  switch (state) {
    case AVAHI_CLIENT_S_RUNNING:
      // The daemon is up — which may be now, or may be minutes from now if
      // avahi-daemon starts after us. Publishing from here rather than from
      // start() is what makes the later case work instead of failing once and
      // staying failed.
      impl->publish();
      break;
    case AVAHI_CLIENT_S_COLLISION:
    case AVAHI_CLIENT_S_REGISTERING:
      // The host name is being renegotiated; the group must be torn down and
      // rebuilt once that settles, or it publishes under a name that is about
      // to stop existing.
      if (impl->group != nullptr) {
        impl->api.entry_group_reset(impl->group);
      }
      break;
    case AVAHI_CLIENT_FAILURE:
      diag::warn("mdns: avahi client failed: %s",
                 impl->api.strerror(impl->api.client_errno(client)));
      break;
    case AVAHI_CLIENT_CONNECTING:
      // avahi-daemon is not running yet. AVAHI_CLIENT_NO_FAIL means we wait
      // rather than error out, so nothing to do.
      break;
  }
}

void Responder::Impl::groupCallback(AvahiEntryGroup* group, AvahiEntryGroupState state,
                                    void* userdata) {
  auto* impl = static_cast<Impl*>(userdata);
  switch (state) {
    case AVAHI_ENTRY_GROUP_ESTABLISHED: {
      {
        std::lock_guard<std::mutex> lock(impl->mutex);
        impl->registeredName = impl->currentName;
      }
      diag::info("mdns: advertising '%s' as %s", impl->currentName.c_str(),
                 impl->advertisement.serviceType.c_str());
      break;
    }
    case AVAHI_ENTRY_GROUP_COLLISION: {
      char* alternative = impl->api.alternative_service_name(impl->currentName.c_str());
      if (alternative != nullptr) {
        diag::info("mdns: name '%s' is taken, retrying as '%s'",
                   impl->currentName.c_str(), alternative);
        impl->currentName = alternative;
        impl->api.free_(alternative);
        impl->api.entry_group_reset(group);
        impl->publish();
      }
      break;
    }
    case AVAHI_ENTRY_GROUP_FAILURE:
      diag::warn("mdns: registration failed: %s",
                 impl->api.strerror(impl->api.client_errno(impl->client)));
      break;
    case AVAHI_ENTRY_GROUP_UNCOMMITED:
    case AVAHI_ENTRY_GROUP_REGISTERING:
      break;
  }
}

Responder::Responder() : impl_(std::make_unique<Impl>()) {}

Responder::~Responder() { stop(); }

bool Responder::start(const Advertisement& advertisement, std::string& error) {
  if (impl_->running.load()) {
    error = "already advertising";
    return false;
  }
  if (!impl_->api.load()) {
    error =
        "libavahi-client is not available (" + impl_->api.library.lastError() + ")";
    return false;
  }

  impl_->advertisement = advertisement;
  impl_->currentName = advertisement.instanceName;

  impl_->poll = impl_->api.threaded_poll_new();
  if (impl_->poll == nullptr) {
    error = "could not create the avahi poll";
    return false;
  }

  int avahiError = 0;
  // NO_FAIL: an instance started before avahi-daemon must wait for it rather
  // than give up, because on a rack that boots everything at once the order is
  // not ours to choose.
  impl_->client = impl_->api.client_new(impl_->api.threaded_poll_get(impl_->poll),
                                        AVAHI_CLIENT_NO_FAIL, &Impl::clientCallback,
                                        impl_.get(), &avahiError);
  if (impl_->client == nullptr) {
    error = std::string("could not create the avahi client: ") +
            impl_->api.strerror(avahiError);
    impl_->api.threaded_poll_free(impl_->poll);
    impl_->poll = nullptr;
    return false;
  }

  if (impl_->api.threaded_poll_start(impl_->poll) < 0) {
    error = "could not start the avahi poll thread";
    impl_->api.client_free(impl_->client);
    impl_->client = nullptr;
    impl_->api.threaded_poll_free(impl_->poll);
    impl_->poll = nullptr;
    return false;
  }

  impl_->running.store(true);
  return true;
}

void Responder::stop() {
  if (!impl_->running.exchange(false)) {
    return;
  }
  // Stopped before anything is freed: the poll thread is inside these
  // structures, and freeing them under it is a use-after-free that only shows
  // up when a browse happens to arrive during shutdown.
  impl_->api.threaded_poll_stop(impl_->poll);

  if (impl_->group != nullptr) {
    // Frees the group and sends the withdrawal, so a fleet list stops showing
    // this instance now rather than when the record ages out.
    impl_->api.entry_group_free(impl_->group);
    impl_->group = nullptr;
  }
  if (impl_->client != nullptr) {
    impl_->api.client_free(impl_->client);
    impl_->client = nullptr;
  }
  if (impl_->poll != nullptr) {
    impl_->api.threaded_poll_free(impl_->poll);
    impl_->poll = nullptr;
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

#else  // WEBLINKED_WITH_AVAHI

namespace weblinked {
namespace mdns {

struct Responder::Impl {};

Responder::Responder() : impl_(std::make_unique<Impl>()) {}
Responder::~Responder() = default;

bool Responder::start(const Advertisement& /*advertisement*/, std::string& error) {
  error = "this build has no avahi support (install libavahi-client-dev and rebuild)";
  return false;
}

void Responder::stop() {}
bool Responder::isRunning() const { return false; }
std::string Responder::registeredName() const { return {}; }

}  // namespace mdns
}  // namespace weblinked

#endif  // WEBLINKED_WITH_AVAHI
