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
      MessageType::ChatMessage,
      MessageType::ListChat,
      MessageType::ChatHistory,
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

TEST(RoundTrip, AScreenShareCarriesWhetherItHasSound) {
  const ScreenShareStarted loud{"8F42A1", "user1", true};
  EXPECT_EQ(round_trip(loud), loud);
  EXPECT_TRUE(round_trip(loud).has_audio);

  const ScreenShareStarted silent{"8F42A1", "user1", false};
  EXPECT_FALSE(round_trip(silent).has_audio);
}

TEST(Parse, AScreenShareFromAPeerThatPredatesSoundIsSilent) {
  // The field is new. A client built before it cannot be sending it, and
  // "absent" has to read as "no sound" rather than as a malformed message -
  // otherwise an older participant's share stops being announced at all.
  const auto parsed = parse(R"({
    "type": "screen_share_started",
    "room_id": "8F42A1",
    "user_id": "user1"
  })");
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  ASSERT_TRUE(std::holds_alternative<ScreenShareStarted>(parsed.value()));
  EXPECT_FALSE(std::get<ScreenShareStarted>(parsed.value()).has_audio);
}

TEST(RoundTrip, MuteAndUnmute) {
  EXPECT_EQ(round_trip(Mute{"8F42A1", "user1"}), (Mute{"8F42A1", "user1"}));
  EXPECT_EQ(round_trip(Unmute{"8F42A1", "user1"}), (Unmute{"8F42A1", "user1"}));
}

