#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <dv/models/notice.hpp>
#include <dv/models/user.hpp>
#include <dv/protocol/message.hpp>

#include "signaling/hub.hpp"
#include "store/memory_store.hpp"

namespace {

using namespace std::chrono_literals;
using dv::models::Role;
using dv::protocol::Message;
using dv::server::ConnectionId;
using dv::server::Hub;
using dv::server::Outgoing;

namespace proto = dv::protocol;

/// The Hub with the notice and audit stores held outside it, so that a test
/// can ask what was actually written rather than inferring it from the
/// messages that went out. Everything else is left to the Hub's own defaults,
/// which is the server without a database.
class HubNoticeTest : public ::testing::Test {
 protected:
  HubNoticeTest()
      : hub_(Hub::Options{.room_id_seed = 4321U, .notices = &notices_, .audit = &audit_}) {}

  ConnectionId connect() {
    const ConnectionId id = next_connection_++;
    hub_.on_connect(id, "203.0.113." + std::to_string(id), now_);
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

  /// A fresh connection for an account that already exists, which is what the
  /// tests about delivery at the next sign-in need.
  std::pair<ConnectionId, dv::models::User> sign_in(const std::string& username) {
    const ConnectionId connection = connect();
    const auto out = send(connection, proto::Authenticate{username, "password"});
    const auto authenticated = find<proto::Authenticated>(out, connection);
    EXPECT_TRUE(authenticated.has_value());
    return {connection, authenticated ? authenticated->user : dv::models::User{}};
  }

  /// Everything of type T addressed at `connection`, in the order it was
  /// produced. `find` answers the first one; a login that owes somebody three
  /// notices produces three.
  template <typename T>
  static std::vector<T> all(const std::vector<Outgoing>& out, ConnectionId connection) {
    std::vector<T> found;
    for (const Outgoing& outgoing : out) {
      if (outgoing.connection == connection && std::holds_alternative<T>(outgoing.message)) {
        found.push_back(std::get<T>(outgoing.message));
      }
    }
    return found;
  }

  template <typename T>
  static std::optional<T> find(const std::vector<Outgoing>& out, ConnectionId connection) {
    const std::vector<T> found = all<T>(out, connection);
    return found.empty() ? std::nullopt : std::optional<T>{found.front()};
  }

  /// Notices to anybody at all, for the tests that say nobody was given one.
  static std::size_t count_notices(const std::vector<Outgoing>& out) {
    std::size_t total = 0;
    for (const Outgoing& outgoing : out) {
      if (std::holds_alternative<proto::Notice>(outgoing.message)) {
        ++total;
      }
    }
    return total;
  }

  /// What the audit log holds, newest first, as it is read back.
  [[nodiscard]] std::vector<dv::models::AuditEntry> entries() const { return audit_.list(0, {}); }

  /// One turn of the loop the server runs on its own. The only thing that
  /// happens to a Hub nobody is talking to, and therefore the only way a
  /// notice written by another program can reach somebody already here.
  std::vector<Outgoing> tick() {
    std::vector<ConnectionId> timed_out;
    return hub_.tick(now_, timed_out);
  }

  /// What tools/dbadmin does: the document, and no message. No sender, for
  /// the reason a restriction written the same way names nobody - a shell is
  /// not an account.
  dv::models::Notice write_behind_the_back(const std::string& user_id, const std::string& text) {
    auto written = notices_.append(dv::models::Notice{.user_id = user_id, .text = text});
    EXPECT_TRUE(written.ok()) << written.error().message;
    return written.ok() ? std::move(written).take() : dv::models::Notice{};
  }

  dv::server::store::MemoryNoticeStore notices_;
  dv::server::store::MemoryAuditLog audit_;
  Hub hub_;
  ConnectionId next_connection_ = 1;
  Hub::Clock::time_point now_ = Hub::Clock::now();
};

// --- the gate ----------------------------------------------------------------

TEST_F(HubNoticeTest, AnOrdinaryUserCannotSendOne) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);

