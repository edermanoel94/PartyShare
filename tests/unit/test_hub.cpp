#include <chrono>
#include <cstddef>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include <gtest/gtest.h>

#include <dv/models/chat.hpp>
#include <dv/protocol/message.hpp>

#include "signaling/hub.hpp"

namespace {

using namespace std::chrono_literals;
using dv::protocol::Message;
using dv::server::ConnectionId;
using dv::server::Hub;
using dv::server::Outgoing;

namespace proto = dv::protocol;

/// Drives the Hub the way the transport layer does, without any sockets.
class HubTest : public ::testing::Test {
 protected:
  HubTest() : hub_(Hub::Options{5, 5000ms, 15000ms, 4321u}) {}

  ConnectionId connect() {
    const ConnectionId id = next_connection_++;
    hub_.on_connect(id, now_);
    return id;
  }

  std::vector<Outgoing> send(ConnectionId connection, const Message& message) {
    return hub_.on_message(connection, proto::serialize(message), now_);
  }

  std::vector<Outgoing> send_raw(ConnectionId connection, const std::string& payload) {
    return hub_.on_message(connection, payload, now_);
  }

  /// Registers an account and logs a fresh connection into it.
  std::pair<ConnectionId, dv::models::User> login(const std::string& username) {
    const auto registered = hub_.authenticator().add_user(username, "password", username);
    EXPECT_TRUE(registered.ok()) << registered.error().message;

    const ConnectionId connection = connect();
    const auto out = send(connection, proto::Authenticate{username, "password"});
    const auto authenticated = find<proto::Authenticated>(out, connection);
    EXPECT_TRUE(authenticated.has_value());
    return {connection, authenticated ? authenticated->user : dv::models::User{}};
  }

  std::string create_room(ConnectionId connection, const std::string& user_id) {
    const auto out = send(connection, proto::CreateRoom{user_id, "dev-room"});
    const auto created = find<proto::RoomCreated>(out, connection);
    EXPECT_TRUE(created.has_value());
    return created ? created->room_id : std::string{};
  }

  /// The first message of type T addressed at `connection`, if any.
  template <typename T>
  static std::optional<T> find(const std::vector<Outgoing>& out, ConnectionId connection) {
    for (const Outgoing& outgoing : out) {
      if (outgoing.connection == connection && std::holds_alternative<T>(outgoing.message)) {
        return std::get<T>(outgoing.message);
      }
    }
    return std::nullopt;
  }

  template <typename T>
  static int count(const std::vector<Outgoing>& out, ConnectionId connection) {
    int total = 0;
    for (const Outgoing& outgoing : out) {
      if (outgoing.connection == connection && std::holds_alternative<T>(outgoing.message)) {
        ++total;
      }
    }
    return total;
  }

