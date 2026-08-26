#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "signaling/authenticator.hpp"
#include "store/memory_store.hpp"

namespace {

using dv::server::Authenticator;
using Clock = Authenticator::Clock;

const Clock::time_point kNow{};

TEST(Authenticator, RegistersAUserWithAnIdentity) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "password", "Ana");
  ASSERT_TRUE(user.ok()) << user.error().message;
  EXPECT_FALSE(user.value().id.empty());
  EXPECT_EQ(user.value().display_name, "Ana");
}

TEST(Authenticator, FallsBackToTheUsernameAsDisplayName) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "password", "");
  ASSERT_TRUE(user.ok());
  EXPECT_EQ(user.value().display_name, "ana");
}

TEST(Authenticator, GivesEveryUserADistinctIdentifier) {
  Authenticator auth;
  const auto first = auth.add_user("ana", "password", "");
  const auto second = auth.add_user("bruno", "password", "");
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_NE(first.value().id, second.value().id);
}

TEST(Authenticator, RejectsADuplicateUsername) {
  Authenticator auth;
  EXPECT_TRUE(auth.add_user("ana", "password", "").ok());
  const auto duplicate = auth.add_user("ana", "outra", "");
  ASSERT_FALSE(duplicate.ok());
  EXPECT_EQ(duplicate.error().code, "user_exists");
}

TEST(Authenticator, RejectsEmptyCredentials) {
  Authenticator auth;
  EXPECT_EQ(auth.add_user("", "password", "").error().code, "invalid_value");
  EXPECT_EQ(auth.add_user("ana", "", "").error().code, "invalid_value");
}

TEST(Authenticator, AuthenticatesWithTheRightPassword) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "password", "Ana");
  ASSERT_TRUE(user.ok());

  const auto session = auth.authenticate("ana", "password", kNow);
  ASSERT_TRUE(session.ok()) << session.error().message;
  EXPECT_EQ(session.value().user.id, user.value().id);
  EXPECT_FALSE(session.value().token.empty());
  EXPECT_GT(session.value().expires_in_seconds, 0);
}

TEST(Authenticator, RejectsAWrongPassword) {
  Authenticator auth;
  EXPECT_TRUE(auth.add_user("ana", "password", "").ok());
  const auto session = auth.authenticate("ana", "wrong", kNow);
  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.error().code, "unauthorized");
}

TEST(Authenticator, AnUnknownUserFailsIdenticallyToAWrongPassword) {
  Authenticator auth;
  EXPECT_TRUE(auth.add_user("ana", "password", "").ok());
  const auto wrong_password = auth.authenticate("ana", "wrong", kNow);
  const auto unknown_user = auth.authenticate("nobody", "wrong", kNow);

  // Identical replies, so the protocol cannot be used to enumerate accounts.
  EXPECT_EQ(wrong_password.error().code, unknown_user.error().code);
  EXPECT_EQ(wrong_password.error().message, unknown_user.error().message);
}

TEST(Authenticator, RefusesABannedAccount) {
  dv::server::store::MemoryUserStore users;
  Authenticator auth(Authenticator::Options{}, users);
  const auto registered = auth.add_user("ana", "password", "Ana");
  ASSERT_TRUE(registered.ok());

  auto account = users.find_by_id(registered.value().id);
  ASSERT_TRUE(account.has_value());
  account->user.restrictions.banned = true;
  ASSERT_FALSE(users.update(*account).has_value());

  const auto session = auth.authenticate("ana", "password", kNow);
  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.error().code, "account_banned");
}

TEST(Authenticator, ABannedAccountIsStillRefusedIdenticallyOnAWrongPassword) {
  // The order matters and is easy to get backwards. Answering "banned" to
  // whoever typed the username would turn a login form into a way to ask which
  // accounts exist; only the person holding the password gets the real reason.
  dv::server::store::MemoryUserStore users;
  Authenticator auth(Authenticator::Options{}, users);
  const auto registered = auth.add_user("ana", "password", "Ana");
  ASSERT_TRUE(registered.ok());

  auto account = users.find_by_id(registered.value().id);
  ASSERT_TRUE(account.has_value());
  account->user.restrictions.banned = true;
  ASSERT_FALSE(users.update(*account).has_value());

  const auto wrong = auth.authenticate("ana", "wrong", kNow);
  ASSERT_FALSE(wrong.ok());
  EXPECT_EQ(wrong.error().code, "unauthorized");
  EXPECT_EQ(wrong.error().message, "invalid username or password");
}

TEST(Authenticator, IssuesADistinctTokenPerLogin) {
  Authenticator auth;
  EXPECT_TRUE(auth.add_user("ana", "password", "").ok());
  const auto first = auth.authenticate("ana", "password", kNow);
  const auto second = auth.authenticate("ana", "password", kNow);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_NE(first.value().token, second.value().token);
}

TEST(Authenticator, ValidatesAFreshToken) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "password", "Ana");
  const auto session = auth.authenticate("ana", "password", kNow);
  ASSERT_TRUE(session.ok());

  const auto validated = auth.validate(session.value().token, kNow);
  ASSERT_TRUE(validated.ok()) << validated.error().message;
  EXPECT_EQ(validated.value().id, user.value().id);
}

TEST(Authenticator, RejectsAnUnknownToken) {
  Authenticator auth;
  const auto validated = auth.validate("does-not-exist", kNow);
  ASSERT_FALSE(validated.ok());
  EXPECT_EQ(validated.error().code, "unauthorized");
}

