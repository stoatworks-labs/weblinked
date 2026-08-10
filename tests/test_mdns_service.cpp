// The mDNS advertisement's string handling.
//
// The registration itself needs a responder and a network, so it is verified
// with `tools/mdns_probe` and with `dns-sd` rather than here. What *is* here is
// everything that decides what goes on the wire — which is where the mistakes
// are, and all of it is reachable without a socket.
//
// The TXT encoding earns its tests the same way the OSC decoder did: it is a
// length-prefixed format, so an off-by-one in a length byte produces a record
// that parses into plausible nonsense rather than failing. A consumer reading
// `oscport` out of a mis-encoded record gets a number, sends OSC to it, and
// nothing happens — with no error anywhere on either machine.

#include <string>
#include <vector>

#include "core/mdns_service.h"
#include "test_support.h"

using namespace weblinked;

namespace {

/// Decodes the length-prefixed form back into pairs, written longhand rather
/// than with any helper the encoder shares, so a mistake cannot cancel itself
/// out. Returns false if any length byte runs past the end — which is the
/// failure being guarded against.
bool decodeTxt(const std::vector<uint8_t>& encoded,
               std::vector<std::pair<std::string, std::string>>& out) {
  size_t offset = 0;
  while (offset < encoded.size()) {
    const size_t length = encoded[offset];
    ++offset;
    if (length == 0) {
      // The empty-record byte. Only legal as the whole record.
      if (encoded.size() != 1) {
        return false;
      }
      return true;
    }
    if (offset + length > encoded.size()) {
      return false;
    }
    const std::string text(reinterpret_cast<const char*>(encoded.data() + offset),
                           length);
    offset += length;
    const auto equals = text.find('=');
    if (equals == std::string::npos) {
      out.emplace_back(text, "");
    } else {
      out.emplace_back(text.substr(0, equals), text.substr(equals + 1));
    }
  }
  return true;
}

std::string valueFor(const std::vector<std::pair<std::string, std::string>>& pairs,
                     const std::string& key) {
  for (const auto& pair : pairs) {
    if (pair.first == key) {
      return pair.second;
    }
  }
  return "<missing>";
}

mdns::TxtInputs sampleInputs() {
  mdns::TxtInputs inputs;
  inputs.name = "Stage Left";
  inputs.version = "0.7.1";
  inputs.id = "deadbeef";
  inputs.httpPort = 7654;
  inputs.tokenRequired = false;
  inputs.oscEnabled = true;
  inputs.oscPort = 7655;
  inputs.oscPrefix = "/weblinked";
  return inputs;
}

}  // namespace

WEBLINKED_TEST(mdns_txt_round_trips_through_the_wire_format) {
  const auto records = mdns::buildTxt(sampleInputs());
  const auto encoded = mdns::encodeTxt(records);

  std::vector<std::pair<std::string, std::string>> decoded;
  CHECK(decodeTxt(encoded, decoded));
  CHECK_EQ(decoded.size(), records.size());
  for (size_t index = 0; index < records.size(); ++index) {
    CHECK_STR(decoded[index].first, records[index].first);
    CHECK_STR(decoded[index].second, records[index].second);
  }
}

WEBLINKED_TEST(mdns_txt_carries_what_http_cannot_report) {
  // The reason the whole feature is worth having: neither of these is in
  // /api/state, so a controller that finds an instance over HTTP alone has to
  // assume them. rookery shipped assuming exactly these two.
  auto inputs = sampleInputs();
  inputs.oscPort = 9001;
  inputs.oscPrefix = "/stage";
  std::vector<std::pair<std::string, std::string>> decoded;
  CHECK(decodeTxt(mdns::encodeTxt(mdns::buildTxt(inputs)), decoded));
  CHECK_STR(valueFor(decoded, "oscport"), "9001");
  CHECK_STR(valueFor(decoded, "oscprefix"), "/stage");
}

WEBLINKED_TEST(mdns_txt_announces_a_token_before_a_client_connects) {
  auto inputs = sampleInputs();
  inputs.tokenRequired = true;
  std::vector<std::pair<std::string, std::string>> decoded;
  CHECK(decodeTxt(mdns::encodeTxt(mdns::buildTxt(inputs)), decoded));
  CHECK_STR(valueFor(decoded, "token"), "1");

  inputs.tokenRequired = false;
  decoded.clear();
  CHECK(decodeTxt(mdns::encodeTxt(mdns::buildTxt(inputs)), decoded));
  CHECK_STR(valueFor(decoded, "token"), "0");
}

WEBLINKED_TEST(mdns_txt_omits_the_osc_port_when_osc_is_off) {
  // Advertising a port nothing listens on is worse than advertising nothing: a
  // controller would show the instance as OSC-capable and every cue sent to it
  // would vanish without an error, because OSC has no replies.
  auto inputs = sampleInputs();
  inputs.oscEnabled = false;
  std::vector<std::pair<std::string, std::string>> decoded;
  CHECK(decodeTxt(mdns::encodeTxt(mdns::buildTxt(inputs)), decoded));
  CHECK_STR(valueFor(decoded, "osc"), "0");
  CHECK_STR(valueFor(decoded, "oscport"), "<missing>");
  CHECK_STR(valueFor(decoded, "oscprefix"), "<missing>");
}

WEBLINKED_TEST(mdns_txt_version_tag_comes_first) {
  // RFC 6763 §6.7 puts txtvers first so a consumer can refuse a record it does
  // not understand rather than misreading it.
  const auto records = mdns::buildTxt(sampleInputs());
  CHECK(!records.empty());
  CHECK_STR(records.front().first, "txtvers");
}