  Hub hub_;
  Hub::Clock::time_point now_{};
  ConnectionId next_connection_ = 1;
};

// --- authentication ----------------------------------------------------------

TEST_F(HubTest, AuthenticationSucceedsWithValidCredentials) {
  EXPECT_TRUE(hub_.authenticator().add_user("ana", "password", "Ana").ok());
  const ConnectionId connection = connect();

  const auto out = send(connection, proto::Authenticate{"ana", "password"});
  const auto authenticated = find<proto::Authenticated>(out, connection);
  ASSERT_TRUE(authenticated.has_value());
  EXPECT_EQ(authenticated->user.display_name, "Ana");
  EXPECT_FALSE(authenticated->token.empty());
}

TEST_F(HubTest, AuthenticationFailsWithTheWrongPassword) {
  EXPECT_TRUE(hub_.authenticator().add_user("ana", "password", "Ana").ok());
  const ConnectionId connection = connect();

  const auto out = send(connection, proto::Authenticate{"ana", "wrong"});
  const auto error = find<proto::ErrorMessage>(out, connection);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
}

TEST_F(HubTest, EverythingIsRejectedBeforeAuthentication) {
  const ConnectionId connection = connect();

  const auto out = send(connection, proto::CreateRoom{"whoever", "room"});
  const auto error = find<proto::ErrorMessage>(out, connection);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
}

// The exception to the rule above, and the reason it exists: the server pings
// every connection it holds, including one that has not authenticated, so
// refusing the answer means telling a client at the login screen `unauthorized`
// once per heartbeat interval until it gives up or the window closes.
TEST_F(HubTest, TheHeartbeatIsAnsweredBeforeAuthentication) {
  const ConnectionId connection = connect();

  const auto out = send(connection, proto::Pong{});

  EXPECT_FALSE(find<proto::ErrorMessage>(out, connection).has_value())
      << "a pong from an unauthenticated connection was refused";
}

TEST_F(HubTest, ASecondLoginDetachesTheOlderConnection) {
  const auto [first, user] = login("ana");
  const std::string room = create_room(first, user.id);

  // The same account logs in again from somewhere else.
  const ConnectionId second = connect();
  const auto out = send(second, proto::Authenticate{"ana", "password"});
  ASSERT_TRUE(find<proto::Authenticated>(out, second).has_value());

  // The old connection lost its identity and is back to square one.
  const auto stale = send(first, proto::JoinRoom{room, user.id, ""});
  const auto error = find<proto::ErrorMessage>(stale, first);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
}

// --- rooms -------------------------------------------------------------------

TEST_F(HubTest, CreateRoomReturnsAValidIdentifier) {
  const auto [connection, user] = login("ana");
  const std::string room = create_room(connection, user.id);
  EXPECT_TRUE(dv::models::is_valid_room_id(room)) << room;
}

TEST_F(HubTest, CreateRoomRejectsSomeoneElsesUserId) {
  const auto [connection, user] = login("ana");
  const auto out = send(connection, proto::CreateRoom{"other-id", "room"});
  const auto error = find<proto::ErrorMessage>(out, connection);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
}

TEST_F(HubTest, JoiningAnUnknownRoomFails) {
  const auto [connection, user] = login("ana");
  const auto out = send(connection, proto::JoinRoom{"ABCDEF", user.id, ""});
  const auto error = find<proto::ErrorMessage>(out, connection);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "room_not_found");
}

TEST_F(HubTest, TheJoinerSeesEveryoneAndThemselvesLast) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");
  const auto [carla, carla_user] = login("carla");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const auto out = send(carla, proto::JoinRoom{room, carla_user.id, ""});

  std::vector<std::string> announced;
  for (const Outgoing& outgoing : out) {
    if (outgoing.connection != carla) {
      continue;
    }
    if (const auto* joined = std::get_if<proto::UserJoined>(&outgoing.message)) {
      announced.push_back(joined->user.id);
    }
  }

  ASSERT_EQ(announced.size(), 3u);
  EXPECT_EQ(announced.back(), carla_user.id) << "own user_joined must arrive last";
}

TEST_F(HubTest, TheOthersAreToldAboutTheNewParticipant) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});

  const auto out = send(bruno, proto::JoinRoom{room, bruno_user.id, ""});
  const auto announced = find<proto::UserJoined>(out, ana);
  ASSERT_TRUE(announced.has_value());
  EXPECT_EQ(announced->user.id, bruno_user.id);
}

TEST_F(HubTest, TheCapacityLimitIsEnforced) {
  const auto [owner, owner_user] = login("owner");
  const std::string room = create_room(owner, owner_user.id);
  (void)send(owner, proto::JoinRoom{room, owner_user.id, ""});

  for (int i = 1; i < 5; ++i) {
    const auto [connection, user] = login("user" + std::to_string(i));
    const auto out = send(connection, proto::JoinRoom{room, user.id, ""});
    EXPECT_FALSE(find<proto::ErrorMessage>(out, connection).has_value());
  }

  const auto [extra, extra_user] = login("extra");
  const auto out = send(extra, proto::JoinRoom{room, extra_user.id, ""});
  const auto error = find<proto::ErrorMessage>(out, extra);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "room_full");
}

TEST_F(HubTest, LeavingTellsTheRemainingParticipants) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const auto out = send(bruno, proto::LeaveRoom{room, bruno_user.id});
  const auto left = find<proto::UserLeft>(out, ana);
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(left->user_id, bruno_user.id);
}

TEST_F(HubTest, ADroppedConnectionRemovesTheParticipant) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const auto out = hub_.on_disconnect(bruno, now_);
  const auto left = find<proto::UserLeft>(out, ana);
  ASSERT_TRUE(left.has_value());
  EXPECT_EQ(left->user_id, bruno_user.id);
  EXPECT_FALSE(hub_.rooms().find(room)->contains(bruno_user.id));
}

