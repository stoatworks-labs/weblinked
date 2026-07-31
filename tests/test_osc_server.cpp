// The OSC packet decoder.
//
// These exist because of a bug that shipped in v0.3.0 and survived a whole
// release unnoticed: readString() advanced the offset by padded(textLength + 1)
// when padded() already accounted for the terminating NUL. For any string whose
// length was 3 mod 4 the offset ran four bytes past the end, the read reported
// failure, and the *entire message* was discarded in silence.
//
// It was invisible for three reasons worth remembering. It affected only a
// quarter of possible strings, so /weblinked/url worked for most URLs somebody
// tried. It failed silently rather than logging, because a malformed packet is
// a normal thing to ignore. And the decoder lived in the target that links CEF,
// so nothing cheap could reach it — which is why osc_server.cpp is now part of
// weblinked_core.
//
// The lengths below are chosen to cover every residue mod 4, on both the
// address and the argument, because that residue is the whole bug.

#include <cstring>
#include <string>
#include <vector>

#include "control/osc_server.h"
#include "test_support.h"

using namespace weblinked;

namespace {

/// Builds an OSC string exactly as a sender must: the text, a NUL, then padding
/// to the next multiple of four. Written out longhand rather than reusing the
/// decoder's own helper, so a mistake in that helper cannot cancel itself out.
void appendOscString(std::vector<uint8_t>& out, const std::string& text) {
  out.insert(out.end(), text.begin(), text.end());
  out.push_back(0);
  while (out.size() % 4 != 0) {
    out.push_back(0);
  }
}

/// One message: address, ",s" type tag, one string argument.
std::vector<uint8_t> stringMessage(const std::string& address,
                                   const std::string& argument) {
  std::vector<uint8_t> packet;
  appendOscString(packet, address);
  appendOscString(packet, ",s");
  appendOscString(packet, argument);
  return packet;
}

/// Decodes one packet and returns every message it produced.
std::vector<OscServer::Message> decode(const std::vector<uint8_t>& packet) {
  std::vector<OscServer::Message> received;
  OscServer::decodePacket(packet.data(), packet.size(),
                          [&](const OscServer::Message& message) {
                            received.push_back(message);
                          });
  return received;
}

/// A string of exactly `length` characters, made of URL-legal text so the cases
/// read like the real thing they stand for.
std::string ofLength(size_t length) {
  std::string text = "file:///x/";
  while (text.size() < length) {
    text.push_back('a' + static_cast<char>(text.size() % 26));
  }
  text.resize(length);
  return text;
}

}  // namespace

WEBLINKED_TEST(osc_decodes_a_string_argument_of_every_length_mod_four) {
  // 60..67 covers each residue twice. 63 is the length of the file:// URL that
  // exposed the bug in the first place.
  for (size_t length = 60; length <= 67; ++length) {
    const std::string argument = ofLength(length);
    const auto received = decode(stringMessage("/weblinked/url", argument));

    CHECK_EQ(received.size(), 1u);
    if (received.size() != 1) {
      std::printf("    argument length %zu (%zu mod 4) produced no message\n",
                  length, length % 4);
      continue;
    }
    CHECK_STR(received.front().address, "/weblinked/url");
    CHECK_EQ(received.front().stringArgs.size(), 1u);
    if (received.front().stringArgs.size() == 1) {
      CHECK_STR(received.front().stringArgs.front(), argument);
    }
  }
}

WEBLINKED_TEST(osc_decodes_an_address_of_every_length_mod_four) {
  // The address is read by the same function, so a padding mistake there eats
  // the type tag rather than the argument — same silence, different cause.
  for (size_t extra = 0; extra < 4; ++extra) {
    std::string address = "/weblinked/source/ab/url";  // 24 characters
    address.append(extra, 'x');
    const auto received = decode(stringMessage(address, "https://example.com"));

    CHECK_EQ(received.size(), 1u);
    if (received.size() != 1) {
      std::printf("    address length %zu (%zu mod 4) produced no message\n",
                  address.size(), address.size() % 4);
      continue;
    }
    CHECK_STR(received.front().address, address);
    CHECK_EQ(received.front().stringArgs.size(), 1u);
  }
}