  const auto out = send(user_connection, proto::SendNotice{admin.id, "you are demoted"});
  const auto refused = find<proto::ErrorMessage>(out, user_connection);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "forbidden");

  EXPECT_TRUE(notices_.pending_for(admin.id).empty());
}

TEST_F(HubNoticeTest, AnOrdinaryUserMayAcknowledgeTheirOwn) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);

  const auto sent = send(admin_connection, proto::SendNotice{user.id, "the meeting moved"});
  const auto delivered = find<proto::Notice>(sent, user_connection);
  ASSERT_TRUE(delivered.has_value());

  const auto out = send(user_connection, proto::AcknowledgeNotice{delivered->notice.id});
  EXPECT_FALSE(find<proto::ErrorMessage>(out, user_connection).has_value());
  EXPECT_TRUE(notices_.pending_for(user.id).empty());
}

// --- sending -----------------------------------------------------------------

TEST_F(HubNoticeTest, ItReachesTheRecipientAndComesBackToTheSender) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);

  const auto out = send(admin_connection, proto::SendNotice{user.id, "the meeting moved"});

  const auto delivered = find<proto::Notice>(out, user_connection);
  ASSERT_TRUE(delivered.has_value());
  EXPECT_EQ(delivered->notice.text, "the meeting moved");
  EXPECT_EQ(delivered->notice.user_id, user.id);
  EXPECT_EQ(delivered->notice.from_user_id, admin.id);
  EXPECT_EQ(delivered->notice.from_display_name, "ana");
  EXPECT_FALSE(delivered->notice.id.empty());
  EXPECT_GT(delivered->notice.created_at, 0);

  // The administrator's copy is the same row, which is what makes it a
  // confirmation rather than an echo: it carries the identifier the store
  // assigned.
  const auto confirmed = find<proto::Notice>(out, admin_connection);
  ASSERT_TRUE(confirmed.has_value());
  EXPECT_EQ(confirmed->notice.id, delivered->notice.id);
}

TEST_F(HubNoticeTest, ItIsWrittenBeforeItIsDelivered) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);

  const auto out = send(admin_connection, proto::SendNotice{user.id, "read this"});
  const auto delivered = find<proto::Notice>(out, user_connection);
  ASSERT_TRUE(delivered.has_value());

  const auto pending = notices_.pending_for(user.id);
  ASSERT_EQ(pending.size(), 1U);
  // The identifier the recipient was given is the identifier of the row that
  // exists, or there would be nothing for them to acknowledge.
  EXPECT_EQ(pending.front().id, delivered->notice.id);
}

TEST_F(HubNoticeTest, TheTextIsTrimmed) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);

  const auto out = send(admin_connection, proto::SendNotice{user.id, "  padded  "});
  const auto delivered = find<proto::Notice>(out, user_connection);
  ASSERT_TRUE(delivered.has_value());
  EXPECT_EQ(delivered->notice.text, "padded");
}

TEST_F(HubNoticeTest, AnEmptyOrOversizedNoticeIsRefused) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);

  for (const std::string& text :
       {std::string{}, std::string("   "), std::string(dv::models::kMaxNoticeTextBytes + 1, 'a')}) {
    const auto out = send(admin_connection, proto::SendNotice{user.id, text});
    const auto refused = find<proto::ErrorMessage>(out, admin_connection);
    ASSERT_TRUE(refused.has_value()) << "accepted a notice of " << text.size() << " bytes";
    EXPECT_EQ(refused->code, "invalid_value");
    EXPECT_FALSE(find<proto::Notice>(out, user_connection).has_value());
  }

  EXPECT_TRUE(notices_.pending_for(user.id).empty());
}

TEST_F(HubNoticeTest, ANoticeToNobodyIsRefused) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);

  const auto out = send(admin_connection, proto::SendNotice{"nobody", "hello?"});
  const auto refused = find<proto::ErrorMessage>(out, admin_connection);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "user_not_found");
  EXPECT_TRUE(notices_.pending_for("nobody").empty());
}

