#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <dv/models/user.hpp>
#include <dv/protocol/message.hpp>

#include "signaling/hub.hpp"
#include "store/memory_store.hpp"
#include "store/session_store.hpp"

namespace {

using namespace std::chrono_literals;
using dv::models::Role;
using dv::protocol::Message;
using dv::server::ConnectionId;
using dv::server::Hub;
using dv::server::Outgoing;
using dv::server::store::SessionRecord;

namespace proto = dv::protocol;

/// The Hub with the session store held outside it.
///
/// Everything asserted here is about what a *second program* would read out of
/// that store, because that is the only reason the store exists: the server
/// itself is holding the sockets and never asks. See store::SessionStore.
class HubPresenceTest : public ::testing::Test {
 protected:
  HubPresenceTest() : hub_(Hub::Options{.room_id_seed = 4321U, .sessions = &sessions_}) {}

  static std::string address_of(ConnectionId id) { return "203.0.113." + std::to_string(id); }

  ConnectionId connect() {
    const ConnectionId id = next_connection_++;
    hub_.on_connect(id, address_of(id), now_);
    return id;
  }

  std::vector<Outgoing> send(ConnectionId connection, const Message& message) {
    return hub_.on_message(connection, proto::serialize(message), now_);
  }

  std::pair<ConnectionId, dv::models::User> login(const std::string& username, Role role) {
    const auto registered = hub_.authenticator().add_user(username, "password", username, role);
    EXPECT_TRUE(registered.ok()) << registered.error().message;
    return sign_in(username);
  }

  std::pair<ConnectionId, dv::models::User> sign_in(const std::string& username) {
    const ConnectionId connection = connect();
    const auto out = send(connection, proto::Authenticate{username, "password"});
    const auto authenticated = find<proto::Authenticated>(out, connection);
    EXPECT_TRUE(authenticated.has_value());
    return {connection, authenticated ? authenticated->user : dv::models::User{}};
  }

  template <typename T>
  static std::optional<T> find(const std::vector<Outgoing>& out, ConnectionId connection) {
    for (const Outgoing& outgoing : out) {
      if (outgoing.connection == connection && std::holds_alternative<T>(outgoing.message)) {
        return std::get<T>(outgoing.message);
      }
    }
    return std::nullopt;
  }

  /// The open session of one account, if the store holds one.
  [[nodiscard]] std::optional<SessionRecord> session_of(const std::string& user_id) const {
    for (const SessionRecord& record : sessions_.list_open()) {
      if (record.user_id == user_id) {
        return record;
      }
    }
    return std::nullopt;
  }

  dv::server::store::MemorySessionStore sessions_;
  Hub hub_;
  ConnectionId next_connection_ = 1;
  Hub::Clock::time_point now_ = Hub::Clock::now();
};

TEST_F(HubPresenceTest, ASocketThatNeverSaidWhoItIsIsNobody) {
  const ConnectionId connection = connect();
  // Presence is about accounts. A connection that has not authenticated is not
  // somebody who is online, it is a socket.
  EXPECT_TRUE(sessions_.list_open().empty());

  (void)hub_.on_disconnect(connection, now_);
  EXPECT_TRUE(sessions_.list_open().empty());
}

TEST_F(HubPresenceTest, SigningInRecordsTheAccountAndWhereItCameFrom) {
  const auto [connection, user] = login("ana", Role::User);

  const auto record = session_of(user.id);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->ip, address_of(connection));
  EXPECT_GT(record->connected_at, 0);
  EXPECT_EQ(record->last_seen_at, record->connected_at);
  EXPECT_TRUE(record->open());
}

TEST_F(HubPresenceTest, AFailedSignInRecordsNothing) {
  const auto registered = hub_.authenticator().add_user("ana", "password", "ana");
  ASSERT_TRUE(registered.ok());

  const ConnectionId connection = connect();
  const auto out = send(connection, proto::Authenticate{"ana", "wrong"});
  ASSERT_TRUE(find<proto::ErrorMessage>(out, connection).has_value());
  EXPECT_TRUE(sessions_.list_open().empty());
}

TEST_F(HubPresenceTest, TheSocketClosingEndsTheSession) {
  const auto [connection, user] = login("ana", Role::User);
  ASSERT_TRUE(session_of(user.id).has_value());

  (void)hub_.on_disconnect(connection, now_);
  EXPECT_FALSE(session_of(user.id).has_value());
  EXPECT_TRUE(sessions_.list_open().empty());
}

