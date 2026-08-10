// Windows mDNS registration, through the responder built into the OS.
//
// `DnsServiceRegister` arrived in Windows 10 1703 and registers with the
// responder Windows already runs, so — as on macOS — this adds a record to
// something live rather than putting a second thing on UDP 5353. It is
// resolved with GetProcAddress rather than linked, because a hard import makes
// the whole executable fail to load on a Windows without it, with a loader
// error that names dnsapi.dll and not the feature.
//
// Note that Apple's Bonjour service, if the machine has it (iTunes installs
// it), is a *separate* responder that also serves _tcp browses. Registering
// here does not involve it and does not conflict with it.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <windns.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>

#include "core/dylib.h"
#include "core/mdns_service.h"
#include "diag/diag.h"

namespace weblinked {
namespace mdns {
namespace {

/// How long shutdown waits for the responder to confirm the withdrawal before
/// giving up. Bounded because this runs on the way out of a live show machine:
/// a stale record for a couple of minutes is bad, a process that will not exit
/// is worse.
constexpr auto kDeregisterTimeout = std::chrono::milliseconds(2000);

std::wstring widen(const std::string& text) {
  if (text.empty()) {
    return {};
  }
  const int length = ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(),
                                           static_cast<int>(text.size()), nullptr, 0);
  if (length <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(length), L'\0');
  ::MultiByteToWideChar(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                        wide.data(), length);
  return wide;
}

/// The dnsapi entry points, which exist only on Windows 10 1703 and later.
struct DnsApi {
  PDNS_SERVICE_INSTANCE(WINAPI* constructInstance)
  (PCWSTR, PCWSTR, PIP4_ADDRESS, PIP6_ADDRESS, WORD, WORD, WORD, DWORD, PCWSTR*,
   PCWSTR*) = nullptr;
  DWORD(WINAPI* registerService)
  (PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL) = nullptr;
  DWORD(WINAPI* deregisterService)
  (PDNS_SERVICE_REGISTER_REQUEST, PDNS_SERVICE_CANCEL) = nullptr;
  void(WINAPI* freeInstance)(PDNS_SERVICE_INSTANCE) = nullptr;

  Dylib library;

  bool load() {
    if (!library.open({"dnsapi.dll"})) {
      return false;
    }
    const bool complete = library.symbol("DnsServiceConstructInstance", constructInstance) &&
                          library.symbol("DnsServiceRegister", registerService) &&
                          library.symbol("DnsServiceDeRegister", deregisterService) &&
                          library.symbol("DnsServiceFreeInstance", freeInstance);
    if (!complete) {
      library.close();
      return false;
    }
    return true;
  }
};

}  // namespace

struct Responder::Impl {
  DnsApi api;
  PDNS_SERVICE_INSTANCE instance = nullptr;
  DNS_SERVICE_REGISTER_REQUEST request{};
  std::wstring serviceName;
  std::wstring hostName;
  std::atomic<bool> running{false};

  std::mutex mutex;
  std::condition_variable settled;
  bool completed = false;
  std::string registeredName;

  static void WINAPI registerComplete(DWORD status, void* context,
                                      PDNS_SERVICE_INSTANCE instance);
};

void WINAPI Responder::Impl::registerComplete(DWORD status, void* context,
                                              PDNS_SERVICE_INSTANCE instance) {
  auto* impl = static_cast<Impl*>(context);
  {
    std::lock_guard<std::mutex> lock(impl->mutex);
    impl->completed = true;
    if (status == ERROR_SUCCESS && instance != nullptr &&
        instance->pszInstanceName != nullptr) {
      char narrow[512] = {};
      const int written = ::WideCharToMultiByte(CP_UTF8, 0, instance->pszInstanceName,
                                                -1, narrow, sizeof(narrow) - 1, nullptr,
                                                nullptr);
      if (written > 0) {
        impl->registeredName = narrow;
      }
    }
  }
  impl->settled.notify_all();

  if (status != ERROR_SUCCESS) {
    diag::warn("mdns: registration failed (%lu)", static_cast<unsigned long>(status));
  } else {
    diag::info("mdns: advertising '%s'", impl->registeredName.c_str());
  }

  // The instance handed to the callback is the responder's copy, and freeing
  // it is the caller's job. Ours — the one built by constructInstance — is
  // freed in stop(); this one is only freed when it is a different pointer.
  if (instance != nullptr && instance != impl->instance) {
    impl->api.freeInstance(instance);
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
        "the DNS-SD API is not available on this Windows "
        "(needs Windows 10 1703 or later)";
    return false;
  }

  // Windows wants the fully qualified instance name, not the three parts.
  impl_->serviceName =
      widen(advertisement.instanceName + "." + advertisement.serviceType + ".local");
  impl_->hostName = widen(hostName() + ".local");

  std::vector<std::wstring> keyStorage;
  std::vector<std::wstring> valueStorage;
  keyStorage.reserve(advertisement.txt.size());
  valueStorage.reserve(advertisement.txt.size());
  for (const auto& entry : advertisement.txt) {
    keyStorage.push_back(widen(entry.first));
    valueStorage.push_back(widen(entry.second));
  }
  std::vector<PCWSTR> keys;
  std::vector<PCWSTR> values;
  keys.reserve(keyStorage.size());
  values.reserve(valueStorage.size());
  for (size_t index = 0; index < keyStorage.size(); ++index) {
    keys.push_back(keyStorage[index].c_str());
    values.push_back(valueStorage[index].c_str());
  }

  impl_->instance = impl_->api.constructInstance(
      impl_->serviceName.c_str(), impl_->hostName.c_str(), /*pIp4=*/nullptr,
      /*pIp6=*/nullptr,
      // Host byte order here, unlike the macOS backend — dnsapi takes a WORD
      // and does the ordering itself.
      advertisement.port, /*wPriority=*/0, /*wWeight=*/0,
      static_cast<DWORD>(keys.size()), keys.empty() ? nullptr : keys.data(),
      values.empty() ? nullptr : values.data());

  if (impl_->instance == nullptr) {
    error = "could not construct the service instance";
    return false;
  }

  impl_->request = {};
  impl_->request.Version = DNS_QUERY_REQUEST_VERSION1;
  impl_->request.InterfaceIndex = 0;  // every interface
  impl_->request.pServiceInstance = impl_->instance;
  impl_->request.pRegisterCompletionCallback = &Impl::registerComplete;
  impl_->request.pQueryContext = impl_.get();
  impl_->request.unicastEnabled = FALSE;

  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->completed = false;
  }

  const DWORD status = impl_->api.registerService(&impl_->request, nullptr);
  // The call is asynchronous: pending is the success path, and the callback
  // reports the real outcome.
  if (status != ERROR_SUCCESS && status != DNS_REQUEST_PENDING) {
    error = "DnsServiceRegister failed (" + std::to_string(status) + ")";
    impl_->api.freeInstance(impl_->instance);
    impl_->instance = nullptr;
    return false;
  }

  impl_->running.store(true);
  return true;
}

void Responder::stop() {
  if (!impl_->running.exchange(false)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->completed = false;
  }

  // Sends the goodbye, so a fleet list drops this instance now rather than
  // when the record ages out of every cache on the subnet.
  const DWORD status = impl_->api.deregisterService(&impl_->request, nullptr);
  if (status == DNS_REQUEST_PENDING) {
    std::unique_lock<std::mutex> lock(impl_->mutex);
    impl_->settled.wait_for(lock, kDeregisterTimeout,
                            [this] { return impl_->completed; });
  }

  if (impl_->instance != nullptr) {
    impl_->api.freeInstance(impl_->instance);
    impl_->instance = nullptr;
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