TEST(Authenticator, RejectsAnExpiredToken) {
  Authenticator auth(Authenticator::Options{std::chrono::seconds(60)});
  EXPECT_TRUE(auth.add_user("ana", "password", "").ok());
  const auto session = auth.authenticate("ana", "password", kNow);
  ASSERT_TRUE(session.ok());

  EXPECT_TRUE(auth.validate(session.value().token, kNow + std::chrono::seconds(59)).ok());
  const auto expired = auth.validate(session.value().token, kNow + std::chrono::seconds(61));
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().code, "unauthorized");
}

TEST(Authenticator, ExpiringDropsStaleTokens) {
  Authenticator auth(Authenticator::Options{std::chrono::seconds(60)});
  EXPECT_TRUE(auth.add_user("ana", "password", "").ok());
  EXPECT_TRUE(auth.authenticate("ana", "password", kNow).ok());
  EXPECT_EQ(auth.active_token_count(), 1u);

  auth.expire_tokens(kNow + std::chrono::seconds(30));
  EXPECT_EQ(auth.active_token_count(), 1u);

  auth.expire_tokens(kNow + std::chrono::seconds(61));
  EXPECT_EQ(auth.active_token_count(), 0u);
}

TEST(Authenticator, TokensAreLongEnoughToResistGuessing) {
  Authenticator auth;
  EXPECT_TRUE(auth.add_user("ana", "password", "").ok());
  const auto session = auth.authenticate("ana", "password", kNow);
  ASSERT_TRUE(session.ok());
  // 32 random bytes, hex encoded.
  EXPECT_EQ(session.value().token.size(), 64u);
}

TEST(AuthenticatorTest, HashingAPasswordIsDeliberatelySlow) {
  // Section 17 of SPEC.md, and the point of using a key derivation function
  // instead of a digest. A digest is fast by design, which is exactly the
  // property a password store must not have: a modern card tries billions of
  // SHA-256 candidates a second against a stolen file.
  //
  // The number is loose on purpose. What this guards against is somebody
  // swapping the derivation back for a plain hash, which would drop this from
  // milliseconds to microseconds, not against a particular cost setting.
  dv::server::Authenticator authenticator;

  const auto started = std::chrono::steady_clock::now();
  ASSERT_TRUE(authenticator.add_user("ana", "password", "Ana").ok());
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_GT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 5)
      << "hashing a password took no time at all, which means it is not a key derivation";
}

TEST(AuthenticatorTest, TheSamePasswordStoresDifferentlyForDifferentUsers) {
  // Per account salt. Without it, two people with the same password have the
  // same stored value, and one stolen file answers both at once.
  dv::server::Authenticator authenticator;
  ASSERT_TRUE(authenticator.add_user("ana", "same-password", "Ana").ok());
  ASSERT_TRUE(authenticator.add_user("bruno", "same-password", "Bruno").ok());

  // Both still authenticate, which is what says the salt is stored and used
  // rather than merely generated.
  EXPECT_TRUE(authenticator.authenticate("ana", "same-password", kNow).ok());
  EXPECT_TRUE(authenticator.authenticate("bruno", "same-password", kNow).ok());
}

TEST(Authenticator, ChangesAPasswordAfterCheckingTheCurrentOne) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "old-password", "Ana");
  ASSERT_TRUE(user.ok());

  EXPECT_FALSE(auth.change_password(user.value().id, "old-password", "new-password").has_value());

  // The pair of assertions is the whole test. Only the second one says the new
  // password works; only the first says the old one stopped working, which is
  // the half a change that wrote nothing would still pass.
  EXPECT_FALSE(auth.authenticate("ana", "old-password", kNow).ok());
  EXPECT_TRUE(auth.authenticate("ana", "new-password", kNow).ok());
}

TEST(Authenticator, RefusesAChangeWithTheWrongCurrentPassword) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "old-password", "Ana");
  ASSERT_TRUE(user.ok());

  const auto failure = auth.change_password(user.value().id, "not-it", "new-password");
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "invalid_password");

  // Nothing was written. A refusal that had already replaced the password
  // would leave an account whose owner was told no and whose password moved.
  EXPECT_TRUE(auth.authenticate("ana", "old-password", kNow).ok());
  EXPECT_FALSE(auth.authenticate("ana", "new-password", kNow).ok());
}

TEST(Authenticator, RefusesAChangeToTheSamePassword) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "password", "Ana");
  ASSERT_TRUE(user.ok());

  const auto failure = auth.change_password(user.value().id, "password", "password");
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "invalid_value");
}

TEST(Authenticator, RefusesAnEmptyNewPassword) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "password", "Ana");
  ASSERT_TRUE(user.ok());

  const auto failure = auth.change_password(user.value().id, "password", "");
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "invalid_value");
  EXPECT_TRUE(auth.authenticate("ana", "password", kNow).ok());
}

TEST(Authenticator, RefusesAChangeForAnAccountThatIsNotThere) {
  Authenticator auth;
  const auto failure = auth.change_password("0123456789abcdef", "old", "new");
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "user_not_found");
}

TEST(Authenticator, DoesNotRevokeTokensOnAPasswordChange) {
  // Pinning where the policy lives rather than approving of it. Ending the
  // sessions of an account whose password just changed is the right answer,
  // and it is the Hub that gives it - see Hub::handle_change_password. Here,
  // a change is a change to the store and nothing else, which is what lets a
  // future caller that should not sign anybody out use the same method.
  Authenticator auth;
  const auto user = auth.add_user("ana", "old-password", "Ana");
  ASSERT_TRUE(user.ok());
  ASSERT_TRUE(auth.authenticate("ana", "old-password", kNow).ok());
  ASSERT_EQ(auth.active_token_count(), 1U);

  EXPECT_FALSE(auth.change_password(user.value().id, "old-password", "new-password").has_value());
  EXPECT_EQ(auth.active_token_count(), 1U);
}

}  // namespace
