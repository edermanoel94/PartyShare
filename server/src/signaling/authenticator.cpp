#include "signaling/authenticator.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

#include "store/memory_store.hpp"

namespace dv::server {
namespace {

std::string to_hex(const unsigned char* data, std::size_t size) {
  static constexpr std::string_view kDigits = "0123456789abcdef";
  std::string hex;
  hex.reserve(size * 2);
  for (std::size_t i = 0; i < size; ++i) {
    hex.push_back(kDigits[data[i] >> 4]);
    hex.push_back(kDigits[data[i] & 0x0F]);
  }
  return hex;
}

std::string random_hex(std::size_t bytes) {
  std::vector<unsigned char> buffer(bytes);
  if (RAND_bytes(buffer.data(), static_cast<int>(bytes)) != 1) {
    // Without a working CSPRNG nothing here can be trusted, so failing loudly
    // is the only safe option.
    throw std::runtime_error("the system random number generator failed");
  }
  return to_hex(buffer.data(), buffer.size());
}

/// Derives the stored form of a password.
///
/// scrypt rather than a plain digest, because a digest is fast by design and
/// that is the one property a password store must not have. Section 17 of
/// SPEC.md forbids credentials in plain text; a SHA-256 of a password is not
/// plain text but it is not far off, since a modern card tries billions of
/// candidates a second against it.
///
/// The cost parameters are the usual interactive ones: N of 2^14 with r of 8
/// is sixteen mebibytes and something like fifty milliseconds per attempt.
/// Bigger is stronger against an attacker with the file, and is also what an
/// attacker without it would aim a flood of logins at, so this is deliberately
/// the moderate end rather than the maximum.
std::string derive_key_hex(const std::string& password, const std::string& salt_hex) {
  constexpr std::uint64_t kCost = 1U << 14U;  // N
  constexpr std::uint64_t kBlockSize = 8;     // r
  constexpr std::uint64_t kParallelism = 1;   // p
  constexpr std::size_t kKeyBytes = 32;
  // OpenSSL refuses anything above this, and N * r * 128 is what scrypt uses.
  constexpr std::uint64_t kMaxMemory = std::uint64_t{64} * 1024 * 1024;

  std::array<unsigned char, kKeyBytes> key{};
  if (EVP_PBE_scrypt(password.data(), password.size(),
                     reinterpret_cast<const unsigned char*>(salt_hex.data()), salt_hex.size(),
                     kCost, kBlockSize, kParallelism, kMaxMemory, key.data(), key.size()) != 1) {
    throw std::runtime_error("password key derivation failed");
  }
  return to_hex(key.data(), key.size());
}

/// Compares without leaking, through timing, how many leading characters
/// matched.
bool constant_time_equals(const std::string& left, const std::string& right) {
  if (left.size() != right.size()) {
    return false;
  }
  return CRYPTO_memcmp(left.data(), right.data(), left.size()) == 0;
}

Error unauthorized() {
  // Deliberately identical for an unknown user and a wrong password.
  return Error{.code = "unauthorized", .message = "invalid username or password"};
}

}  // namespace

Authenticator::Authenticator() : Authenticator(Options{}) {}

Authenticator::Authenticator(Options options)
    : options_(options),
      owned_users_(std::make_unique<store::MemoryUserStore>()),
      users_(owned_users_.get()) {}

Authenticator::Authenticator(Options options, store::UserStore& users)
    : options_(options), users_(&users) {}

Result<models::User> Authenticator::add_user(const std::string& username,
                                             const std::string& password, std::string display_name,
                                             models::Role role) {
  if (username.empty() || password.empty()) {
    return Result<models::User>::failure("invalid_value",
                                         "username and password must not be empty");
  }

  store::Account account;
  account.username = username;
  account.user.id = random_hex(16);
  account.user.role = role;
  // Not a ternary with a std::move in one arm: the other arm is a reference,
  // so the whole expression is one and nothing moves.
  if (display_name.empty()) {
    account.user.display_name = username;
  } else {
    account.user.display_name = std::move(display_name);
  }
  account.salt_hex = random_hex(16);
  account.password_hash_hex = derive_key_hex(password, account.salt_hex);

  const models::User user = account.user;
  if (auto failure = users_->create(std::move(account))) {
    return Result<models::User>::failure(*failure);
  }
  return user;
}

Result<Authenticator::Session> Authenticator::authenticate(const std::string& username,
                                                           const std::string& password,
                                                           Clock::time_point now) {
  const auto account = users_->find_by_username(username);
  if (!account.has_value()) {
    return Result<Session>::failure(unauthorized());
  }

  if (!constant_time_equals(account->password_hash_hex,
                            derive_key_hex(password, account->salt_hex))) {
    return Result<Session>::failure(unauthorized());
  }

  Session session;
  session.user = account->user;
  session.token = random_hex(32);
  session.expires_in_seconds = static_cast<int>(
      std::chrono::duration_cast<std::chrono::seconds>(options_.token_lifetime).count());

  tokens_.emplace(session.token,
                  Token{.user_id = account->user.id, .expires_at = now + options_.token_lifetime});
  return session;
}

Result<models::User> Authenticator::validate(const std::string& token, Clock::time_point now) {
  const auto it = tokens_.find(token);
  if (it == tokens_.end()) {
    return Result<models::User>::failure("unauthorized", "unknown session token");
  }
  if (now >= it->second.expires_at) {
    tokens_.erase(it);
    return Result<models::User>::failure("unauthorized", "session token has expired");
  }

  const auto account = users_->find_by_id(it->second.user_id);
  if (!account.has_value()) {
    tokens_.erase(it);
    return Result<models::User>::failure("unauthorized", "the account no longer exists");
  }
  return account->user;
}

Result<Authenticator::Credentials> Authenticator::derive(const std::string& password) const {
  if (password.empty()) {
    return Result<Credentials>::failure("invalid_value", "password must not be empty");
  }

  // A new salt as well as a new hash. Reusing the old one would mean two
  // passwords of the same account share a salt, which is exactly the property
  // a salt exists to destroy.
  Credentials credentials;
  credentials.salt_hex = random_hex(16);
  credentials.password_hash_hex = derive_key_hex(password, credentials.salt_hex);
  return credentials;
}

std::optional<Error> Authenticator::set_password(const std::string& user_id,
                                                 const std::string& password) {
  auto credentials = derive(password);
  if (!credentials) {
    return credentials.error();
  }

  auto account = users_->find_by_id(user_id);
  if (!account.has_value()) {
    return Error{.code = "user_not_found", .message = "no such account"};
  }

  account->salt_hex = std::move(credentials).take().salt_hex;
  account->password_hash_hex = derive_key_hex(password, account->salt_hex);
  return users_->update(*account);
}

void Authenticator::revoke_tokens_of(const std::string& user_id) {
  for (auto it = tokens_.begin(); it != tokens_.end();) {
    it = (it->second.user_id == user_id) ? tokens_.erase(it) : std::next(it);
  }
}

void Authenticator::expire_tokens(Clock::time_point now) {
  for (auto it = tokens_.begin(); it != tokens_.end();) {
    it = (now >= it->second.expires_at) ? tokens_.erase(it) : std::next(it);
  }
}

}  // namespace dv::server
