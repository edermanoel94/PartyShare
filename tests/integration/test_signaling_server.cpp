// End to end tests for the signaling server: real sockets, real WebSocket
// handshakes, real JSON on the wire.
//
// These cover the M2 acceptance criteria from PLAN.md: five clients in one
// room, a full room, an unknown room, an abrupt disconnection, two clients
// racing for the screen share, and malformed input.

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "signaling/server.hpp"
#include "websocket_test_client.hpp"

namespace {

using namespace std::chrono_literals;
using dv::server::SignalingServer;
using dv::testing::WebSocketTestClient;

namespace proto = dv::protocol;

/// Generous enough to survive a loaded CI machine, short enough that a genuine
/// failure does not stall the suite.
constexpr auto kTimeout = 3000ms;

/// Opening the socket gets its own, longer allowance, because it is the one
/// step whose cost does not belong to us: every message wait after it is a
/// loopback round trip measured in microseconds, while the handshake happens
/// while the fixture is still hashing its eight accounts with scrypt.
///
/// Three seconds was not enough. Running the suite alongside a clang-tidy pass
/// produced three failures in one run, all of them here and all of them at the
/// timeout rather than on an assertion. A message wait that needs more than
/// three seconds is a bug; a handshake that does is a busy machine.
constexpr auto kConnectTimeout = 10000ms;

class SignalingServerTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SignalingServer::Options options;
    options.bind_address = "127.0.0.1";
    options.port = 0;  // let the operating system pick a free port
    // These suites are about signaling alone, so media routing stays off.
    // With the SFU on, joining a room also produces an offer from the server
    // itself, which has nothing to do with what is under test here and is
    // covered by test_sfu.cpp.
    options.enable_sfu = false;
    options.hub.max_participants_per_room = 5;
    options.hub.heartbeat_interval = 500ms;
    options.hub.heartbeat_timeout = 2000ms;

    server_ = std::make_unique<SignalingServer>(options);
    for (int i = 0; i < 8; ++i) {
      const std::string name = "user" + std::to_string(i);
      ASSERT_TRUE(server_->add_user(name, "password", name).ok());
    }
    server_->start();
    ASSERT_NE(server_->port(), 0);
  }

  void TearDown() override {
    clients_.clear();
    server_->stop();
    server_.reset();
  }

  /// Connects a client and logs it in, returning the client and its identity.
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

  std::string create_room(WebSocketTestClient& client, const std::string& user_id) {
    client.send(proto::CreateRoom{user_id, ""});
    const auto created = client.wait_for<proto::RoomCreated>(kTimeout);
    EXPECT_TRUE(created.has_value());
    return created ? created->room_id : std::string{};
  }

  /// Joins and waits until the server confirms, which is the user_joined that
  /// names the joiner themselves.
  void join(WebSocketTestClient& client, const std::string& room, const dv::models::User& user) {
    client.send(proto::JoinRoom{room, user.id, ""});
    for (int i = 0; i < 10; ++i) {
      const auto joined = client.wait_for<proto::UserJoined>(kTimeout);
      ASSERT_TRUE(joined.has_value());
      if (joined->user.id == user.id) {
        return;
      }
    }
    FAIL() << "never received our own user_joined";
  }

  std::unique_ptr<SignalingServer> server_;
  std::vector<std::unique_ptr<WebSocketTestClient>> clients_;
};

TEST_F(SignalingServerTest, AClientCanConnectAndAuthenticate) {
  const auto [client, user] = login("user0");
  EXPECT_FALSE(user.id.empty());
  EXPECT_EQ(user.display_name, "user0");
}

TEST_F(SignalingServerTest, TheWrongPasswordIsRejectedOverTheWire) {
  WebSocketTestClient client(server_->port());
  ASSERT_TRUE(client.wait_until_open(kConnectTimeout));

  client.send(proto::Authenticate{"user0", "wrong"});
  const auto error = client.wait_for<proto::ErrorMessage>(kTimeout);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
}

TEST_F(SignalingServerTest, MessagesBeforeAuthenticationAreRejected) {
  WebSocketTestClient client(server_->port());
  ASSERT_TRUE(client.wait_until_open(kConnectTimeout));

  client.send(proto::CreateRoom{"whoever", "room"});
  const auto error = client.wait_for<proto::ErrorMessage>(kTimeout);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
}

TEST_F(SignalingServerTest, ARoomCanBeCreatedAndJoined) {
  auto [client, user] = login("user0");
  const std::string room = create_room(*client, user.id);
  ASSERT_TRUE(dv::models::is_valid_room_id(room)) << room;
  join(*client, room, user);
}

TEST_F(SignalingServerTest, JoiningAnUnknownRoomIsRefused) {
  auto [client, user] = login("user0");
  client->send(proto::JoinRoom{"ABCDEF", user.id, ""});

  const auto error = client->wait_for<proto::ErrorMessage>(kTimeout);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "room_not_found");
}

TEST_F(SignalingServerTest, FiveClientsShareOneRoom) {
  auto [owner, owner_user] = login("user0");
  const std::string room = create_room(*owner, owner_user.id);
  join(*owner, room, owner_user);

  for (int i = 1; i < 5; ++i) {
    auto [client, user] = login("user" + std::to_string(i));
    join(*client, room, user);
  }

  // The room owner was told about each of the four newcomers.
  const auto announcements = owner->collect<proto::UserJoined>(kTimeout, 4);
  EXPECT_EQ(announcements.size(), 4u);
}

