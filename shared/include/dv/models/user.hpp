#pragma once

#include <string>

namespace dv::models {

/// Section 18 of SPEC.md.
struct User {
  std::string id;
  std::string display_name;
  /// URL or local path. Empty means the client falls back to initials.
  std::string avatar;

  friend bool operator==(const User&, const User&) = default;
};

/// A user as seen inside a room, together with the state the other
/// participants need to render them.
struct Participant {
  User user;
  bool muted = false;
  bool sharing_screen = false;

  friend bool operator==(const Participant&, const Participant&) = default;
};

}  // namespace dv::models
