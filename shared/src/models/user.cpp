#include <algorithm>

#include <dv/models/user.hpp>

namespace dv::models {

bool is_valid_display_name(const std::string& name) {
  // Not trimmed first, unlike a room name. A room name is stored trimmed, so
  // measuring the untrimmed one would make the rule depend on padding; a
  // display name is stored as given, so the thing to check is the thing that
  // will be stored.
  //
  // No length limit here on purpose. An account created without a display name
  // is given its username as one, usernames are not capped, and a rule that
  // refused a long name would lock those accounts out of every room. Length is
  // a separate question from this one and does not belong in the same answer.
  return std::ranges::none_of(
      name, [](unsigned char character) { return character < 0x20 || character == 0x7F; });
}

std::string_view to_string(Role role) noexcept {
  return role == Role::Admin ? "admin" : "user";
}

Role role_from_string(std::string_view name) noexcept {
  // Only an exact "admin" grants the role. Everything else, including the
  // empty string and a value from a future version of the schema, is a plain
  // user: a parser that guesses in the permissive direction is a parser that
  // hands out privileges by accident.
  return name == "admin" ? Role::Admin : Role::User;
}

std::string user_label(const std::string& id, const std::string& display_name,
                       const std::string& username) {
  // The username stands in for a missing display name rather than the two
  // being joined blindly: an account whose display name did not survive a
  // store round trip should read as its username, not as empty parentheses.
  const std::string& name = display_name.empty() ? username : display_name;
  if (name.empty()) {
    return id;
  }
  if (username.empty() || name == username) {
    return name;
  }
  return name + " (" + username + ")";
}

std::string describe(const Restrictions& restrictions) {
  // The wire names, in the order the struct declares them, so that two
  // accounts carrying the same restrictions produce the same line and an audit
  // log can be read by eye without sorting anything.
  std::string text;
  const auto append = [&text](std::string_view name) {
    if (!text.empty()) {
      text += ' ';
    }
    text += name;
  };

  if (restrictions.banned) {
    append("banned");
  }
  if (restrictions.muted) {
    append("muted");
  }
  if (restrictions.silenced) {
    append("silenced");
  }
  if (restrictions.screen_share_blocked) {
    append("screen_share_blocked");
  }
  return text;
}

}  // namespace dv::models