// --- negotiation relay -------------------------------------------------------

TEST_F(HubTest, AnOfferIsForwardedUntouched) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const proto::Offer offer{room, ana_user.id, bruno_user.id, "v=0\r\ns=-\r\n"};
  const auto out = send(ana, offer);

  const auto forwarded = find<proto::Offer>(out, bruno);
  ASSERT_TRUE(forwarded.has_value());
  EXPECT_EQ(*forwarded, offer);
  EXPECT_FALSE(find<proto::Offer>(out, ana).has_value());
}

TEST_F(HubTest, AnOfferSentUnderSomeoneElsesNameIsRejected) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  // Ana claims the offer comes from Bruno.
  const auto out = send(ana, proto::Offer{room, bruno_user.id, ana_user.id, "v=0\r\n"});
  const auto error = find<proto::ErrorMessage>(out, ana);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
  EXPECT_EQ(count<proto::Offer>(out, bruno), 0);
}

TEST_F(HubTest, RelayingToSomeoneOutsideTheRoomFails) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});

  const auto out = send(ana, proto::Offer{room, ana_user.id, bruno_user.id, "v=0\r\n"});
  const auto error = find<proto::ErrorMessage>(out, ana);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "not_in_room");
}

TEST_F(HubTest, IceCandidatesAreForwarded) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const proto::IceCandidate candidate{
      room, ana_user.id, bruno_user.id, "candidate:1 1 UDP 1 10.0.0.1 5000 typ host", "audio", 0};
  const auto out = send(ana, candidate);
  const auto forwarded = find<proto::IceCandidate>(out, bruno);
  ASSERT_TRUE(forwarded.has_value());
  EXPECT_EQ(*forwarded, candidate);
}

// --- state changes -----------------------------------------------------------

TEST_F(HubTest, MuteIsConfirmedToEveryoneIncludingTheSender) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const auto out = send(ana, proto::Mute{room, ana_user.id});
  EXPECT_TRUE(find<proto::Mute>(out, ana).has_value());
  EXPECT_TRUE(find<proto::Mute>(out, bruno).has_value());
  EXPECT_TRUE(hub_.rooms().find(room)->find(ana_user.id)->muted);
}

TEST_F(HubTest, MutingAnotherParticipantIsRejected) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const auto out = send(ana, proto::Mute{room, bruno_user.id});
  const auto error = find<proto::ErrorMessage>(out, ana);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
  EXPECT_FALSE(hub_.rooms().find(room)->find(bruno_user.id)->muted);
}

TEST_F(HubTest, OnlyOneParticipantMayShareTheScreen) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const auto first = send(ana, proto::ScreenShareStarted{room, ana_user.id});
  EXPECT_TRUE(find<proto::ScreenShareStarted>(first, bruno).has_value());

  const auto second = send(bruno, proto::ScreenShareStarted{room, bruno_user.id});
  const auto error = find<proto::ErrorMessage>(second, bruno);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "screen_share_busy");
}

TEST_F(HubTest, AJoinerLearnsAboutAShareAlreadyInProgress) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(ana, proto::ScreenShareStarted{room, ana_user.id});

  const auto [bruno, bruno_user] = login("bruno");
  const auto out = send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const auto sharing = find<proto::ScreenShareStarted>(out, bruno);
  ASSERT_TRUE(sharing.has_value());
  EXPECT_EQ(sharing->user_id, ana_user.id);
}

TEST_F(HubTest, DroppingWhileSharingReleasesTheScreen) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});
  (void)send(ana, proto::ScreenShareStarted{room, ana_user.id});

  const auto out = hub_.on_disconnect(ana, now_);
  EXPECT_TRUE(find<proto::ScreenShareStopped>(out, bruno).has_value());
  EXPECT_EQ(hub_.rooms().find(room)->screen_sharer(), nullptr);
}

// --- chat --------------------------------------------------------------------

namespace {

/// What a client sends: a room, itself, and the text. Everything else on a
/// chat message belongs to the server.
proto::ChatMessage said(const std::string& room_id, const std::string& user_id,
                        const std::string& text) {
  return proto::ChatMessage{.message = {.room_id = room_id, .user_id = user_id, .text = text}};
}

}  // namespace