WEBLINKED_TEST(osc_decodes_the_url_that_exposed_the_padding_bug) {
  // The exact message that did nothing at all in v0.3.0.
  const std::string url =
      "file:///Users/allansargeant/Projects/weblinked/tools/clock.html";
  CHECK_EQ(url.size() % 4, 3u);  // the residue that broke

  const auto received = decode(stringMessage("/weblinked/url", url));
  CHECK_EQ(received.size(), 1u);
  if (received.size() == 1 && received.front().stringArgs.size() == 1) {
    CHECK_STR(received.front().stringArgs.front(), url);
  }
}

WEBLINKED_TEST(osc_decodes_a_message_with_no_arguments) {
  // A bare trigger: address, no type tag at all. Valid OSC 1.0, and what a
  // Companion button sends when it is bound to a plain reload.
  std::vector<uint8_t> packet;
  appendOscString(packet, "/weblinked/reload");

  const auto received = decode(packet);
  CHECK_EQ(received.size(), 1u);
  if (received.size() == 1) {
    CHECK_STR(received.front().address, "/weblinked/reload");
    CHECK(received.front().stringArgs.empty());
    // firstBool's fallback is what distinguishes "no argument" from an explicit
    // zero, which is the difference between reload and do-nothing.
    CHECK(received.front().firstBool(true));
    CHECK(!received.front().firstBool(false));
  }
}

WEBLINKED_TEST(osc_decodes_mixed_argument_types) {
  std::vector<uint8_t> packet;
  appendOscString(packet, "/weblinked/output/Graphic");
  appendOscString(packet, ",si");
  appendOscString(packet, "abc");  // 3 mod 4 again, mid-packet this time
  // OSC integers are big-endian on the wire, so the most significant byte goes
  // first. Written straight from the value: converting to network order *and*
  // then emitting most-significant-first would swap it twice.
  const int32_t value = 1;
  for (int shift = 24; shift >= 0; shift -= 8) {
    packet.push_back(static_cast<uint8_t>((value >> shift) & 0xff));
  }

  const auto received = decode(packet);
  CHECK_EQ(received.size(), 1u);
  if (received.size() == 1) {
    CHECK_EQ(received.front().stringArgs.size(), 1u);
    CHECK_EQ(received.front().intArgs.size(), 1u);
    if (!received.front().intArgs.empty()) {
      CHECK_EQ(received.front().intArgs.front(), 1);
    }
  }
}

WEBLINKED_TEST(osc_rejects_a_truncated_packet_without_reading_past_it) {
  // The guard that the padding bug was tripping legitimately: a string with no
  // terminator inside the packet must be refused, not read off the end.
  std::vector<uint8_t> packet = {'/', 'w', 'e', 'b'};
  const auto received = decode(packet);
  CHECK(received.empty());
}

WEBLINKED_TEST(osc_unwraps_a_bundle) {
  const auto inner = stringMessage("/weblinked/url", ofLength(63));

  std::vector<uint8_t> packet;
  appendOscString(packet, "#bundle");
  for (int i = 0; i < 8; ++i) {
    packet.push_back(0);  // timetag, ignored
  }
  const uint32_t size = static_cast<uint32_t>(inner.size());
  for (int shift = 24; shift >= 0; shift -= 8) {
    packet.push_back(static_cast<uint8_t>((size >> shift) & 0xff));
  }
  packet.insert(packet.end(), inner.begin(), inner.end());

  const auto received = decode(packet);
  CHECK_EQ(received.size(), 1u);
  if (received.size() == 1) {
    CHECK_STR(received.front().address, "/weblinked/url");
  }
}