TEST(RoundTrip, ChatMessage) {
  const ChatMessage original{.message = {.id = "42",
                                         .room_id = "8F42A1",
                                         .user_id = "user1",
                                         .display_name = "Ana",
                                         .text = "the build is green",
                                         .timestamp_seconds = 1755676800}};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, ChatHistory) {
  const ChatHistory original{.room_id = "8F42A1",
                             .messages = {
                                 {.id = "1",
                                  .room_id = "8F42A1",
                                  .user_id = "user1",
                                  .display_name = "Ana",
                                  .text = "morning",
                                  .timestamp_seconds = 1755676800},
                                 {.id = "2",
                                  .room_id = "8F42A1",
                                  .user_id = "user2",
                                  .display_name = "Bruno",
                                  .text = "morning",
                                  .timestamp_seconds = 1755676860},
                             }};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, AChatMessageCarriesEmoji) {
  // Not a curiosity: this is the first field of the protocol somebody types
  // for fun rather than for the server, so it is the one that will be full of
  // characters outside the basic plane. Four bytes each in UTF-8 and a
  // surrogate pair in UTF-16, which is where a JSON library that escapes on
  // its own initiative would show up.
  const ChatMessage original{.message = {.id = "42",
                                         .room_id = "8F42A1",
                                         .user_id = "user1",
                                         .display_name = "Ana 🌟",
                                         .text = "deploy feito 🚀🎉 tudo verde ✅",
                                         .timestamp_seconds = 1755676800}};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, AnEmptyChatHistoryStaysAnArray) {
  // A room where nobody has spoken yet. The field has to survive as an empty
  // array rather than disappearing, because a client that finds it missing
  // would answer `missing_field` to a perfectly ordinary reply.
  const ChatHistory original{.room_id = "8F42A1"};
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, ListChat) {
  const ListChat original{.room_id = "8F42A1", .limit = 25};
  EXPECT_EQ(round_trip(original), original);
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
  EXPECT_EQ(type_of(Message{ChatMessage{}}), MessageType::ChatMessage);
  EXPECT_EQ(type_of(Message{ListChat{}}), MessageType::ListChat);
  EXPECT_EQ(type_of(Message{ChatHistory{}}), MessageType::ChatHistory);
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

TEST(RoundTrip, RestrictUser) {
  RestrictUser original;
  original.user_id = "user123";
  original.banned = true;
  original.silenced = false;
  original.reason = "reading the room out loud";
  // `muted` and `screen_share_blocked` are left absent on purpose: the point of
  // the type is that "leave this alone" survives the wire as a different thing
  // from "set this to false".
  const RestrictUser back = round_trip(original);
  EXPECT_EQ(back, original);
  EXPECT_FALSE(back.muted.has_value());
  EXPECT_FALSE(back.screen_share_blocked.has_value());
  ASSERT_TRUE(back.silenced.has_value());
  EXPECT_FALSE(*back.silenced);
}

TEST(Protocol, AnAbsentRestrictionFlagIsNotFalse) {
  // The failure this guards against is silent: an administrator lifting a mute
  // would also lift the ban a colleague applied a minute earlier, and the only
  // sign of it would be somebody being able to log in again.
  const auto parsed = parse(R"({"type":"restrict_user","user_id":"user123","muted":true})");
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  const auto& value = std::get<RestrictUser>(parsed.value());
  ASSERT_TRUE(value.muted.has_value());
  EXPECT_TRUE(*value.muted);
  EXPECT_FALSE(value.banned.has_value());
  EXPECT_FALSE(value.silenced.has_value());
  EXPECT_FALSE(value.screen_share_blocked.has_value());

  // And what goes out again carries only what was asked about.
  const std::string text = serialize(parsed.value());
  EXPECT_NE(text.find("\"muted\":true"), std::string::npos) << text;
  EXPECT_EQ(text.find("\"banned\""), std::string::npos) << text;
}

TEST(RoundTrip, UserRestricted) {
  UserRestricted original;
  original.user_id = "user123";
  original.restrictions = dv::models::Restrictions{
      .banned = false, .muted = true, .silenced = true, .screen_share_blocked = false};
  original.by_user_id = "admin1";
  original.reason = "for the rest of the meeting";
  original.room_id = "8F42A1";
  EXPECT_EQ(round_trip(original), original);
}

TEST(RoundTrip, AUserCarriesTheirRestrictions) {
  dv::models::User user{"user123", "Ana", ""};
  user.restrictions.silenced = true;
  user.restrictions.screen_share_blocked = true;

  const UserJoined original{"8F42A1", user};
  const UserJoined back = round_trip(original);
  EXPECT_EQ(back, original);
  EXPECT_TRUE(back.user.restrictions.silenced);
  EXPECT_TRUE(back.user.restrictions.screen_share_blocked);
  EXPECT_FALSE(back.user.restrictions.banned);
}

TEST(Protocol, AUserWithoutRestrictionsHasNothingTakenAway) {
  // What a client or a database from before restrictions existed sends. It
  // must not acquire a ban by omission, for the same reason omitting `role`
  // does not make somebody an administrator.
  const auto parsed = parse(
      R"({"type":"user_joined","room_id":"8F42A1","user":{"id":"user123","display_name":"Ana"}})");
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;
  const auto& value = std::get<UserJoined>(parsed.value());
  EXPECT_FALSE(value.user.restrictions.any());
}

TEST(Protocol, AChatMessageNeedsItsPayload) {
  const auto parsed = parse(R"({"type":"chat_message"})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "missing_field");
  EXPECT_EQ(parsed.error().message, "message is required");
}

TEST(Protocol, AChatMessageWithoutTextIsRefused) {
  const auto parsed =
      parse(R"({"type":"chat_message","message":{"room_id":"8F42A1","user_id":"user1"}})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "missing_field");
  // Reported against the nested object's own key, which is the one the sender
  // can place. See FieldReader::object.
  EXPECT_EQ(parsed.error().message, "message text is required");
}

TEST(Protocol, AChatMessageMayOmitWhatTheServerFillsIn) {
  // What a client actually sends: a room, itself, and what it wants to say.
  // The identifier, the name and the time are the server's.
  const auto parsed = parse(
      R"({"type":"chat_message","message":{"room_id":"8F42A1","user_id":"user1","text":"hi"}})");
  ASSERT_TRUE(parsed.ok()) << parsed.error().message;

  const auto& chat = std::get<ChatMessage>(parsed.value());
  EXPECT_EQ(chat.message.text, "hi");
  EXPECT_TRUE(chat.message.id.empty());
  EXPECT_TRUE(chat.message.display_name.empty());
  EXPECT_EQ(chat.message.timestamp_seconds, 0);
}

TEST(Protocol, AChatPayloadThatIsNotAnObjectIsRefused) {
  const auto parsed = parse(R"({"type":"chat_message","message":"hello"})");
  ASSERT_FALSE(parsed.ok());
  EXPECT_EQ(parsed.error().code, "invalid_type");
}

}  // namespace
