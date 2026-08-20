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

}  // namespace dv::models
