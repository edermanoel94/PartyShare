// What the Server field of the sign-in screen makes of what is typed into it.
//
// The field asks for an IP or a name and hands the signaling client a URL, so
// the interesting cases are the ones where those differ: a missing scheme, a
// missing port, an address copied out of a browser, an IPv6 address with
// nowhere obvious to put a port. And the round trip through
// display_server_address, which is what decides whether an address that came
// out of config.ini and went through the field untouched counts as a change.

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "app/server_address.hpp"

namespace {

using dv::client::app::display_server_address;
using dv::client::app::expand_server_address;

/// The URL for `typed`, or the refusal's message prefixed with "refused: " so
/// that a wrong expectation prints what was said rather than throwing.
[[nodiscard]] std::string expanded(std::string_view typed) {
  const auto result = expand_server_address(typed);
  return result ? result.value() : "refused: " + result.error().message;
}

[[nodiscard]] bool refused(std::string_view typed) {
  const auto result = expand_server_address(typed);
  return !result && result.error().code == "invalid_value";
}

TEST(ServerAddressTest, ABareAddressGetsTheSchemeAndTheDefaultPort) {
  EXPECT_EQ(expanded("192.168.1.10"), "ws://192.168.1.10:8080");
  EXPECT_EQ(expanded("party.example.com"), "ws://party.example.com:8080");
  EXPECT_EQ(expanded("localhost"), "ws://localhost:8080");
}

TEST(ServerAddressTest, APortThatWasTypedIsKept) {
  EXPECT_EQ(expanded("192.168.1.10:9000"), "ws://192.168.1.10:9000");
  EXPECT_EQ(expanded("party.example.com:443"), "ws://party.example.com:443");
}

TEST(ServerAddressTest, AUrlIsTakenAsWritten) {
  // Port or no port: wss://party.example.com is a reverse proxy on 443, and
  // giving it :8080 would break the one address the person got right.
  EXPECT_EQ(expanded("ws://192.168.1.10:8080"), "ws://192.168.1.10:8080");
  EXPECT_EQ(expanded("wss://party.example.com"), "wss://party.example.com");
  EXPECT_EQ(expanded("ws://party.example.com"), "ws://party.example.com");
  EXPECT_EQ(expanded("wss://party.example.com/ws"), "wss://party.example.com/ws");
}

TEST(ServerAddressTest, ABrowsersSchemeBecomesTheSocketsOne) {
  EXPECT_EQ(expanded("http://192.168.1.10:8080"), "ws://192.168.1.10:8080");
  EXPECT_EQ(expanded("https://party.example.com"), "wss://party.example.com");
}

TEST(ServerAddressTest, TheSchemeIsReadWhicheverCaseItIsTypedIn) {
  EXPECT_EQ(expanded("WS://192.168.1.10:8080"), "ws://192.168.1.10:8080");
  EXPECT_EQ(expanded("HTTPS://party.example.com"), "wss://party.example.com");
}

TEST(ServerAddressTest, SurroundingWhitespaceIsDroppedAndInnerWhitespaceRefused) {
  EXPECT_EQ(expanded("  192.168.1.10:8080 \n"), "ws://192.168.1.10:8080");
  EXPECT_TRUE(refused("192.168. 1.10"));
  EXPECT_TRUE(refused("party example.com"));
}

TEST(ServerAddressTest, APathIsCarriedThrough) {
  EXPECT_EQ(expanded("party.example.com/ws"), "ws://party.example.com:8080/ws");
  EXPECT_EQ(expanded("party.example.com:9000/ws?x=1"), "ws://party.example.com:9000/ws?x=1");
}

TEST(ServerAddressTest, AnIpv6AddressIsGivenBracketsSoThatAPortCanFollow) {
  EXPECT_EQ(expanded("::1"), "ws://[::1]:8080");
  EXPECT_EQ(expanded("fe80::1"), "ws://[fe80::1]:8080");
  EXPECT_EQ(expanded("[::1]"), "ws://[::1]:8080");
  EXPECT_EQ(expanded("[::1]:9000"), "ws://[::1]:9000");
  EXPECT_EQ(expanded("ws://[::1]:9000"), "ws://[::1]:9000");
}

TEST(ServerAddressTest, RefusesWhatTheSocketWouldRefuseLater) {
  EXPECT_TRUE(refused(""));
  EXPECT_TRUE(refused("   "));
  EXPECT_TRUE(refused(":8080"));
  EXPECT_TRUE(refused("ws://"));
  EXPECT_TRUE(refused("ws://:8080"));
  EXPECT_TRUE(refused("[::1"));
  EXPECT_TRUE(refused("[::1]x"));
  EXPECT_TRUE(refused("tcp://192.168.1.10:8080"));
  EXPECT_TRUE(refused("ftp://party.example.com"));
}

TEST(ServerAddressTest, RefusesAPortThatIsNotOne) {
  EXPECT_TRUE(refused("192.168.1.10:"));
  EXPECT_TRUE(refused("192.168.1.10:abc"));
  EXPECT_TRUE(refused("192.168.1.10:0"));
  EXPECT_TRUE(refused("192.168.1.10:65536"));
  EXPECT_TRUE(refused("192.168.1.10:123456"));
  EXPECT_TRUE(refused("ws://192.168.1.10:99999"));
  EXPECT_EQ(expanded("192.168.1.10:65535"), "ws://192.168.1.10:65535");
}

TEST(ServerAddressTest, EveryRefusalComesWithASentenceForTheScreen) {
  for (const std::string_view typed : {"", "a b", "ws://", "x:y", "tcp://x", "[::1"}) {
    const auto result = expand_server_address(typed);
    ASSERT_FALSE(result.ok()) << typed;
    EXPECT_FALSE(result.error().message.empty()) << typed;
  }
}

TEST(ServerAddressTest, DisplayTakesTheSchemeOffAPlainAddressWithAPort) {
  EXPECT_EQ(display_server_address("ws://192.168.1.10:8080"), "192.168.1.10:8080");
  EXPECT_EQ(display_server_address("ws://party.example.com:9000"), "party.example.com:9000");
  EXPECT_EQ(display_server_address("ws://[::1]:8080"), "[::1]:8080");
}

TEST(ServerAddressTest, DisplayLeavesAloneWhatWouldNotComeBackTheSame) {
  // wss:// says TLS, which a bare address does not; ws://host would gain a
  // port on the way back; and a URL the socket would refuse is shown as it is,
  // so that the refusal is about what is on screen.
  EXPECT_EQ(display_server_address("wss://party.example.com"), "wss://party.example.com");
  EXPECT_EQ(display_server_address("wss://party.example.com:443"), "wss://party.example.com:443");
  EXPECT_EQ(display_server_address("ws://party.example.com"), "ws://party.example.com");
  EXPECT_EQ(display_server_address("ws://"), "ws://");
  EXPECT_EQ(display_server_address(""), "");
  EXPECT_EQ(display_server_address("not a url"), "not a url");
}

TEST(ServerAddressTest, WhatDisplayShowsExpandsBackToTheSameUrl) {
  for (const std::string_view url :
       {"ws://192.168.1.10:8080", "ws://party.example.com:9000", "ws://[::1]:8080",
        "ws://party.example.com:8080/ws", "wss://party.example.com", "ws://party.example.com",
        "wss://party.example.com:443"}) {
    EXPECT_EQ(expanded(display_server_address(url)), url) << url;
  }
}

}  // namespace