TEST_F(HubTest, AMessageReachesTheWholeRoomIncludingItsSender) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const auto out = send(ana, said(room, ana_user.id, "the build is green"));

  // The sender included: nobody displays anything until the server has agreed
  // to it, which is what keeps the order the same on every screen.
  const auto to_ana = find<proto::ChatMessage>(out, ana);
  const auto to_bruno = find<proto::ChatMessage>(out, bruno);
  ASSERT_TRUE(to_ana.has_value());
  ASSERT_TRUE(to_bruno.has_value());
  EXPECT_EQ(to_ana->message.text, "the build is green");
  EXPECT_EQ(to_ana->message, to_bruno->message);
}

TEST_F(HubTest, TheServerFillsInTheIdentifierTheNameAndTheTime) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});

  const auto out = send(ana, said(room, ana_user.id, "hello"));
  const auto chat = find<proto::ChatMessage>(out, ana);

  ASSERT_TRUE(chat.has_value());
  EXPECT_FALSE(chat->message.id.empty());
  EXPECT_GT(chat->message.timestamp_seconds, 0);
  EXPECT_EQ(chat->message.display_name, "Ana");
  EXPECT_EQ(chat->message.room_id, room);
}

TEST_F(HubTest, TheSenderCannotSignSomebodyElsesName) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, "Bruno"});

  proto::ChatMessage forged = said(room, ana_user.id, "I am not who I say");
  forged.message.display_name = "Bruno";
  const auto out = send(ana, forged);

  const auto chat = find<proto::ChatMessage>(out, bruno);
  ASSERT_TRUE(chat.has_value());
  // The name the room knows them by, not the one they asked for.
  EXPECT_EQ(chat->message.display_name, "Ana");
  EXPECT_EQ(chat->message.user_id, ana_user.id);
}

TEST_F(HubTest, AMessageSentUnderSomeoneElsesUserIdIsRejected) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});
  (void)send(bruno, proto::JoinRoom{room, bruno_user.id, ""});

  const auto out = send(ana, said(room, bruno_user.id, "not mine to say"));
  const auto error = find<proto::ErrorMessage>(out, ana);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
  EXPECT_TRUE(hub_.chat().list(room, 0).empty());
}

TEST_F(HubTest, SpeakingIntoARoomYouAreNotInIsRejected) {
  const auto [ana, ana_user] = login("ana");
  const auto [bruno, bruno_user] = login("bruno");

  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});

  const auto out = send(bruno, said(room, bruno_user.id, "let me in"));
  const auto error = find<proto::ErrorMessage>(out, bruno);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "not_in_room");
  EXPECT_TRUE(hub_.chat().list(room, 0).empty());
}

TEST_F(HubTest, AnEmptyMessageIsRefusedRatherThanStored) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});

  const auto out = send(ana, said(room, ana_user.id, "   \n  "));
  const auto error = find<proto::ErrorMessage>(out, ana);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "invalid_value");
  EXPECT_TRUE(hub_.chat().list(room, 0).empty());
}

TEST_F(HubTest, AMessageBeyondTheLimitIsRefusedRatherThanTruncated) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});

  const std::string too_long(dv::models::kMaxChatTextBytes + 1, 'x');
  const auto out = send(ana, said(room, ana_user.id, too_long));
  const auto error = find<proto::ErrorMessage>(out, ana);

  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "invalid_value");
  // Half a message is worse than none: nothing was written.
  EXPECT_TRUE(hub_.chat().list(room, 0).empty());
}

TEST_F(HubTest, WhatIsStoredIsTrimmed) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, ""});

  const auto out = send(ana, said(room, ana_user.id, "  hello  "));
  const auto chat = find<proto::ChatMessage>(out, ana);

  ASSERT_TRUE(chat.has_value());
  EXPECT_EQ(chat->message.text, "hello");
  // The same text on the wire and in the store, so the copy read back later is
  // the copy the room saw.
  const auto stored = hub_.chat().list(room, 0);
  ASSERT_EQ(stored.size(), 1U);
  EXPECT_EQ(stored.front().text, "hello");
}

