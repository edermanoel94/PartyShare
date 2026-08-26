#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <unordered_map>

#include <dv/core/result.hpp>
#include <dv/models/user.hpp>

#include "store/user_store.hpp"

namespace dv::server {

/// Username and password authentication, which section 18 of SPEC.md allows to
/// stay simple for the MVP.
///
/// Passwords are stored as a random salt plus the scrypt derivation of salt and
/// password, so nothing readable is kept in memory or written to a log. See
/// derive_key_hex in the implementation for why a plain digest is not enough.
///
/// Where the accounts live is not this class's business: it holds a UserStore
/// and does the cryptography. That is what lets the same code serve a server
/// with a MongoDB behind it and one with nothing but memory, and what keeps the
/// tests free of a database.
///
/// Tokens are the exception and stay here, in memory. A session token is worth
/// exactly one process lifetime: persisting it would mean a stolen database
/// hands over live sessions as well as password hashes, and the client already
/// reconnects by authenticating again.
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

  /// Owns an in-memory store, which is what a server without a database and
  /// most of the tests want.
  Authenticator();
  explicit Authenticator(Options options);
  /// Uses the caller's store, which has to outlive this object.
  Authenticator(Options options, store::UserStore& users);

  /// Registers an account. Fails with user_exists on a duplicate username, and
  /// with invalid_value on empty input.
  [[nodiscard]] Result<models::User> add_user(const std::string& username,
                                              const std::string& password, std::string display_name,
                                              models::Role role = models::Role::User);

  /// Issues a session token. Fails with unauthorized, using the same message
  /// for an unknown user and a wrong password, so the reply cannot be used to
  /// enumerate accounts.
  [[nodiscard]] Result<Session> authenticate(const std::string& username,
                                             const std::string& password, Clock::time_point now);

  /// Resolves a token to the user it belongs to. Fails with unauthorized when
  /// the token is unknown or has expired.
  ///
  /// The account is read back from the store every time rather than copied
  /// into the token when it was issued. That is what makes a role change take
  /// effect on the next action instead of on the next login.
  [[nodiscard]] Result<models::User> validate(const std::string& token, Clock::time_point now);

  /// What a password looks like once stored: a fresh random salt and the key
  /// derived from the two.
  struct Credentials {
    std::string salt_hex;
    std::string password_hash_hex;
  };

  /// Derives credentials without writing them anywhere.
  ///
  /// Exists so that a caller changing several fields of an account at once can
  /// do it in a single store write. The alternative, calling set_password after
  /// its own update, is two writes that can half succeed and leave an account
  /// whose role changed and whose password did not. Fails with invalid_value on
  /// an empty password.
  [[nodiscard]] Result<Credentials> derive(const std::string& password) const;

  /// Replaces a password, hashing it with a fresh salt. Fails with
  /// user_not_found, and with invalid_value on an empty password.
  [[nodiscard]] std::optional<Error> set_password(const std::string& user_id,
                                                  const std::string& password);

  /// Replaces a password after checking the one the account has now.
  ///
  /// The check is what separates this from set_password, and it is not
  /// politeness: set_password is what an administrator uses on somebody else's
  /// account, and the authority there is the administrator's role. An ordinary
  /// user has no such authority over anything, so the only thing that can
  /// stand in for it is proof that they already know the password they are
  /// replacing. Without that, an unattended session is enough to take an
  /// account away from its owner.
  ///
  /// Failure codes:
  ///   user_not_found    no account with that identifier
  ///   invalid_password  `current_password` is not the account's password
  ///   invalid_value     the new password is empty, or is the current one
  ///
  /// Deliberately not `unauthorized`: that code means "this session is not who
  /// it says it is", and the client answers it by sending somebody back to the
  /// login screen. The session here is perfectly valid and one field of the
  /// form is wrong.
  ///
  /// Does not revoke anything. Whether the account's sessions survive a
  /// password change is a policy question, and the Hub is where it is
  /// answered - see Hub::handle_change_password.
  [[nodiscard]] std::optional<Error> change_password(const std::string& user_id,
                                                     const std::string& current_password,
                                                     const std::string& new_password);

  /// Ends every session of one account. Used when the account is deleted, so
  /// that a token issued a minute earlier stops working immediately rather
  /// than at its own expiry.
  void revoke_tokens_of(const std::string& user_id);

  /// Drops tokens that are past their lifetime.
  void expire_tokens(Clock::time_point now);

  [[nodiscard]] std::size_t active_token_count() const noexcept { return tokens_.size(); }

  [[nodiscard]] store::UserStore& users() noexcept { return *users_; }
  [[nodiscard]] const store::UserStore& users() const noexcept { return *users_; }

 private:
  struct Token {
    std::string user_id;
    Clock::time_point expires_at;
  };

  Options options_;
  /// Set only when this object created the store, and null when one was
  /// injected. `users_` is what everything else uses either way.
  std::unique_ptr<store::UserStore> owned_users_;
  store::UserStore* users_;
  std::unordered_map<std::string, Token> tokens_;  // by token
};

}  // namespace dv::server
