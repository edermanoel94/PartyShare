#include <string>
#include <variant>

#include <gtest/gtest.h>

#include <dv/protocol/message.hpp>

namespace {

using namespace dv::protocol;  // NOLINT(google-build-using-namespace): test readability

/// Serializes a message and parses it back, asserting the round trip preserves
/// both the alternative and every field.
template <typename T>
T round_trip(const T& original) {
  const auto parsed = parse(serialize(Message{original}));
  EXPECT_TRUE(parsed.ok()) << (parsed.ok() ? "" : parsed.error().message);
  if (!parsed.ok()) {
    return T{};
  }
  EXPECT_TRUE(std::holds_alternative<T>(parsed.value()));
  return std::get<T>(parsed.value());
}

// --- type table --------------------------------------------------------------

TEST(MessageType, NamesRoundTrip) {
  const MessageType types[] = {
      MessageType::CreateRoom,
      MessageType::RoomCreated,
      MessageType::JoinRoom,
      MessageType::LeaveRoom,
      MessageType::UserJoined,
      MessageType::UserLeft,
      MessageType::Offer,
      MessageType::Answer,
      MessageType::IceCandidate,
      MessageType::ScreenShareStarted,
      MessageType::ScreenShareStopped,
      MessageType::Mute,
      MessageType::Unmute,
      MessageType::Error,
      MessageType::Ping,
      MessageType::Pong,
  };
  for (const MessageType type : types) {
    const auto name = type_name(type);
    EXPECT_NE(name, "unknown");
    EXPECT_EQ(type_from_name(name), type);
  }
}

TEST(MessageType, NamesMatchTheSpec) {
  EXPECT_EQ(type_name(MessageType::Authenticate), "authenticate");
  EXPECT_EQ(type_name(MessageType::Authenticated), "authenticated");
  EXPECT_EQ(type_name(MessageType::JoinRoom), "join_room");
  EXPECT_EQ(type_name(MessageType::LeaveRoom), "leave_room");
  EXPECT_EQ(type_name(MessageType::UserJoined), "user_joined");
  EXPECT_EQ(type_name(MessageType::UserLeft), "user_left");
  EXPECT_EQ(type_name(MessageType::Offer), "offer");
  EXPECT_EQ(type_name(MessageType::Answer), "answer");
  EXPECT_EQ(type_name(MessageType::IceCandidate), "ice_candidate");
  EXPECT_EQ(type_name(MessageType::ScreenShareStarted), "screen_share_started");
  EXPECT_EQ(type_name(MessageType::ScreenShareStopped), "screen_share_stopped");
  EXPECT_EQ(type_name(MessageType::Mute), "mute");
  EXPECT_EQ(type_name(MessageType::Unmute), "unmute");
}

TEST(MessageType, UnknownNameYieldsNothing) {
  EXPECT_FALSE(type_from_name("join_channel").has_value());
  EXPECT_FALSE(type_from_name("").has_value());
}

// --- the wire example from the spec ------------------------------------------

TEST(Parse, AcceptsTheExampleFromTheSpec) {
  const auto parsed = parse(R"({"type":"join_room","room_id":"8F42A1","user_id":"user123"})");
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  ASSERT_TRUE(std::holds_alternative<JoinRoom>(parsed.value()));
  const auto& join = std::get<JoinRoom>(parsed.value());
  EXPECT_EQ(join.room_id, "8F42A1");
  EXPECT_EQ(join.user_id, "user123");
  EXPECT_EQ(join.display_name, "");
}

TEST(Serialize, EmitsAFlatObjectWithATypeField) {
  const std::string text = serialize(Message{LeaveRoom{"8F42A1", "user123"}});
  EXPECT_NE(text.find(R"("type":"leave_room")"), std::string::npos);
  EXPECT_NE(text.find(R"("room_id":"8F42A1")"), std::string::npos);
  EXPECT_NE(text.find(R"("user_id":"user123")"), std::string::npos);
}

// --- round trips -------------------------------------------------------------

TEST(RoundTrip, Authenticate) {
  const Authenticate original{"ana", "correct horse battery staple"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, Authenticated) {
  const Authenticated original{dv::models::User{"user123", "Ana", ""}, "0a1b2c3d", 3600};
  EXPECT_EQ(round_trip(original), original);
}

TEST(Parse, AuthenticateRequiresBothCredentials) {
  EXPECT_EQ(parse(R"({"type":"authenticate","username":"ana"})").error().code, "missing_field");
  EXPECT_EQ(parse(R"({"type":"authenticate","password":"x"})").error().code, "missing_field");
}

TEST(RoundTrip, CreateRoom) {
  const CreateRoom original{"user123", "dev-room"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, RoomCreated) {
  const RoomCreated original{"8F42A1", "dev-room"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, JoinRoom) {
  const JoinRoom original{"8F42A1", "user123", "Ana"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, LeaveRoom) {
  const LeaveRoom original{"8F42A1", "user123"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, UserJoined) {
  const UserJoined original{"8F42A1", dv::models::User{"user123", "Ana", "https://x/a.png"}};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, UserLeft) {
  const UserLeft original{"8F42A1", "user123"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, Offer) {
  const Offer original{"8F42A1", "user1", "user2", "v=0\r\no=- 0 0 IN IP4 127.0.0.1\r\n"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, Answer) {
  const Answer original{"8F42A1", "user2", "user1", "v=0\r\na=recvonly\r\n"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, IceCandidate) {
  const IceCandidate original{"8F42A1", "user1",
                              "user2",  "candidate:1 1 UDP 2130706431 192.168.0.1 54321 typ host",
                              "audio",  1};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, ScreenShareStartedAndStopped) {
  EXPECT_EQ(round_trip(ScreenShareStarted{"8F42A1", "user1"}),
            (ScreenShareStarted{"8F42A1", "user1"}));
  EXPECT_EQ(round_trip(ScreenShareStopped{"8F42A1", "user1"}),
            (ScreenShareStopped{"8F42A1", "user1"}));
}

TEST(RoundTrip, MuteAndUnmute) {
  EXPECT_EQ(round_trip(Mute{"8F42A1", "user1"}), (Mute{"8F42A1", "user1"}));
  EXPECT_EQ(round_trip(Unmute{"8F42A1", "user1"}), (Unmute{"8F42A1", "user1"}));
}

TEST(RoundTrip, ErrorMessage) {
  const ErrorMessage original{"room_full", "the room already has 5 participants"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, PingAndPong) {
  EXPECT_EQ(round_trip(Ping{"abc"}), (Ping{"abc"}));
  EXPECT_EQ(round_trip(Pong{"abc"}), (Pong{"abc"}));
}

TEST(RoundTrip, PreservesUnicodeAndEmbeddedQuotes) {
  const JoinRoom original{"8F42A1", "user1", R"(Ana "A" Ferrão)"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, PreservesAnSdpWithNewlines) {
  const std::string sdp = "v=0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\na=rtpmap:111 opus/48000/2\r\n";
  const Offer original{"8F42A1", "user1", "user2", sdp};
  EXPECT_EQ(round_trip(original).sdp, sdp);
}

// --- invalid input -----------------------------------------------------------

TEST(Parse, RejectsMalformedJson) {
  const auto parsed = parse(R"({"type": "join_room",)");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "invalid_json");
}

TEST(Parse, RejectsAnEmptyPayload) {
  const auto parsed = parse("");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "invalid_json");
}

TEST(Parse, RejectsANonObjectRoot) {
  EXPECT_EQ(parse(R"(["join_room"])").error().code, "invalid_json");
  EXPECT_EQ(parse(R"("join_room")").error().code, "invalid_json");
  EXPECT_EQ(parse("42").error().code, "invalid_json");
}

TEST(Parse, RejectsAMissingTypeField) {
  const auto parsed = parse(R"({"room_id":"8F42A1"})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "missing_field");
}

TEST(Parse, RejectsANonStringType) {
  const auto parsed = parse(R"({"type": 7})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "invalid_type");
}

TEST(Parse, RejectsAnUnknownType) {
  const auto parsed = parse(R"({"type":"send_file","room_id":"8F42A1"})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "unknown_message_type");
}

TEST(Parse, RejectsAMissingRequiredField) {
  const auto parsed = parse(R"({"type":"join_room","room_id":"8F42A1"})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "missing_field");
  EXPECT_NE(parsed.error().message.find("user_id"), std::string::npos);
}

TEST(Parse, RejectsAFieldWithTheWrongType) {
  const auto parsed = parse(R"({"type":"join_room","room_id":123,"user_id":"user1"})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "invalid_type");
  EXPECT_NE(parsed.error().message.find("room_id"), std::string::npos);
}

TEST(Parse, TreatsAnExplicitNullAsMissing) {
  const auto parsed = parse(R"({"type":"join_room","room_id":null,"user_id":"user1"})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "missing_field");
}

TEST(Parse, RejectsANonIntegerMlineIndex) {
  const auto parsed = parse(R"({"type":"ice_candidate","room_id":"8F42A1","from_user_id":"a",
    "to_user_id":"b","candidate":"c","sdp_mid":"audio","sdp_mline_index":"1"})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "invalid_type");
}

TEST(Parse, RejectsANestedUserThatIsNotAnObject) {
  const auto parsed = parse(R"({"type":"user_joined","room_id":"8F42A1","user":"user1"})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "invalid_type");
}

TEST(Parse, RejectsANestedUserMissingItsId) {
  const auto parsed =
      parse(R"({"type":"user_joined","room_id":"8F42A1","user":{"display_name":"Ana"}})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "missing_field");
}

TEST(Parse, ReportsTheFirstFailureOnly) {
  const auto parsed = parse(R"({"type":"offer","room_id":123,"from_user_id":456})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_NE(parsed.error().message.find("room_id"), std::string::npos);
  EXPECT_EQ(parsed.error().message.find("from_user_id"), std::string::npos);
}

TEST(Parse, IgnoresUnknownFieldsSoTheProtocolCanEvolve) {
  const auto parsed =
      parse(R"({"type":"join_room","room_id":"8F42A1","user_id":"user1","future_field":true})");
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
}

TEST(Parse, OptionalFieldsMayBeAbsent) {
  const auto parsed = parse(R"({"type":"ping"})");
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  EXPECT_EQ(std::get<Ping>(parsed.value()).nonce, "");
}

// --- type_of -----------------------------------------------------------------

TEST(TypeOf, MatchesTheAlternativeHeld) {
  EXPECT_EQ(type_of(Message{Authenticate{}}), MessageType::Authenticate);
  EXPECT_EQ(type_of(Message{Authenticated{}}), MessageType::Authenticated);
  EXPECT_EQ(type_of(Message{CreateRoom{}}), MessageType::CreateRoom);
  EXPECT_EQ(type_of(Message{RoomCreated{}}), MessageType::RoomCreated);
  EXPECT_EQ(type_of(Message{JoinRoom{}}), MessageType::JoinRoom);
  EXPECT_EQ(type_of(Message{LeaveRoom{}}), MessageType::LeaveRoom);
  EXPECT_EQ(type_of(Message{UserJoined{}}), MessageType::UserJoined);
  EXPECT_EQ(type_of(Message{UserLeft{}}), MessageType::UserLeft);
  EXPECT_EQ(type_of(Message{Offer{}}), MessageType::Offer);
  EXPECT_EQ(type_of(Message{Answer{}}), MessageType::Answer);
  EXPECT_EQ(type_of(Message{IceCandidate{}}), MessageType::IceCandidate);
  EXPECT_EQ(type_of(Message{ScreenShareStarted{}}), MessageType::ScreenShareStarted);
  EXPECT_EQ(type_of(Message{ScreenShareStopped{}}), MessageType::ScreenShareStopped);
  EXPECT_EQ(type_of(Message{Mute{}}), MessageType::Mute);
  EXPECT_EQ(type_of(Message{Unmute{}}), MessageType::Unmute);
  EXPECT_EQ(type_of(Message{ErrorMessage{}}), MessageType::Error);
  EXPECT_EQ(type_of(Message{Ping{}}), MessageType::Ping);
  EXPECT_EQ(type_of(Message{Pong{}}), MessageType::Pong);
}

TEST(Protocol, ForceMuteRequiresADirection) {
  // The comment in the parser used to say this was required while the code
  // defaulted it to true, so a client that misspelled the field got a mute
  // rather than a refusal: the opposite of guessing nothing.
  const auto parsed =
      dv::protocol::parse(R"({"type":"force_mute","room_id":"8F42A1","user_id":"user123"})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "missing_field");
}

}  // namespace