TEST_F(HubPresenceTest, ASecondSignInLeavesExactlyOneOpenSession) {
  const auto [first, user] = login("ana", Role::User);
  const auto first_record = session_of(user.id);
  ASSERT_TRUE(first_record.has_value());

  const auto [second, again] = sign_in("ana");
  const auto second_record = session_of(user.id);
  ASSERT_TRUE(second_record.has_value());

  // One account, one open row, and the row is the new socket's rather than the
  // one it displaced.
  EXPECT_EQ(sessions_.list_open().size(), 1U);
  EXPECT_NE(second_record->id, first_record->id);
  EXPECT_EQ(second_record->ip, address_of(second));

  // And the old socket closing afterwards does not take the new row with it.
  (void)hub_.on_disconnect(first, now_);
  ASSERT_TRUE(session_of(user.id).has_value());
  EXPECT_EQ(session_of(user.id)->id, second_record->id);
}

TEST_F(HubPresenceTest, TheHeartbeatSaysEverybodyIsStillThere) {
  const auto [connection, user] = login("ana", Role::User);
  ASSERT_TRUE(session_of(user.id).has_value());

  std::vector<ConnectionId> timed_out;
  (void)hub_.tick(now_ + 6s, timed_out);
  EXPECT_TRUE(timed_out.empty());

  // The store stamps a wall clock and this test runs inside one second, so the
  // number cannot be asserted to have moved - see test_session_store.cpp,
  // which pins the times and does exactly that. What is asserted here is what
  // the Hub is responsible for: the session is still open and still theirs.
  const auto record = session_of(user.id);
  ASSERT_TRUE(record.has_value());
  EXPECT_GE(record->last_seen_at, record->connected_at);
}

TEST_F(HubPresenceTest, AConnectionThisPassGaveUpOnIsNotReportedAsSeen) {
  const auto [connection, user] = login("ana", Role::User);

  std::vector<ConnectionId> timed_out;
  (void)hub_.tick(now_ + 20s, timed_out);
  ASSERT_EQ(timed_out.size(), 1U);
  EXPECT_EQ(timed_out.front(), connection);

  // The session is still open here on purpose: the Hub only reports the
  // timeout, and the transport closes the socket, which is what runs
  // on_disconnect and ends the row. Saying it was seen just now would have
  // kept it looking present for another interval.
  (void)hub_.on_disconnect(connection, now_ + 20s);
  EXPECT_TRUE(sessions_.list_open().empty());
}

TEST_F(HubPresenceTest, BanningSomebodyEndsTheirSession) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);
  ASSERT_TRUE(session_of(user.id).has_value());

  proto::RestrictUser ban;
  ban.user_id = user.id;
  ban.banned = true;
  (void)send(admin_connection, ban);

  // The socket is still open - a client that is hung up on cannot be told why
  // - but it is nobody now, and a presence report that still listed them would
  // be saying a banned account is on the platform.
  EXPECT_FALSE(session_of(user.id).has_value());
  EXPECT_TRUE(session_of(admin.id).has_value());
}

TEST_F(HubPresenceTest, DeletingAnAccountEndsItsSession) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);
  ASSERT_TRUE(session_of(user.id).has_value());

  (void)send(admin_connection, proto::DeleteUser{user.id});
  EXPECT_FALSE(session_of(user.id).has_value());
}

TEST_F(HubPresenceTest, ChangingAPasswordEndsTheSessionItWasChangedFrom) {
  const auto [connection, user] = login("ana", Role::User);
  ASSERT_TRUE(session_of(user.id).has_value());

  const auto out = send(connection, proto::ChangePassword{"password", "another"});
  ASSERT_TRUE(find<proto::PasswordChanged>(out, connection).has_value());

  // Every session of the account is revoked, this one included, so there is
  // nobody online under it until somebody signs in again.
  EXPECT_FALSE(session_of(user.id).has_value());
}

TEST_F(HubPresenceTest, TwoAccountsAreTwoRows) {
  const auto [ana_connection, ana] = login("ana", Role::User);
  const auto [bruno_connection, bruno] = login("bruno", Role::User);

  EXPECT_EQ(sessions_.list_open().size(), 2U);
  ASSERT_TRUE(session_of(ana.id).has_value());
  ASSERT_TRUE(session_of(bruno.id).has_value());
  EXPECT_NE(session_of(ana.id)->ip, session_of(bruno.id)->ip);
}

}  // namespace
