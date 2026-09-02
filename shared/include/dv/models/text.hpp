#pragma once

#include <string>

namespace dv::models {

/// `text` without the whitespace around it.
///
/// Byte by byte, and without <cctype>, so that a byte above 127 cannot be
/// misread as a space by a locale nobody set. The continuation bytes of a
/// UTF-8 character are never any of the six characters this trims, which is
/// what makes a byte-wise trim safe on text that is not ASCII.
///
/// Shared rather than written where it is needed, because it is needed in two
/// places that have to agree. A chat line and an administrator's notice are
/// both text somebody typed, stored trimmed and length checked after the trim;
/// two copies of this would be two chances for the check and the storage to
/// drift apart by one space.
[[nodiscard]] std::string trim_text(const std::string& text);

}  // namespace dv::models
