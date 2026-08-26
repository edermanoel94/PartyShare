#include "app/chat_links.hpp"

#include <array>
#include <cstddef>
#include <string_view>
#include <vector>

namespace dv::client::app {
namespace {

constexpr std::string_view kHttp = "http://";
constexpr std::string_view kHttps = "https://";

/// ASCII only, and deliberately: std::tolower takes an int and is undefined for
/// a negative char, which is every byte of a UTF-8 character above U+007F.
[[nodiscard]] constexpr char lower(char byte) noexcept {
  return (byte >= 'A' && byte <= 'Z') ? static_cast<char>(byte - 'A' + 'a') : byte;
}

[[nodiscard]] bool starts_with_ignoring_case(std::string_view text, std::string_view prefix) {
  if (text.size() < prefix.size()) {
    return false;
  }
  for (std::size_t i = 0; i < prefix.size(); ++i) {
    if (lower(text[i]) != prefix[i]) {
      return false;
    }
  }
  return true;
}

/// Whether a scheme may begin at this byte.
///
/// RFC 3986 lets a scheme hold letters, digits, `+`, `-` and `.`, so those are
/// exactly the bytes that must not come first: without this, the `http://` in
/// `xhttp://` or `not-http://` would be read as the start of a URL rather than
/// as the tail of a word somebody wrote.
[[nodiscard]] bool may_start_scheme(std::string_view text, std::size_t at) {
  if (at == 0) {
    return true;
  }
  const char before = text[at - 1];
  const bool scheme_byte = (before >= 'a' && before <= 'z') || (before >= 'A' && before <= 'Z') ||
                           (before >= '0' && before <= '9') || before == '+' || before == '-' ||
                           before == '.';
  return !scheme_byte;
}

/// Where the URL stops: the first space, control character or angle bracket.
///
/// Angle brackets are in the list because `<http://example.com>` is a common
/// way to write a URL in running text, and because the renderer above this
/// escapes them - a bracket left inside the span would come out as `&lt;`
/// inside an href.
[[nodiscard]] bool terminates_url(char byte) {
  const auto value = static_cast<unsigned char>(byte);
  if (value <= ' ' || value == 0x7F) {
    return true;
  }
  return byte == '<' || byte == '>' || byte == '"';
}

/// Trims the punctuation a sentence put after the URL rather than inside it.
///
/// `See http://example.com.` ends in a full stop that belongs to the sentence,
/// and `(see http://example.com/a_(b))` ends in a parenthesis that belongs to
/// the URL. The difference is whether the closing bracket has an opener inside
/// the span, which is the rule every other linkifier settled on for the same
/// reason: it is the only one that gets Wikipedia URLs right.
[[nodiscard]] std::size_t trim_trailing_punctuation(std::string_view text, std::size_t begin,
                                                    std::size_t end) {
  constexpr std::array<char, 3> kOpeners = {'(', '[', '{'};
  constexpr std::array<char, 3> kClosers = {')', ']', '}'};

  while (end > begin) {
    const char last = text[end - 1];

    bool closer = false;
    for (std::size_t i = 0; i < kClosers.size(); ++i) {
      if (last != kClosers[i]) {
        continue;
      }
      closer = true;
      std::size_t opened = 0;
      std::size_t closed = 0;
      for (std::size_t at = begin; at < end; ++at) {
        opened += static_cast<std::size_t>(text[at] == kOpeners[i]);
        closed += static_cast<std::size_t>(text[at] == kClosers[i]);
      }
      // Balanced or over-opened: the bracket is part of the URL, and so is
      // everything before it. Nothing further can be trimmed either, because
      // whatever precedes a kept bracket is inside the URL by construction.
      if (opened >= closed) {
        return end;
      }
      break;
    }
    if (closer) {
      --end;
      continue;
    }

    // Sentence punctuation. A URL may legitimately end in any of these, but a
    // line of prose ends in them far more often, and the cost of the wrong
    // guess is one character the reader has to add back rather than a link
    // that swallowed the full stop after it.
    if (last == '.' || last == ',' || last == ';' || last == ':' || last == '!' || last == '?' ||
        last == '\'' || last == '`') {
      --end;
      continue;
    }
    break;
  }
  return end;
}

}  // namespace

std::vector<LinkSpan> find_links(std::string_view text) {
  std::vector<LinkSpan> spans;

  std::size_t at = 0;
  while (at < text.size()) {
    const std::string_view rest = text.substr(at);
    std::string_view scheme;
    if (starts_with_ignoring_case(rest, kHttps)) {
      scheme = kHttps;
    } else if (starts_with_ignoring_case(rest, kHttp)) {
      scheme = kHttp;
    }
    if (scheme.empty() || !may_start_scheme(text, at)) {
      ++at;
      continue;
    }

    std::size_t end = at + scheme.size();
    while (end < text.size() && !terminates_url(text[end])) {
      ++end;
    }

    // `http://` on its own is a scheme and no host. Linking it would give the
    // reader something to click that can only fail.
    const std::size_t authority = at + scheme.size();
    end = trim_trailing_punctuation(text, authority, end);
    if (end <= authority) {
      at = authority;
      continue;
    }

    spans.push_back(LinkSpan{.begin = at, .end = end});
    at = end;
  }

  return spans;
}

}  // namespace dv::client::app
