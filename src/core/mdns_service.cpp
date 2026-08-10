#include "core/mdns_service.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#else
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <SystemConfiguration/SystemConfiguration.h>
#endif

namespace weblinked {
namespace mdns {
namespace {

/// The longest a DNS label may be, which is what bounds an instance name.
constexpr size_t kMaxNameBytes = 63;
/// A single TXT string carries its length in one byte.
constexpr size_t kMaxTxtEntryBytes = 255;

/// Backs up off a UTF-8 continuation byte, so a truncated name is still a
/// valid string rather than a sequence ending mid-character. A name that ends
/// in half a character renders as a replacement glyph in a fleet list, which
/// looks like corruption rather than truncation.
size_t utf8SafeLength(const std::string& text, size_t limit) {
  if (text.size() <= limit) {
    return text.size();
  }
  size_t length = limit;
  while (length > 0 &&
         (static_cast<unsigned char>(text[length]) & 0xC0) == 0x80) {
    --length;
  }
  return length;
}

}  // namespace

std::string sanitiseInstanceName(const std::string& raw) {
  std::string cleaned;
  cleaned.reserve(raw.size());
  for (const char character : raw) {
    const unsigned char byte = static_cast<unsigned char>(character);
    // A dot would split the name and put the service in a subdomain nothing
    // browses, so it becomes a dash rather than being dropped — an operator
    // who typed "Stage 1.2" should still recognise the result.
    if (character == '.') {
      cleaned.push_back('-');
      continue;
    }
    // Control characters, including the tabs and newlines that arrive when a
    // name is pasted out of a spreadsheet.
    if (byte < 0x20 || byte == 0x7F) {
      continue;
    }
    cleaned.push_back(character);
  }

  const auto notSpace = [](unsigned char c) { return std::isspace(c) == 0; };
  cleaned.erase(cleaned.begin(),
                std::find_if(cleaned.begin(), cleaned.end(), notSpace));
  cleaned.erase(std::find_if(cleaned.rbegin(), cleaned.rend(), notSpace).base(),
                cleaned.end());

  cleaned.resize(utf8SafeLength(cleaned, kMaxNameBytes));
  if (cleaned.empty()) {
    return "WebLinked";
  }
  return cleaned;
}

std::string instanceNameFor(const std::string& configured, const std::string& host,
                            uint16_t httpPort) {
  if (!configured.empty()) {
    return sanitiseInstanceName(configured);
  }
  std::string host_part = host.empty() ? std::string("WebLinked") : host;
  return sanitiseInstanceName(host_part + " (" + std::to_string(httpPort) + ")");
}

std::string instanceId(const std::string& host, uint16_t httpPort) {
  // FNV-1a, 32-bit. Not a security property — it only has to be stable across
  // restarts and different between two instances on one host.
  uint32_t hash = 2166136261u;
  const std::string material = host + ":" + std::to_string(httpPort);
  for (const char character : material) {
    hash ^= static_cast<unsigned char>(character);
    hash *= 16777619u;
  }
  char buffer[9] = {};
  std::snprintf(buffer, sizeof(buffer), "%08x", hash);
  return std::string(buffer);
}

std::string hostName() {
#if defined(__APPLE__)
  // Not gethostname(), which on a stock Mac answers "Mac" — every machine in a
  // rack of them, identically, while Bonjour advertises each host under its
  // *local host name* ("studio-3.local"). Taking the POSIX name would put a
  // list of eight identical "Mac (7654)" rows in front of an operator whose
  // browser is simultaneously showing eight distinct addresses. This is the
  // name the responder will actually resolve to.
  if (CFStringRef local = ::SCDynamicStoreCopyLocalHostName(nullptr);
      local != nullptr) {
    char buffer[256] = {};
    const bool converted =
        ::CFStringGetCString(local, buffer, sizeof(buffer), kCFStringEncodingUTF8);
    ::CFRelease(local);
    if (converted && buffer[0] != '\0') {
      return std::string(buffer);
    }
  }
#endif
  char buffer[256] = {};
  if (::gethostname(buffer, sizeof(buffer) - 1) != 0) {
    return {};
  }
  std::string name(buffer);
  // Some hosts answer fully qualified. Bonjour appends the domain itself, so
  // leaving it on produces "studio.local.local" in a browse list.
  const auto dot = name.find('.');
  if (dot != std::string::npos) {
    name.resize(dot);
  }
  return name;
}

bool bindIsAdvertisable(const std::string& bindAddress, std::string* reason) {
  const auto refuse = [reason](const char* text) {
    if (reason != nullptr) {
      *reason = text;
    }
    return false;
  };

  if (bindAddress == "localhost" || bindAddress == "::1" ||
      bindAddress.rfind("127.", 0) == 0) {
    return refuse(
        "the control API is bound to loopback, so nothing off this machine "
        "could reach the advertised address — start with --bind 0.0.0.0 to be "
        "discoverable");
  }
  if (reason != nullptr) {
    reason->clear();
  }
  return true;
}

TxtRecords buildTxt(const TxtInputs& inputs) {
  TxtRecords records;
  // RFC 6763 §6.7: a version tag, first, so a consumer can refuse a record it
  // does not understand instead of misreading it.
  records.emplace_back("txtvers", "1");
  records.emplace_back("ver", inputs.version);
  records.emplace_back("name", inputs.name);
  records.emplace_back("id", inputs.id);
  // Where a consumer should look to confirm this really is a WebLinked. It is
  // the endpoint rookery's subnet sweep already probes for `compiled_backends`,
  // named here so a future consumer does not have to hard-code it.
  records.emplace_back("path", "/api/state");
  // Announced so a controller can ask for the token while the operator is
  // still looking at the add dialog, rather than adding the instance and
  // showing a failed row a second later.
  records.emplace_back("token", inputs.tokenRequired ? "1" : "0");
  records.emplace_back("osc", inputs.oscEnabled ? "1" : "0");
  if (inputs.oscEnabled) {
    records.emplace_back("oscport", std::to_string(inputs.oscPort));
    records.emplace_back("oscprefix", inputs.oscPrefix);
  }
  return records;
}

std::vector<uint8_t> encodeTxt(const TxtRecords& records) {
  std::vector<uint8_t> encoded;
  for (const auto& entry : records) {
    if (entry.first.empty() ||
        entry.first.find('=') != std::string::npos) {
      continue;
    }
    const std::string text = entry.first + "=" + entry.second;
    // Dropped whole rather than truncated: a consumer cannot tell a cut value
    // from a real one, and a half-written OSC port is worse than an absent one
    // because it looks usable.
    if (text.size() > kMaxTxtEntryBytes) {
      continue;
    }
    encoded.push_back(static_cast<uint8_t>(text.size()));
    encoded.insert(encoded.end(), text.begin(), text.end());
  }
  // A zero-length TXT record is illegal; the empty record is one zero byte.
  if (encoded.empty()) {
    encoded.push_back(0);
  }
  return encoded;
}

}  // namespace mdns
}  // namespace weblinked
