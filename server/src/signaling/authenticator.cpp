#include "signaling/authenticator.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/rand.h>

namespace dv::server {
namespace {

std::string to_hex(const unsigned char* data, std::size_t size) {
  static constexpr char kDigits[] = "0123456789abcdef";
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

std::string sha256_hex(const std::string& input) {
  std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
  unsigned int digest_size = 0;
  if (EVP_Digest(input.data(), input.size(), digest.data(), &digest_size, EVP_sha256(), nullptr) !=
      1) {
    throw std::runtime_error("SHA-256 computation failed");
  }
  return to_hex(digest.data(), digest_size);
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
  return Error{"unauthorized", "invalid username or password"};
}

}  // namespace

Authenticator::Authenticator() : Authenticator(Options{}) {}

Authenticator::Authenticator(Options options) : options_(options) {}

Result<models::User> Authenticator::add_user(const std::string& username,
                                             const std::string& password,
                                             std::string display_name) {
  if (username.empty() || password.empty()) {
    return Result<models::User>::failure("invalid_value",
                                         "username and password must not be empty");
  }
  if (accounts_.contains(username)) {
    return Result<models::User>::failure("user_exists", "username is already taken");
  }

  Account account;
  account.user.id = random_hex(16);
  account.user.display_name = display_name.empty() ? username : std::move(display_name);
  account.salt_hex = random_hex(16);
  account.password_hash_hex = sha256_hex(account.salt_hex + password);

  const models::User user = account.user;
  accounts_.emplace(username, std::move(account));
  return user;
}

Result<Authenticator::Session> Authenticator::authenticate(const std::string& username,
                                                           const std::string& password,
                                                           Clock::time_point now) {
  const auto it = accounts_.find(username);
  if (it == accounts_.end()) {
    return Result<Session>::failure(unauthorized());
  }

  const Account& account = it->second;
  if (!constant_time_equals(account.password_hash_hex, sha256_hex(account.salt_hex + password))) {
    return Result<Session>::failure(unauthorized());
  }

  Session session;
  session.user = account.user;
  session.token = random_hex(32);
  session.expires_in_seconds = static_cast<int>(
      std::chrono::duration_cast<std::chrono::seconds>(options_.token_lifetime).count());

  tokens_.emplace(session.token, Token{account.user.id, now + options_.token_lifetime});
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

  const std::string& user_id = it->second.user_id;
  const auto account = std::find_if(accounts_.begin(), accounts_.end(), [&](const auto& entry) {
    return entry.second.user.id == user_id;
  });
  if (account == accounts_.end()) {
    tokens_.erase(it);
    return Result<models::User>::failure("unauthorized", "the account no longer exists");
  }
  return account->second.user;
}

void Authenticator::expire_tokens(Clock::time_point now) {
  for (auto it = tokens_.begin(); it != tokens_.end();) {
    it = (now >= it->second.expires_at) ? tokens_.erase(it) : std::next(it);
  }
}

}  // namespace dv::server