WEBLINKED_TEST(mdns_txt_lengths_are_exact_at_every_residue) {
  // The same sweep the OSC decoder needs, for the same reason: a length-prefix
  // bug that only bites at one length mod something is the kind that ships.
  // Here the prefix is a single byte, so the interesting boundary is 255.
  for (size_t length = 1; length <= 300; ++length) {
    mdns::TxtRecords records;
    records.emplace_back("k", std::string(length, 'x'));
    const auto encoded = mdns::encodeTxt(records);

    std::vector<std::pair<std::string, std::string>> decoded;
    CHECK(decodeTxt(encoded, decoded));
    // "k=" plus the value; anything longer than 255 total is dropped whole.
    if (length + 2 <= 255) {
      CHECK_EQ(decoded.size(), static_cast<size_t>(1));
      CHECK_EQ(decoded[0].second.size(), length);
    } else {
      CHECK(decoded.empty());
    }
  }
}

WEBLINKED_TEST(mdns_txt_never_encodes_a_zero_length_record) {
  // A TXT record of zero length is illegal; the empty record is one zero byte.
  const auto encoded = mdns::encodeTxt({});
  CHECK_EQ(encoded.size(), static_cast<size_t>(1));
  CHECK_EQ(static_cast<int>(encoded[0]), 0);
}

WEBLINKED_TEST(mdns_txt_drops_a_key_containing_an_equals) {
  // The key/value split is the first '=', so a key with one in it would decode
  // as a different key entirely.
  mdns::TxtRecords records;
  records.emplace_back("a=b", "c");
  records.emplace_back("ok", "yes");
  std::vector<std::pair<std::string, std::string>> decoded;
  CHECK(decodeTxt(mdns::encodeTxt(records), decoded));
  CHECK_EQ(decoded.size(), static_cast<size_t>(1));
  CHECK_STR(decoded[0].first, "ok");
}

WEBLINKED_TEST(mdns_instance_name_loses_the_dots) {
  // A dot would split the name and put the service in a subdomain nothing
  // browses — the service would register successfully and be invisible.
  CHECK_STR(mdns::sanitiseInstanceName("Stage 1.2"), "Stage 1-2");
  CHECK_STR(mdns::sanitiseInstanceName("a.b.c"), "a-b-c");
}

WEBLINKED_TEST(mdns_instance_name_survives_operator_input) {
  CHECK_STR(mdns::sanitiseInstanceName("  Front of House  "), "Front of House");
  CHECK_STR(mdns::sanitiseInstanceName("Lobby\tScreen\n"), "LobbyScreen");
  // Never empty: an empty instance name is a registration failure, and the
  // cause ("the name field was blank") would not appear anywhere.
  CHECK_STR(mdns::sanitiseInstanceName(""), "WebLinked");
  CHECK_STR(mdns::sanitiseInstanceName("   "), "WebLinked");
}

WEBLINKED_TEST(mdns_instance_name_truncates_on_a_character_boundary) {
  // A DNS label is 63 bytes. Cutting mid-character would leave a name that
  // renders as a replacement glyph, which reads as corruption rather than as
  // truncation.
  const std::string wide(40, 'e');  // 40 bytes
  const std::string accented = wide + std::string(20, 'x') + "\xC3\xA9";  // é at 61
  const auto result = mdns::sanitiseInstanceName(accented + accented);
  CHECK(result.size() <= 63);
  // Last byte must not be a continuation byte, i.e. the cut landed cleanly.
  CHECK((static_cast<unsigned char>(result.back()) & 0xC0) != 0x80);
}

WEBLINKED_TEST(mdns_default_name_includes_the_port) {
  // Always, not only on collision: two instances on one host is normal here
  // (the Chromium profile directory is keyed on the control port), and a name
  // that only becomes unique under the responder's conflict resolution gives
  // an operator "studio (2)", which identifies nothing.
  CHECK_STR(mdns::instanceNameFor("", "studio", 7654), "studio (7654)");
  CHECK_STR(mdns::instanceNameFor("", "studio", 7664), "studio (7664)");
  // An explicit --name wins, and is still sanitised.
  CHECK_STR(mdns::instanceNameFor("Stage.Left", "studio", 7654), "Stage-Left");
}

WEBLINKED_TEST(mdns_instance_id_is_stable_and_distinct) {
  // Stable across restarts, so a controller can recognise a returning
  // instance; distinct per port, so two on one host are two entries.
  CHECK_STR(mdns::instanceId("studio", 7654), mdns::instanceId("studio", 7654));
  CHECK(mdns::instanceId("studio", 7654) != mdns::instanceId("studio", 7664));
  CHECK(mdns::instanceId("studio", 7654) != mdns::instanceId("lobby", 7654));
}

WEBLINKED_TEST(mdns_refuses_to_advertise_a_loopback_bind) {
  // The trap this whole check exists for: the default bind is 127.0.0.1, so
  // the common case is an instance nothing off-machine can reach. Advertising
  // it puts a row in every fleet list on the subnet that fails to connect,
  // with nothing anywhere explaining why.
  std::string reason;
  CHECK(!mdns::bindIsAdvertisable("127.0.0.1", &reason));
  CHECK(!reason.empty());
  CHECK(!mdns::bindIsAdvertisable("127.0.1.1", &reason));
  CHECK(!mdns::bindIsAdvertisable("localhost", &reason));
  CHECK(!mdns::bindIsAdvertisable("::1", &reason));

  CHECK(mdns::bindIsAdvertisable("0.0.0.0", &reason));
  CHECK(reason.empty());
  CHECK(mdns::bindIsAdvertisable("192.168.1.40", &reason));
}
