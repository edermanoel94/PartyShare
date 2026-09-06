#pragma once

#include <string>
#include <string_view>

#include <dv/core/result.hpp>

namespace dv::client::app {

/// The port the server listens on unless told otherwise: the default of
/// `--port` in server/src/main.cpp and of config::ServerConfig::port. Text
/// rather than a number, because the only thing done with it is putting it
/// after a colon.
inline constexpr std::string_view kDefaultServerPort = "8080";

/// Turns what somebody typed into the Server field of the sign-in screen into
/// the URL the signaling client wants.
///
/// The field asks for "the server", and what people have to hand is an IP or a
/// name, not a URL: `192.168.1.10`, `party.example.com`, sometimes with a port.
/// Making them type `ws://` in front is making them know what a WebSocket is,
/// and the default port is a fact about this program that the program can
/// supply itself. So:
///
/// - A bare address gets `ws://` in front and, without a port, `:8080` after.
/// - An address that already starts with `ws://` or `wss://` is taken as
///   written, port or no port. `wss://party.example.com` through a reverse
///   proxy on 443 is a real deployment, and appending `:8080` to it would break
///   the one address the person got right.
/// - `http://` and `https://` become `ws://` and `wss://`, because an address
///   copied out of a browser's bar is the most likely thing to be pasted here.
/// - An IPv6 address written bare, `::1`, is put in brackets so that the port
///   after it can be told from it.
///
/// Refused, with a sentence for the screen in the error's message: nothing at
/// all, a space in the middle, a scheme this client does not speak, a missing
/// host, or a port that is not a number from 1 to 65535. Every refusal is one
/// that SignalingClient::connect or the server would also refuse, only later
/// and in words about sockets.
///
/// Surrounding whitespace is dropped rather than refused: a trailing space
/// after a paste is invisible, and refusing it would read as refusing the
/// address.
[[nodiscard]] Result<std::string> expand_server_address(std::string_view typed);

/// The address as the Server field shows it: `url` with its `ws://` taken off
/// when taking it off loses nothing.
///
/// The field accepts what this puts there, and puts back exactly `url` - that
/// round trip is the contract, and it is what keeps a value that came from
/// config.ini and went through the field unchanged from counting as a change.
/// So the scheme only comes off a plain `ws://` address that names its port:
/// `ws://192.168.1.10:8080` shows as `192.168.1.10:8080`, which typed back in
/// expands to the same URL. `wss://` stays, because it says something -
/// TLS - that a bare address does not; and `ws://host` with no port stays,
/// because `host` typed back in would gain `:8080`.
[[nodiscard]] std::string display_server_address(std::string_view url);

}  // namespace dv::client::app