TEST_F(SignalingServerTest, TheSixthClientIsToldTheRoomIsFull) {
  auto [owner, owner_user] = login("user0");
  const std::string room = create_room(*owner, owner_user.id);
  join(*owner, room, owner_user);

  for (int i = 1; i < 5; ++i) {
    auto [client, user] = login("user" + std::to_string(i));
    join(*client, room, user);
  }

  auto [extra, extra_user] = login("user5");
  extra->send(proto::JoinRoom{room, extra_user.id, ""});

  const auto error = extra->wait_for<proto::ErrorMessage>(kTimeout);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "room_full");
}

TEST_F(SignalingServerTest, LeavingIsAnnouncedToTheOthers) {
  auto [ana, ana_user] = login("user0");
  const std::string room = create_room(*ana, ana_user.id);
  join(*ana, room, ana_user);

  auto [bruno, bruno_user] = login("user1");
  join(*bruno, room, bruno_user);
  ASSERT_TRUE(ana->wait_for<proto::UserJoined>(kTimeout).has_value());

  bruno->send(proto::LeaveRoom{room, bruno_user.id});
  const auto left = ana->wait_for<proto::UserLeft>(kTimeout);
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(left->user_id, bruno_user.id);
}

TEST_F(SignalingServerTest, AnAbruptDisconnectionIsAnnouncedToTheOthers) {
  auto [ana, ana_user] = login("user0");
  const std::string room = create_room(*ana, ana_user.id);
  join(*ana, room, ana_user);

  auto [bruno, bruno_user] = login("user1");
  join(*bruno, room, bruno_user);
  ASSERT_TRUE(ana->wait_for<proto::UserJoined>(kTimeout).has_value());

  // No closing handshake, the way a crashed client behaves.
  bruno->force_close();

  const auto left = ana->wait_for<proto::UserLeft>(5000ms);
  ASSERT_TRUE(left.has_value()) << "user_left must arrive well inside 5 seconds";
  EXPECT_EQ(left->user_id, bruno_user.id);
}

TEST_F(SignalingServerTest, AnOfferReachesOnlyItsRecipient) {
  auto [ana, ana_user] = login("user0");
  const std::string room = create_room(*ana, ana_user.id);
  join(*ana, room, ana_user);

  auto [bruno, bruno_user] = login("user1");
  join(*bruno, room, bruno_user);

  auto [carla, carla_user] = login("user2");
  join(*carla, room, carla_user);

  const std::string sdp = "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n";
  ana->send(proto::Offer{room, ana_user.id, bruno_user.id, sdp});

  const auto forwarded = bruno->wait_for<proto::Offer>(kTimeout);
  ASSERT_TRUE(forwarded.has_value());
  EXPECT_EQ(forwarded->sdp, sdp);
  EXPECT_EQ(forwarded->from_user_id, ana_user.id);

  EXPECT_TRUE(carla->none_arrives<proto::Offer>(500ms));
}

TEST_F(SignalingServerTest, TwoClientsRacingForTheScreenShare) {
  auto [ana, ana_user] = login("user0");
  const std::string room = create_room(*ana, ana_user.id);
  join(*ana, room, ana_user);

  auto [bruno, bruno_user] = login("user1");
  join(*bruno, room, bruno_user);

  ana->send(proto::ScreenShareStarted{room, ana_user.id});
  const auto started = bruno->wait_for<proto::ScreenShareStarted>(kTimeout);
  ASSERT_TRUE(started.has_value());
  EXPECT_EQ(started->user_id, ana_user.id);

  bruno->send(proto::ScreenShareStarted{room, bruno_user.id});
  const auto error = bruno->wait_for<proto::ErrorMessage>(kTimeout);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "screen_share_busy");
}

TEST_F(SignalingServerTest, AJoinerIsToldAboutAShareInProgress) {
  auto [ana, ana_user] = login("user0");
  const std::string room = create_room(*ana, ana_user.id);
  join(*ana, room, ana_user);
  ana->send(proto::ScreenShareStarted{room, ana_user.id});
  ASSERT_TRUE(ana->wait_for<proto::ScreenShareStarted>(kTimeout).has_value());

  auto [bruno, bruno_user] = login("user1");
  bruno->send(proto::JoinRoom{room, bruno_user.id, ""});

  const auto sharing = bruno->wait_for<proto::ScreenShareStarted>(kTimeout);
  ASSERT_TRUE(sharing.has_value());
  EXPECT_EQ(sharing->user_id, ana_user.id);
}

TEST_F(SignalingServerTest, MuteIsBroadcastToTheRoom) {
  auto [ana, ana_user] = login("user0");
  const std::string room = create_room(*ana, ana_user.id);
  join(*ana, room, ana_user);

  auto [bruno, bruno_user] = login("user1");
  join(*bruno, room, bruno_user);

  ana->send(proto::Mute{room, ana_user.id});
  const auto muted = bruno->wait_for<proto::Mute>(kTimeout);
  ASSERT_TRUE(muted.has_value());
  EXPECT_EQ(muted->user_id, ana_user.id);
}

TEST_F(SignalingServerTest, MalformedInputDoesNotBringTheServerDown) {
  auto [client, user] = login("user0");

  for (const std::string& payload :
       {std::string("{not json"), std::string("[1,2,3]"), std::string(R"({"type":"nope"})"),
        std::string(R"({"type":"join_room"})")}) {
    client->send_raw(payload);
    const auto error = client->wait_for<proto::ErrorMessage>(kTimeout);
    EXPECT_TRUE(error.has_value()) << "payload: " << payload;
  }

  // Still alive and serving.
  const std::string room = create_room(*client, user.id);
  EXPECT_TRUE(dv::models::is_valid_room_id(room));
}

TEST_F(SignalingServerTest, TheServerPingsIdleClients) {
  auto [client, user] = login("user0");
  const auto ping = client->wait_for<proto::Ping>(kTimeout);
  EXPECT_TRUE(ping.has_value());
}

}  // namespace