TEST_F(HubTest, EmojiSurviveTheStoreAndTheBroadcast) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});

  const std::string text = "deploy feito 🚀🎉 tudo verde ✅";
  const auto out = send(ana, said(room, ana_user.id, text));

  // The whole path: parsed off the wire, trimmed, written, read back and
  // serialized again. Anything on it that counted characters instead of bytes
  // would hand back half of one.
  const auto chat = find<proto::ChatMessage>(out, ana);
  ASSERT_TRUE(chat.has_value());
  EXPECT_EQ(chat->message.text, text);

  const auto stored = hub_.chat().list(room, 0);
  ASSERT_EQ(stored.size(), 1U);
  EXPECT_EQ(stored.front().text, text);
}

TEST_F(HubTest, AMessageOfNothingButEmojiIsAMessage) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});

  const auto out = send(ana, said(room, ana_user.id, "  👍  "));

  EXPECT_FALSE(find<proto::ErrorMessage>(out, ana).has_value());
  const auto chat = find<proto::ChatMessage>(out, ana);
  ASSERT_TRUE(chat.has_value());
  EXPECT_EQ(chat->message.text, "👍");
}

TEST_F(HubTest, AJoinerIsSentWhatWasAlreadySaid) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});
  (void)send(ana, said(room, ana_user.id, "morning"));

  const auto [bruno, bruno_user] = login("bruno");
  const auto out = send(bruno, proto::JoinRoom{room, bruno_user.id, "Bruno"});

  const auto history = find<proto::ChatHistory>(out, bruno);
  ASSERT_TRUE(history.has_value());
  EXPECT_EQ(history->room_id, room);
  ASSERT_EQ(history->messages.size(), 1U);
  EXPECT_EQ(history->messages.front().text, "morning");
  EXPECT_EQ(history->messages.front().display_name, "Ana");
}

TEST_F(HubTest, AJoinerLearnsWhoIsThereBeforeWhatWasSaid) {
  // The history names people, so it has to arrive after the participant list
  // it names them from. See section 7 of docs/protocol.md.
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});
  (void)send(ana, said(room, ana_user.id, "morning"));

  const auto [bruno, bruno_user] = login("bruno");
  const auto out = send(bruno, proto::JoinRoom{room, bruno_user.id, "Bruno"});

  std::optional<std::size_t> last_joined;
  std::optional<std::size_t> history_at;
  for (std::size_t i = 0; i < out.size(); ++i) {
    if (out[i].connection != bruno) {
      continue;
    }
    if (std::holds_alternative<proto::UserJoined>(out[i].message)) {
      last_joined = i;
    }
    if (std::holds_alternative<proto::ChatHistory>(out[i].message)) {
      history_at = i;
    }
  }
  ASSERT_TRUE(last_joined.has_value());
  ASSERT_TRUE(history_at.has_value());
  EXPECT_LT(*last_joined, *history_at);
}

TEST_F(HubTest, ListChatAnswersAParticipantWithTheNewestMessages) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});
  for (const char* text : {"one", "two", "three"}) {
    (void)send(ana, said(room, ana_user.id, text));
  }

  const auto out = send(ana, proto::ListChat{.room_id = room, .limit = 2});
  const auto history = find<proto::ChatHistory>(out, ana);

  ASSERT_TRUE(history.has_value());
  ASSERT_EQ(history->messages.size(), 2U);
  EXPECT_EQ(history->messages.front().text, "two");
  EXPECT_EQ(history->messages.back().text, "three");
}

TEST_F(HubTest, ListChatIsRefusedToSomebodyOutsideTheRoom) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});
  (void)send(ana, said(room, ana_user.id, "between us"));

  // Authenticated, and knows the identifier, and still is not entitled to the
  // conversation. Six characters is a guess, not a credential.
  const auto [bruno, bruno_user] = login("bruno");
  const auto out = send(bruno, proto::ListChat{.room_id = room});

  const auto error = find<proto::ErrorMessage>(out, bruno);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "not_in_room");
  EXPECT_FALSE(find<proto::ChatHistory>(out, bruno).has_value());
}

