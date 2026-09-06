#include "app/server_address.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace dv::client::app {
namespace {

constexpr std::string_view kWhitespace = " \t\r\n";

/// The schemes the field understands, and what each one means to the
/// signaling client. Two of them are the browser's, because an address copied
/// out of a browser's bar is the most likely thing to be pasted here.
struct Scheme {
  std::string_view typed;
  std::string_view spoken;
};
constexpr std::array<Scheme, 4> kSchemes = {{
    {.typed = "ws://", .spoken = "ws://"},
    {.typed = "wss://", .spoken = "wss://"},
    {.typed = "http://", .spoken = "ws://"},
    {.typed = "https://", .spoken = "wss://"},
}};

/// Everything short of the highest port number has five digits or fewer, so a
/// sixth digit is refused before it is turned into a number that might not fit.
constexpr std::size_t kLongestPort = 5;
constexpr int kHighestPort = 65535;

[[nodiscard]] std::string_view trimmed(std::string_view text) {
  const auto first = text.find_first_not_of(kWhitespace);
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(kWhitespace);
  return text.substr(first, last - first + 1);
}

/// Whether `text` begins with `prefix`, letter case aside. A scheme is case
/// insensitive by RFC 3986, and `WS://` typed with caps lock on is the same
/// scheme.
[[nodiscard]] bool starts_with_ignoring_case(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) {
    return false;
  }
  return std::ranges::equal(prefix, text.substr(0, prefix.size()), [](char left, char right) {
    return std::tolower(static_cast<unsigned char>(left)) ==
           std::tolower(static_cast<unsigned char>(right));
  });
}

[[nodiscard]] bool all_digits(std::string_view text) {
  return !text.empty() && std::ranges::all_of(text, [](char byte) {
    return std::isdigit(static_cast<unsigned char>(byte)) != 0;
  });
}

template <typename T>
[[nodiscard]] Result<T> refused(std::string message) {
  return Result<T>::failure("invalid_value", std::move(message));
}

/// What follows the scheme, taken apart.
struct Authority {
  /// As written, with the brackets an IPv6 address needs - added here when
  /// the address was typed without them.
  std::string host;
  /// The digits after the host's colon, or empty when there was no colon.
  std::string port;
  /// From the first `/`, `?` or `#` on, or empty. Carried through untouched:
  /// a path is not something this field is expected to hold, but one that was
  /// typed is not something to lose quietly either.
  std::string rest;
};

/// Splits `text`, an address with its scheme already taken off, into its host,
/// its port and whatever follows them.
[[nodiscard]] Result<Authority> split(std::string_view text) {
  Authority parts;
  const auto rest_at = text.find_first_of("/?#");
  const std::string_view authority = text.substr(0, rest_at);
  if (rest_at != std::string_view::npos) {
    parts.rest = std::string(text.substr(rest_at));
  }

  std::string_view port;
  bool has_port = false;
  if (authority.starts_with('[')) {
    const auto close = authority.find(']');
    if (close == std::string_view::npos) {
      return refused<Authority>(
          "An IPv6 address goes in brackets, with the port after them: [::1]:8080.");
    }
    parts.host = std::string(authority.substr(0, close + 1));
    const std::string_view after = authority.substr(close + 1);
    if (!after.empty()) {
      if (!after.starts_with(':')) {
        return refused<Authority>(
            "After an IPv6 address in brackets only a port can follow: [::1]:8080.");
      }
      has_port = true;
      port = after.substr(1);
    }
  } else {
    const auto colons = std::ranges::count(authority, ':');
    if (colons == 0) {
      parts.host = std::string(authority);
    } else if (colons == 1) {
      const auto colon = authority.find(':');
      parts.host = std::string(authority.substr(0, colon));
      has_port = true;
      port = authority.substr(colon + 1);
    } else {
      // Two or more colons and no brackets is an IPv6 address written bare,
      // `::1`, and there is no telling its last group from a port. Taken as
      // the whole address and given its brackets, so that a port can be put
      // after it.
      parts.host = "[" + std::string(authority) + "]";
    }
  }

  if (parts.host.empty() || parts.host == "[]") {
    return refused<Authority>(
        "Enter the server's address: an IP or a name, such as 192.168.1.10 or "
        "party.example.com.");
  }
  if (has_port) {
    const int number =
        all_digits(port) && port.size() <= kLongestPort ? std::stoi(std::string(port)) : 0;
    if (number < 1 || number > kHighestPort) {
      return refused<Authority>(
          "The port after the colon has to be a number from 1 to 65535: 192.168.1.10:8080.");
    }
    parts.port = std::string(port);
  }
  return parts;
}

}  // namespace

Result<std::string> expand_server_address(std::string_view typed) {
  const std::string_view address = trimmed(typed);
  if (address.empty()) {
    return refused<std::string>(
        "Enter the server's address: an IP or a name, such as 192.168.1.10 or "
        "party.example.com.");
  }
  if (address.find_first_of(kWhitespace) != std::string_view::npos) {
    return refused<std::string>("A server address has no spaces in it.");
  }

  std::string_view scheme;
  std::string_view after_scheme = address;
  for (const Scheme& known : kSchemes) {
    if (starts_with_ignoring_case(address, known.typed)) {
      scheme = known.spoken;
      after_scheme = address.substr(known.typed.size());
      break;
    }
  }
  if (scheme.empty() && address.find("://") != std::string_view::npos) {
    return refused<std::string>(
        "PartyShare reaches its server over ws:// or wss://. An IP or a name on its own is "
        "enough: 192.168.1.10, or party.example.com.");
  }

  Result<Authority> parts = split(after_scheme);
  if (!parts) {
    return refused<std::string>(parts.error().message);
  }
  Authority authority = std::move(parts).take();

  // A URL that was typed as one is taken as written. A bare address is what
  // the field asks for, and it is completed with the two things the program
  // knows and the person should not have to.
  const bool bare = scheme.empty();
  if (bare) {
    scheme = "ws://";
    if (authority.port.empty()) {
      authority.port = std::string(kDefaultServerPort);
    }
  }

  std::string url = std::string(scheme) + authority.host;
  if (!authority.port.empty()) {
    url += ":" + authority.port;
  }
  url += authority.rest;
  return url;
}

std::string display_server_address(std::string_view url) {
  constexpr std::string_view kPlain = "ws://";
  if (!url.starts_with(kPlain)) {
    return std::string(url);
  }
  const Result<Authority> parts = split(url.substr(kPlain.size()));
  if (!parts || parts.value().port.empty()) {
    return std::string(url);
  }
  return std::string(url.substr(kPlain.size()));
}

}  // namespace dv::client::app