TEST_F(HubNoticeTest, AnAdministratorMayWriteToThemselvesAndIsToldOnce) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);

  const auto out = send(admin_connection, proto::SendNotice{admin.id, "remember the backups"});
  // One copy and not two: the delivery and the confirmation are the same
  // message going to the same place.
  EXPECT_EQ(all<proto::Notice>(out, admin_connection).size(), 1U);
}

// --- delivery on the heartbeat, for the writer that cannot send ---------------

TEST_F(HubNoticeTest, OneWrittenElsewhereReachesSomebodyAlreadySignedIn) {
  const auto [user_connection, user] = login("bruno", Role::User);

  const dv::models::Notice written = write_behind_the_back(user.id, "please use a headset");
  const auto out = tick();

  const auto delivered = all<proto::Notice>(out, user_connection);
  ASSERT_EQ(delivered.size(), 1U);
  EXPECT_EQ(delivered.front().notice.id, written.id);
  EXPECT_EQ(delivered.front().notice.text, "please use a headset");
  // Exactly what was written, sender included: the client already says "an
  // administrator" for an empty name, so nothing is invented here.
  EXPECT_TRUE(delivered.front().notice.from_user_id.empty());
  EXPECT_TRUE(delivered.front().notice.from_display_name.empty());

  // Nothing recorded on this side. Whoever wrote it wrote the entry, and the
  // moment this server noticed is not a fact anybody is looking for.
  EXPECT_TRUE(entries().empty());
}

TEST_F(HubNoticeTest, OneDeliveredOnAHeartbeatIsNotDeliveredAgainOnTheNext) {
  const auto [user_connection, user] = login("bruno", Role::User);
  (void)write_behind_the_back(user.id, "read this");
  ASSERT_EQ(all<proto::Notice>(tick(), user_connection).size(), 1U);

  // Still pending: nobody has acknowledged it. A pass that handed over
  // whatever is pending would be a box on the screen every five seconds.
  ASSERT_EQ(notices_.pending_for(user.id).size(), 1U);
  for (int pass = 0; pass < 3; ++pass) {
    EXPECT_TRUE(all<proto::Notice>(tick(), user_connection).empty()) << "pass " << pass;
  }
}

TEST_F(HubNoticeTest, OneTheServerDeliveredItselfIsNotRepeatedByTheHeartbeat) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);

  const auto sent = send(admin_connection, proto::SendNotice{user.id, "the meeting moved"});
  ASSERT_EQ(all<proto::Notice>(sent, user_connection).size(), 1U);

  const auto out = tick();
  EXPECT_TRUE(all<proto::Notice>(out, user_connection).empty());
  EXPECT_TRUE(all<proto::Notice>(out, admin_connection).empty());
}

TEST_F(HubNoticeTest, OneHandedOverAtSignInIsNotRepeatedByTheHeartbeat) {
  const auto [first_connection, user] = login("bruno", Role::User);
  (void)hub_.on_disconnect(first_connection, now_);
  (void)write_behind_the_back(user.id, "while you were out");

  const auto [second_connection, again] = sign_in("bruno");
  ASSERT_EQ(again.id, user.id);
  const auto out = tick();
  EXPECT_TRUE(all<proto::Notice>(out, second_connection).empty());
}

TEST_F(HubNoticeTest, ANewSignInIsOwedWhatTheLastOneWasShown) {
  const auto [first_connection, user] = login("bruno", Role::User);
  (void)write_behind_the_back(user.id, "read this");
  ASSERT_EQ(all<proto::Notice>(tick(), first_connection).size(), 1U);

  // Shown and not answered, then gone. The next session is handed it again
  // at sign-in, as it is handed everything pending - what the last
  // connection remembered about deliveries went with that connection.
  (void)hub_.on_disconnect(first_connection, now_);
  const ConnectionId second_connection = connect();
  const auto out = send(second_connection, proto::Authenticate{"bruno", "password"});
  EXPECT_EQ(all<proto::Notice>(out, second_connection).size(), 1U);
}

