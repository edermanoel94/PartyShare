#include <dv/models/user.hpp>

namespace dv::models {

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
