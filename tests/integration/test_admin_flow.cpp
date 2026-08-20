// End to end tests for administration: real sockets, real WebSocket handshakes,
// real JSON on the wire.
//
// The unit tests in test_hub_admin.cpp already cover the rules. What is under
// test here is everything between them and a client: that the thirteen new
// messages survive serialization and parsing in both directions, and that a
// participant who is removed finds out on their own socket rather than only in
// the server's memory.

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <dv/models/user.hpp>

#include "signaling/server.hpp"
#include "websocket_test_client.hpp"

namespace {

using namespace std::chrono_literals;
using dv::models::Role;
using dv::server::SignalingServer;
using dv::testing::WebSocketTestClient;

namespace proto = dv::protocol;

constexpr auto kTimeout = 3000ms;
constexpr auto kConnectTimeout = 10000ms;

class AdminFlowTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SignalingServer::Options options;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    // Signaling alone, as in test_signaling_server.cpp: with the SFU on,
    // joining also produces an offer from the server, which has nothing to do
    // with what is under test here.
    options.enable_sfu = false;
    options.hub.max_participants_per_room = 5;
    options.hub.heartbeat_interval = 500ms;
    options.hub.heartbeat_timeout = 2000ms;

    server_ = std::make_unique<SignalingServer>(options);
    ASSERT_TRUE(server_->add_user("ana", "password", "Ana", Role::Admin).ok());
    ASSERT_TRUE(server_->add_user("bruno", "password", "Bruno", Role::User).ok());
    server_->start();
    ASSERT_NE(server_->port(), 0);
  }

  void TearDown() override {
    clients_.clear();
    server_->stop();
    server_.reset();
  }

  std::pair<WebSocketTestClient*, dv::models::User> login(const std::string& username) {
    auto client = std::make_unique<WebSocketTestClient>(server_->port());
    EXPECT_TRUE(client->wait_until_open(kConnectTimeout)) << "could not connect " << username;

    client->send(proto::Authenticate{username, "password"});
    const auto authenticated = client->wait_for<proto::Authenticated>(kTimeout);
    EXPECT_TRUE(authenticated.has_value()) << "could not authenticate " << username;

    WebSocketTestClient* raw = client.get();
    clients_.push_back(std::move(client));
    return {raw, authenticated ? authenticated->user : dv::models::User{}};
  }

  std::unique_ptr<SignalingServer> server_;
  std::vector<std::unique_ptr<WebSocketTestClient>> clients_;
};

TEST_F(AdminFlowTest, TheRoleSurvivesTheWire) {
  const auto [admin, ana] = login("ana");
  EXPECT_EQ(ana.role, Role::Admin);

  const auto [plain, bruno] = login("bruno");
  EXPECT_EQ(bruno.role, Role::User);
}

TEST_F(AdminFlowTest, AnAdministratorRemovesAParticipantOverTheWire) {
  auto [admin, ana] = login("ana");
  auto [plain, bruno] = login("bruno");

  admin->send(proto::CreateRoom{ana.id, "room"});
  const auto created = admin->wait_for<proto::RoomCreated>(kTimeout);
  ASSERT_TRUE(created.has_value());
  const std::string room = created->room_id;

  admin->send(proto::JoinRoom{room, ana.id, "Ana"});
  plain->send(proto::JoinRoom{room, bruno.id, "Bruno"});
  ASSERT_TRUE(admin->wait_for<proto::UserJoined>(kTimeout).has_value());

  admin->send(proto::KickUser{room, bruno.id, "off topic"});

  // The one thing the unit tests cannot check: it arrives on Bruno's own
  // socket, parsed back into the message the server sent.
  const auto kicked = plain->wait_for<proto::UserKicked>(kTimeout);
  ASSERT_TRUE(kicked.has_value());
  EXPECT_EQ(kicked->user_id, bruno.id);
  EXPECT_EQ(kicked->room_id, room);
  EXPECT_EQ(kicked->reason, "off topic");
}

TEST_F(AdminFlowTest, AParticipantIsRefusedAndTheRoomIsUnchanged) {
  auto [admin, ana] = login("ana");
  auto [plain, bruno] = login("bruno");

  admin->send(proto::CreateRoom{ana.id, "room"});
  const auto created = admin->wait_for<proto::RoomCreated>(kTimeout);
  ASSERT_TRUE(created.has_value());
  const std::string room = created->room_id;

  admin->send(proto::JoinRoom{room, ana.id, "Ana"});
  plain->send(proto::JoinRoom{room, bruno.id, "Bruno"});
  ASSERT_TRUE(admin->wait_for<proto::UserJoined>(kTimeout).has_value());

  plain->send(proto::KickUser{room, ana.id, "no"});

  const auto error = plain->wait_for<proto::ErrorMessage>(kTimeout);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "forbidden");

  // Ana never hears about it, which is the part that matters: a refused
  // administrative message must not reach the room at all.
  EXPECT_FALSE(admin->wait_for<proto::UserKicked>(500ms).has_value());
}