TEST_F(HubNoticeTest, OneWrittenElsewhereForSomebodyAwayWaitsForTheirSignIn) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [first_connection, user] = login("bruno", Role::User);
  (void)hub_.on_disconnect(first_connection, now_);

  (void)write_behind_the_back(user.id, "while you were out");
  // Nobody to give it to, and nobody else is given it.
  EXPECT_EQ(count_notices(tick()), 0U);

  const ConnectionId second_connection = connect();
  const auto out = send(second_connection, proto::Authenticate{"bruno", "password"});
  ASSERT_EQ(all<proto::Notice>(out, second_connection).size(), 1U);
  EXPECT_EQ(all<proto::Notice>(out, second_connection).front().notice.text, "while you were out");
}

// --- delivery to somebody who was not there ----------------------------------

TEST_F(HubNoticeTest, ANoticeToSomebodyAwayIsKeptAndArrivesAtTheirNextSignIn) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [first_connection, user] = login("bruno", Role::User);

  (void)hub_.on_disconnect(first_connection, now_);

  const auto sent = send(admin_connection, proto::SendNotice{user.id, "while you were out"});
  // Nothing to deliver it on, and that is not a failure: this is the case the
  // whole store exists for. The administrator is still told it was written.
  EXPECT_FALSE(find<proto::ErrorMessage>(sent, admin_connection).has_value());
  ASSERT_TRUE(find<proto::Notice>(sent, admin_connection).has_value());

  const ConnectionId second_connection = connect();
  const auto out = send(second_connection, proto::Authenticate{"bruno", "password"});

  const auto delivered = all<proto::Notice>(out, second_connection);
  ASSERT_EQ(delivered.size(), 1U);
  EXPECT_EQ(delivered.front().notice.text, "while you were out");
}

TEST_F(HubNoticeTest, SignInHandsOverEverythingOutstandingOldestFirst) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [first_connection, user] = login("bruno", Role::User);
  (void)hub_.on_disconnect(first_connection, now_);

  (void)send(admin_connection, proto::SendNotice{user.id, "one"});
  (void)send(admin_connection, proto::SendNotice{user.id, "two"});

  const ConnectionId connection = connect();
  const auto out = send(connection, proto::Authenticate{"bruno", "password"});

  const auto delivered = all<proto::Notice>(out, connection);
  ASSERT_EQ(delivered.size(), 2U);
  EXPECT_EQ(delivered[0].notice.text, "one");
  EXPECT_EQ(delivered[1].notice.text, "two");

  // After `authenticated`, so a client is told which account it is before it
  // is told anything about that account.
  const auto& messages = out;
  bool seen_authenticated = false;
  for (const Outgoing& outgoing : messages) {
    if (std::holds_alternative<proto::Authenticated>(outgoing.message)) {
      seen_authenticated = true;
    }
    if (std::holds_alternative<proto::Notice>(outgoing.message)) {
      EXPECT_TRUE(seen_authenticated);
    }
  }
}

TEST_F(HubNoticeTest, OneThatWasShownAndNotAnsweredArrivesAgain) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [first_connection, user] = login("bruno", Role::User);

  (void)send(admin_connection, proto::SendNotice{user.id, "read this"});
  (void)hub_.on_disconnect(first_connection, now_);

  const ConnectionId connection = connect();
  const auto out = send(connection, proto::Authenticate{"bruno", "password"});
  ASSERT_EQ(all<proto::Notice>(out, connection).size(), 1U);
}

