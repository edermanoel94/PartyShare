#pragma once

#include <cstddef>
#include <string_view>
#include <vector>

namespace dv::client::app {

/// One run of a chat line that is a URL, as byte offsets into that line.
///
/// Offsets and not a copy of the text, because the caller has to put the rest
/// of the line back around it: what this feeds is a renderer that escapes the
/// text between the links and wraps the links themselves.
struct LinkSpan {
  /// Offset of the first byte of the URL.
  std::size_t begin = 0;
  /// Offset one past its last byte.
  std::size_t end = 0;

  friend bool operator==(const LinkSpan&, const LinkSpan&) = default;
};

/// Every URL in `text`, in the order they appear, never overlapping.
///
/// Only http:// and https:// are recognised, and that is a decision rather
/// than a limitation of the parser. What comes out of here is handed to the
/// operating system's shell to open, and a chat message is written by another
/// participant: `file:///C:/...` and `javascript:` are the two obvious ways to
/// turn somebody else's line of text into an action on this machine. Anything
/// that is not one of the two web schemes stays plain text, which a reader can
/// still select and copy if they mean it.
///
/// The offsets are bytes, and the text is UTF-8. Nothing here inspects a byte
/// above 0x7F, so a URL with accented characters in it is carried through
/// whole rather than cut at a continuation byte.
[[nodiscard]] std::vector<LinkSpan> find_links(std::string_view text);

}  // namespace dv::client::app