TEST_F(HubTest, AnOrdinaryRoomForgetsItsConversationWhenItEmpties) {
  const auto [ana, ana_user] = login("ana");
  const std::string room = create_room(ana, ana_user.id);
  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});
  (void)send(ana, said(room, ana_user.id, "between us"));
  ASSERT_EQ(hub_.chat().list(room, 0).size(), 1U);

  (void)send(ana, proto::LeaveRoom{room, ana_user.id});

  // The room is gone, and identifiers are handed out again. A history that
  // outlived its room would end up on the screen of whoever is given these six
  // characters next.
  EXPECT_EQ(hub_.rooms().find(room), nullptr);
  EXPECT_TRUE(hub_.chat().list(room, 0).empty());
}

TEST_F(HubTest, APersistentRoomKeepsItsConversationWhenItEmpties) {
  const auto [ana, ana_user] = login("ana");
  const auto created = hub_.rooms().create_room("standup", ana_user.id, true);
  ASSERT_TRUE(created.ok());
  const std::string room = created.value();

  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});
  (void)send(ana, said(room, ana_user.id, "notes from today"));
  (void)send(ana, proto::LeaveRoom{room, ana_user.id});

  // The room survived its last participant, and so did what was said in it:
  // that is the whole point of an identifier worth writing down.
  ASSERT_NE(hub_.rooms().find(room), nullptr);
  ASSERT_EQ(hub_.chat().list(room, 0).size(), 1U);
  EXPECT_EQ(hub_.chat().list(room, 0).front().text, "notes from today");
}

TEST_F(HubTest, ARoomThatIsClosedForgetsItsConversation) {
  const auto [ana, ana_user] = login("ana");
  const auto created = hub_.rooms().create_room("standup", ana_user.id, true);
  ASSERT_TRUE(created.ok());
  const std::string room = created.value();

  (void)send(ana, proto::JoinRoom{room, ana_user.id, "Ana"});
  (void)send(ana, said(room, ana_user.id, "notes from today"));

  const auto removed = hub_.rooms().remove_room(room);
  ASSERT_TRUE(removed.ok());
  EXPECT_TRUE(hub_.chat().list(room, 0).empty());
}

// --- malformed input and heartbeat -------------------------------------------

TEST_F(HubTest, MalformedInputIsAnsweredNotCrashed) {
  const auto [connection, user] = login("ana");

  for (const std::string& payload :
       {std::string("{not json"), std::string(""), std::string("[1,2,3]"),
        std::string(R"({"type":42})"), std::string(R"({"type":"send_file"})")}) {
    const auto out = send_raw(connection, payload);
    const auto error = find<proto::ErrorMessage>(out, connection);
    EXPECT_TRUE(error.has_value()) << "payload: " << payload;
  }
  EXPECT_EQ(hub_.connection_count(), 1u);
}

TEST_F(HubTest, AMessageFromAnUnknownConnectionIsIgnored) {
  const auto out = hub_.on_message(999, R"({"type":"ping"})", now_);
  EXPECT_TRUE(out.empty());
}

TEST_F(HubTest, PingIsAnsweredWithPong) {
  const auto [connection, user] = login("ana");
  const auto out = send(connection, proto::Ping{"abc"});
  const auto pong = find<proto::Pong>(out, connection);
  ASSERT_TRUE(pong.has_value());
  EXPECT_EQ(pong->nonce, "abc");
}

TEST_F(HubTest, TheHeartbeatPingsIdleConnections) {
  const ConnectionId connection = connect();

  std::vector<ConnectionId> timed_out;
  EXPECT_TRUE(hub_.tick(now_ + 1s, timed_out).empty());
  EXPECT_TRUE(timed_out.empty());

  const auto out = hub_.tick(now_ + 6s, timed_out);
  EXPECT_TRUE(find<proto::Ping>(out, connection).has_value());
  EXPECT_TRUE(timed_out.empty());
}

TEST_F(HubTest, AConnectionThatStopsAnsweringTimesOut) {
  const ConnectionId connection = connect();

  std::vector<ConnectionId> timed_out;
  (void)hub_.tick(now_ + 20s, timed_out);
  ASSERT_EQ(timed_out.size(), 1u);
  EXPECT_EQ(timed_out.front(), connection);
}

TEST_F(HubTest, AnyMessageKeepsTheConnectionAlive) {
  const auto [connection, user] = login("ana");

  now_ += 10s;
  (void)send(connection, proto::Ping{});

  std::vector<ConnectionId> timed_out;
  (void)hub_.tick(now_ + 10s, timed_out);
  EXPECT_TRUE(timed_out.empty());
}

}  // namespace