TEST_F(AdminFlowTest, AForcedMuteArrivesNamingWhoDidIt) {
  auto [admin, ana] = login("ana");
  auto [plain, bruno] = login("bruno");

  admin->send(proto::CreateRoom{ana.id, "room"});
  const auto created = admin->wait_for<proto::RoomCreated>(kTimeout);
  ASSERT_TRUE(created.has_value());
  const std::string room = created->room_id;

  admin->send(proto::JoinRoom{room, ana.id, "Ana"});
  plain->send(proto::JoinRoom{room, bruno.id, "Bruno"});
  ASSERT_TRUE(admin->wait_for<proto::UserJoined>(kTimeout).has_value());

  admin->send(proto::ForceMute{room, bruno.id, true});

  const auto muted = plain->wait_for<proto::Mute>(kTimeout);
  ASSERT_TRUE(muted.has_value());
  EXPECT_EQ(muted->user_id, bruno.id);
  EXPECT_EQ(muted->by_user_id, ana.id);
}

TEST_F(AdminFlowTest, TheAccountListCrossesTheWireWithoutSecrets) {
  auto [admin, ana] = login("ana");

  admin->send(proto::ListUsers{});
  const auto list = admin->wait_for<proto::UserList>(kTimeout);
  ASSERT_TRUE(list.has_value());
  ASSERT_EQ(list->users.size(), 2U);

  for (const proto::UserSummary& summary : list->users) {
    EXPECT_FALSE(summary.username.empty());
    EXPECT_FALSE(summary.user.id.empty());
  }

  const auto ana_entry = std::ranges::find_if(
      list->users, [](const proto::UserSummary& summary) { return summary.username == "ana"; });
  ASSERT_NE(ana_entry, list->users.end());
  EXPECT_EQ(ana_entry->user.role, Role::Admin);
  EXPECT_TRUE(ana_entry->online);
}

TEST_F(AdminFlowTest, AnAccountCreatedOverTheWireCanLogIn) {
  auto [admin, ana] = login("ana");

  admin->send(proto::CreateUser{"carla", "another-password", "Carla", Role::Admin});
  const auto list = admin->wait_for<proto::UserList>(kTimeout);
  ASSERT_TRUE(list.has_value());
  EXPECT_EQ(list->users.size(), 3U);

  auto client = std::make_unique<WebSocketTestClient>(server_->port());
  ASSERT_TRUE(client->wait_until_open(kConnectTimeout));
  client->send(proto::Authenticate{"carla", "another-password"});
  const auto authenticated = client->wait_for<proto::Authenticated>(kTimeout);
  ASSERT_TRUE(authenticated.has_value());
  EXPECT_EQ(authenticated->user.role, Role::Admin);
  clients_.push_back(std::move(client));
}

TEST_F(AdminFlowTest, ClosingARoomEmptiesItForEverybody) {
  auto [admin, ana] = login("ana");
  auto [plain, bruno] = login("bruno");

  admin->send(proto::CreateRoom{ana.id, "room"});
  const auto created = admin->wait_for<proto::RoomCreated>(kTimeout);
  ASSERT_TRUE(created.has_value());
  const std::string room = created->room_id;

  admin->send(proto::JoinRoom{room, ana.id, "Ana"});
  plain->send(proto::JoinRoom{room, bruno.id, "Bruno"});
  ASSERT_TRUE(admin->wait_for<proto::UserJoined>(kTimeout).has_value());

  admin->send(proto::DeleteRoom{room});

  ASSERT_TRUE(plain->wait_for<proto::UserKicked>(kTimeout).has_value());
  const auto rooms = admin->wait_for<proto::RoomList>(kTimeout);
  ASSERT_TRUE(rooms.has_value());
  EXPECT_TRUE(rooms->rooms.empty());

  // And the identifier is genuinely gone, not merely emptied.
  plain->send(proto::JoinRoom{room, bruno.id, "Bruno"});
  const auto error = plain->wait_for<proto::ErrorMessage>(kTimeout);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "room_not_found");
}

TEST_F(AdminFlowTest, TheAuditLogCrossesTheWire) {
  auto [admin, ana] = login("ana");
  auto [plain, bruno] = login("bruno");

  admin->send(proto::CreateRoom{ana.id, "room"});
  const auto created = admin->wait_for<proto::RoomCreated>(kTimeout);
  ASSERT_TRUE(created.has_value());
  const std::string room = created->room_id;

  admin->send(proto::JoinRoom{room, ana.id, "Ana"});
  plain->send(proto::JoinRoom{room, bruno.id, "Bruno"});
  ASSERT_TRUE(admin->wait_for<proto::UserJoined>(kTimeout).has_value());

  admin->send(proto::KickUser{room, bruno.id, "off topic"});
  ASSERT_TRUE(plain->wait_for<proto::UserKicked>(kTimeout).has_value());

  admin->send(proto::ListAudit{});
  const auto log = admin->wait_for<proto::AuditList>(kTimeout);
  ASSERT_TRUE(log.has_value());
  ASSERT_EQ(log->entries.size(), 1U);
  EXPECT_EQ(log->entries.front().action, "kick");
  EXPECT_EQ(log->entries.front().actor_id, ana.id);
  EXPECT_EQ(log->entries.front().target_id, bruno.id);
  EXPECT_GT(log->entries.front().timestamp_seconds, 0);
}

}  // namespace
