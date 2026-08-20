#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace dv::models {

/// What a user is allowed to do, section 18 of SPEC.md.
///
/// Two values and no more, because a third one costs a policy nobody has
/// written yet. `User` is deliberately the zero value: a record that arrives
/// without a role, from an older database or a hand written document, is a
/// plain user and never an administrator.
enum class Role : std::uint8_t {
  /// Joins rooms, creates rooms, shares a screen, mutes themselves.
  User,
  /// Everything above, plus removing and muting other participants and
  /// managing the accounts and the rooms.
  Admin,
};

/// The wire and database name of a role, "user" or "admin".
[[nodiscard]] std::string_view to_string(Role role) noexcept;

/// Inverse of to_string. Anything unrecognised is `Role::User`, for the same
/// reason the enum starts there: the safe answer to "I do not know what this
/// is" is the role that can do less.
[[nodiscard]] Role role_from_string(std::string_view name) noexcept;

/// Section 18 of SPEC.md.
struct User {
  std::string id;
  std::string display_name;
  /// URL or local path. Empty means the client falls back to initials.
  std::string avatar;
  Role role = Role::User;

  [[nodiscard]] bool is_admin() const noexcept { return role == Role::Admin; }

  friend bool operator==(const User&, const User&) = default;
};

/// A user as seen inside a room, together with the state the other
/// participants need to render them.
struct Participant {
  User user;
  bool muted = false;
  /// Whether it was an administrator who muted them, rather than themselves.
  ///
  /// Its own flag and not a shade of `muted`, because the two behave
  /// differently: a participant releases their own mute whenever they like,
  /// and only an administrator releases this one. Without the distinction a
  /// forced mute is advice, undone by one click on the button that turned
  /// itself off.
  bool muted_by_admin = false;
  bool sharing_screen = false;

  friend bool operator==(const Participant&, const Participant&) = default;
};

}  // namespace dv::models
