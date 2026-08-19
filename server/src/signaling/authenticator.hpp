#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

#include <dv/core/result.hpp>
#include <dv/models/user.hpp>

namespace dv::server {

/// Username and password authentication, which section 18 of SPEC.md allows to
/// stay simple for the MVP.
///
/// Passwords are stored as a random salt plus SHA-256 of salt and password, so
/// nothing readable is kept in memory or written to a log. That is still not
/// good enough for production: a fast hash is cheap to attack offline, and this
/// has to become Argon2id or bcrypt before real accounts exist. The interface
/// is unaffected by that change.
///
/// Not thread safe. The Hub owns the only instance.
class Authenticator {
 public:
  using Clock = std::chrono::steady_clock;

  struct Options {
    Clock::duration token_lifetime = std::chrono::hours(8);
  };

  struct Session {
    models::User user;
    std::string token;
    int expires_in_seconds = 0;
  };

  Authenticator();
  explicit Authenticator(Options options);

  /// Registers an account. Fails with user_exists on a duplicate username, and
  /// with invalid_value on empty input.
  [[nodiscard]] Result<models::User> add_user(const std::string& username,
                                              const std::string& password,
                                              std::string display_name);

  /// Issues a session token. Fails with unauthorized, using the same message
  /// for an unknown user and a wrong password, so the reply cannot be used to
  /// enumerate accounts.
  [[nodiscard]] Result<Session> authenticate(const std::string& username,
                                             const std::string& password, Clock::time_point now);

  /// Resolves a token to the user it belongs to. Fails with unauthorized when
  /// the token is unknown or has expired.
  [[nodiscard]] Result<models::User> validate(const std::string& token, Clock::time_point now);

  /// Drops tokens that are past their lifetime.
  void expire_tokens(Clock::time_point now);

  [[nodiscard]] std::size_t active_token_count() const noexcept { return tokens_.size(); }

 private:
  struct Account {
    models::User user;
    std::string salt_hex;
    std::string password_hash_hex;
  };

  struct Token {
    std::string user_id;
    Clock::time_point expires_at;
  };

  Options options_;
  std::unordered_map<std::string, Account> accounts_;  // by username
  std::unordered_map<std::string, Token> tokens_;      // by token
};

}  // namespace dv::server
