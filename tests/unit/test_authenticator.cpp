#include <chrono>
#include <string>

#include <gtest/gtest.h>

#include "signaling/authenticator.hpp"

namespace {

using dv::server::Authenticator;
using Clock = Authenticator::Clock;

const Clock::time_point kNow{};

TEST(Authenticator, RegistersAUserWithAnIdentity) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "senha", "Ana");
  ASSERT_TRUE(user.ok()) << user.error().message;
  EXPECT_FALSE(user.value().id.empty());
  EXPECT_EQ(user.value().display_name, "Ana");
}

TEST(Authenticator, FallsBackToTheUsernameAsDisplayName) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "senha", "");
  ASSERT_TRUE(user.ok());
  EXPECT_EQ(user.value().display_name, "ana");
}

TEST(Authenticator, GivesEveryUserADistinctIdentifier) {
  Authenticator auth;
  const auto first = auth.add_user("ana", "senha", "");
  const auto second = auth.add_user("bruno", "senha", "");
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_NE(first.value().id, second.value().id);
}

TEST(Authenticator, RejectsADuplicateUsername) {
  Authenticator auth;
  EXPECT_TRUE(auth.add_user("ana", "senha", "").ok());
  const auto duplicate = auth.add_user("ana", "outra", "");
  ASSERT_FALSE(duplicate.ok());
  EXPECT_EQ(duplicate.error().code, "user_exists");
}

TEST(Authenticator, RejectsEmptyCredentials) {
  Authenticator auth;
  EXPECT_EQ(auth.add_user("", "senha", "").error().code, "invalid_value");
  EXPECT_EQ(auth.add_user("ana", "", "").error().code, "invalid_value");
}

TEST(Authenticator, AuthenticatesWithTheRightPassword) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "senha", "Ana");
  ASSERT_TRUE(user.ok());

  const auto session = auth.authenticate("ana", "senha", kNow);
  ASSERT_TRUE(session.ok()) << session.error().message;
  EXPECT_EQ(session.value().user.id, user.value().id);
  EXPECT_FALSE(session.value().token.empty());
  EXPECT_GT(session.value().expires_in_seconds, 0);
}

TEST(Authenticator, RejectsAWrongPassword) {
  Authenticator auth;
  EXPECT_TRUE(auth.add_user("ana", "senha", "").ok());
  const auto session = auth.authenticate("ana", "errada", kNow);
  ASSERT_FALSE(session.ok());
  EXPECT_EQ(session.error().code, "unauthorized");
}

TEST(Authenticator, AnUnknownUserFailsIdenticallyToAWrongPassword) {
  Authenticator auth;
  EXPECT_TRUE(auth.add_user("ana", "senha", "").ok());
  const auto wrong_password = auth.authenticate("ana", "errada", kNow);
  const auto unknown_user = auth.authenticate("ninguem", "errada", kNow);

  // Identical replies, so the protocol cannot be used to enumerate accounts.
  EXPECT_EQ(wrong_password.error().code, unknown_user.error().code);
  EXPECT_EQ(wrong_password.error().message, unknown_user.error().message);
}

TEST(Authenticator, IssuesADistinctTokenPerLogin) {
  Authenticator auth;
  EXPECT_TRUE(auth.add_user("ana", "senha", "").ok());
  const auto first = auth.authenticate("ana", "senha", kNow);
  const auto second = auth.authenticate("ana", "senha", kNow);
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  EXPECT_NE(first.value().token, second.value().token);
}

TEST(Authenticator, ValidatesAFreshToken) {
  Authenticator auth;
  const auto user = auth.add_user("ana", "senha", "Ana");
  const auto session = auth.authenticate("ana", "senha", kNow);
  ASSERT_TRUE(session.ok());

  const auto validated = auth.validate(session.value().token, kNow);
  ASSERT_TRUE(validated.ok()) << validated.error().message;
  EXPECT_EQ(validated.value().id, user.value().id);
}

TEST(Authenticator, RejectsAnUnknownToken) {
  Authenticator auth;
  const auto validated = auth.validate("nao-existe", kNow);
  ASSERT_FALSE(validated.ok());
  EXPECT_EQ(validated.error().code, "unauthorized");
}

TEST(Authenticator, RejectsAnExpiredToken) {
  Authenticator auth(Authenticator::Options{std::chrono::seconds(60)});
  EXPECT_TRUE(auth.add_user("ana", "senha", "").ok());
  const auto session = auth.authenticate("ana", "senha", kNow);
  ASSERT_TRUE(session.ok());

  EXPECT_TRUE(auth.validate(session.value().token, kNow + std::chrono::seconds(59)).ok());
  const auto expired = auth.validate(session.value().token, kNow + std::chrono::seconds(61));
  ASSERT_FALSE(expired.ok());
  EXPECT_EQ(expired.error().code, "unauthorized");
}

TEST(Authenticator, ExpiringDropsStaleTokens) {
  Authenticator auth(Authenticator::Options{std::chrono::seconds(60)});
  EXPECT_TRUE(auth.add_user("ana", "senha", "").ok());
  EXPECT_TRUE(auth.authenticate("ana", "senha", kNow).ok());
  EXPECT_EQ(auth.active_token_count(), 1u);

  auth.expire_tokens(kNow + std::chrono::seconds(30));
  EXPECT_EQ(auth.active_token_count(), 1u);

  auth.expire_tokens(kNow + std::chrono::seconds(61));
  EXPECT_EQ(auth.active_token_count(), 0u);
}

TEST(Authenticator, TokensAreLongEnoughToResistGuessing) {
  Authenticator auth;
  EXPECT_TRUE(auth.add_user("ana", "senha", "").ok());
  const auto session = auth.authenticate("ana", "senha", kNow);
  ASSERT_TRUE(session.ok());
  // 32 random bytes, hex encoded.
  EXPECT_EQ(session.value().token.size(), 64u);
}

}  // namespace
