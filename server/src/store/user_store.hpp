#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <dv/core/result.hpp>
#include <dv/models/user.hpp>

namespace dv::server::store {

/// One account, as the server keeps it.
///
/// This type never crosses the wire. `protocol::UserSummary` is what an
/// administrator receives, and it exists precisely so that the salt and the
/// hash below have no serializer at all: a field that cannot be written out
/// cannot be leaked by a future message that forgot to exclude it.
struct Account {
  /// Carries the identifier, the display name and the role.
  models::User user;
  std::string username;
  std::string salt_hex;
  std::string password_hash_hex;
  /// Seconds since the Unix epoch, UTC. The store stamps it on `create` when
  /// the caller leaves it at zero, which is what keeps the wall clock out of
  /// the Hub and lets a test pin it.
  std::int64_t created_at = 0;
};

/// Where accounts live.
///
/// An interface rather than a class because there are two of them: one in
/// memory, which is what the tests and a server without a database use, and
/// one on MongoDB. The Authenticator holds a reference to whichever it was
/// given and knows nothing about which one it is.
///
/// Not thread safe. The Hub owns the only reference and serializes access,
/// exactly as it does for the RoomManager.
class UserStore {
 public:
  UserStore() = default;
  virtual ~UserStore() = default;

  UserStore(const UserStore&) = delete;
  UserStore& operator=(const UserStore&) = delete;
  UserStore(UserStore&&) = delete;
  UserStore& operator=(UserStore&&) = delete;

  /// Fails with `user_exists` when the username is taken.
  [[nodiscard]] virtual std::optional<Error> create(Account account) = 0;

  [[nodiscard]] virtual std::optional<Account> find_by_username(
      const std::string& username) const = 0;
  [[nodiscard]] virtual std::optional<Account> find_by_id(const std::string& user_id) const = 0;

  /// Every account, oldest first.
  [[nodiscard]] virtual std::vector<Account> list() const = 0;

  /// Replaces everything but the identifier and the creation time. Fails with
  /// `user_not_found`.
  [[nodiscard]] virtual std::optional<Error> update(const Account& account) = 0;

  /// Fails with `user_not_found`.
  [[nodiscard]] virtual std::optional<Error> remove(const std::string& user_id) = 0;

  /// How many accounts hold `role`.
  ///
  /// Its own operation rather than a filter over `list`, because the rule it
  /// serves is worth naming: the last administrator may not be deleted or
  /// demoted, and a system with nobody able to administer it is a system that
  /// needs the database edited by hand to be recovered.
  [[nodiscard]] virtual std::size_t count_with_role(models::Role role) const = 0;
};

}  // namespace dv::server::store