TEST_F(HubNoticeTest, OneThatWasAnsweredDoesNotComeBack) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [first_connection, user] = login("bruno", Role::User);

  const auto sent = send(admin_connection, proto::SendNotice{user.id, "read this"});
  const auto delivered = find<proto::Notice>(sent, first_connection);
  ASSERT_TRUE(delivered.has_value());
  (void)send(first_connection, proto::AcknowledgeNotice{delivered->notice.id});
  (void)hub_.on_disconnect(first_connection, now_);

  const ConnectionId connection = connect();
  const auto out = send(connection, proto::Authenticate{"bruno", "password"});
  EXPECT_TRUE(all<proto::Notice>(out, connection).empty());
}

// --- acknowledging -----------------------------------------------------------

TEST_F(HubNoticeTest, SomebodyElsesNoticeCannotBeAcknowledged) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);
  const auto [other_connection, other] = login("carla", Role::User);

  const auto sent = send(admin_connection, proto::SendNotice{user.id, "for bruno"});
  const auto delivered = find<proto::Notice>(sent, user_connection);
  ASSERT_TRUE(delivered.has_value());

  const auto out = send(other_connection, proto::AcknowledgeNotice{delivered->notice.id});
  const auto refused = find<proto::ErrorMessage>(out, other_connection);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "notice_not_found");

  // Still waiting for the person it was written to.
  EXPECT_EQ(notices_.pending_for(user.id).size(), 1U);
}

TEST_F(HubNoticeTest, AnIdentifierNobodyIssuedIsRefusedTheSameWay) {
  const auto [user_connection, user] = login("bruno", Role::User);

  const auto out = send(user_connection, proto::AcknowledgeNotice{"nothing"});
  const auto refused = find<proto::ErrorMessage>(out, user_connection);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "notice_not_found");
}

// --- the record --------------------------------------------------------------

TEST_F(HubNoticeTest, SendingAndAnsweringAreBothRecorded) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);

  const auto sent = send(admin_connection, proto::SendNotice{user.id, "the meeting moved"});
  const auto delivered = find<proto::Notice>(sent, user_connection);
  ASSERT_TRUE(delivered.has_value());
  (void)send(user_connection, proto::AcknowledgeNotice{delivered->notice.id});

  const auto log = entries();
  ASSERT_EQ(log.size(), 2U);

  // Newest first, so the answer comes back before the message.
  EXPECT_EQ(log[0].action, "acknowledge_notice");
  EXPECT_EQ(log[0].actor_id, user.id);
  EXPECT_EQ(log[0].actor_username, "bruno");
  EXPECT_EQ(log[0].target_id, user.id);
  EXPECT_NE(log[0].detail.find("notice=" + delivered->notice.id), std::string::npos);

  EXPECT_EQ(log[1].action, "send_notice");
  EXPECT_EQ(log[1].actor_id, admin.id);
  EXPECT_EQ(log[1].target_id, user.id);
  // The text is in the entry, because "an administrator sent a message" with
  // no message is a row nobody can act on.
  EXPECT_NE(log[1].detail.find("the meeting moved"), std::string::npos);
  EXPECT_NE(log[1].detail.find("notice=" + delivered->notice.id), std::string::npos);
}

TEST_F(HubNoticeTest, ARefusedNoticeIsNotRecorded) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);

  (void)send(admin_connection, proto::SendNotice{"nobody", "hello?"});
  (void)send(admin_connection, proto::SendNotice{admin.id, ""});
  EXPECT_TRUE(entries().empty());
}

// --- an account going away ---------------------------------------------------

TEST_F(HubNoticeTest, DeletingAnAccountTakesItsNoticesWithIt) {
  const auto [admin_connection, admin] = login("ana", Role::Admin);
  const auto [user_connection, user] = login("bruno", Role::User);

  (void)send(admin_connection, proto::SendNotice{user.id, "read this"});
  ASSERT_EQ(notices_.pending_for(user.id).size(), 1U);

  (void)send(admin_connection, proto::DeleteUser{user.id});
  EXPECT_TRUE(notices_.pending_for(user.id).empty());
}

}  // namespace
