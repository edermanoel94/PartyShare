#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <variant>

#include <dv/core/result.hpp>
#include <dv/models/user.hpp>

/// Signaling protocol, section 13 of SPEC.md.
///
/// The wire format is a flat JSON object carrying a "type" discriminator:
///
///   {"type": "join_room", "room_id": "8F42A1", "user_id": "user123"}
///
/// The protocol is deliberately independent of this C++ representation, so the
/// server can be reimplemented in another language later (section 14 of
/// SPEC.md). docs/protocol.md is the normative description.
namespace dv::protocol {

/// Reserved participant identifier for the server's own media endpoint.
///
/// Media is routed through an SFU (section 12 of SPEC.md), so the far end of a
/// participant's PeerConnection is the server, not another participant. An
/// `offer`, `answer` or `ice_candidate` addressed to this identifier is
/// consumed by the server instead of being relayed, and what the server sends
/// back carries it as `from_user_id`.
///
/// It cannot collide with a real participant: user identifiers are 16
/// hexadecimal characters assigned by the authenticator, and this is not one.
inline constexpr std::string_view kSfuUserId = "sfu";

enum class MessageType : std::uint8_t {
  Authenticate,
  Authenticated,
  CreateRoom,
  RoomCreated,
  JoinRoom,
  LeaveRoom,
  UserJoined,
  UserLeft,
  Offer,
  Answer,
  IceCandidate,
  ScreenShareStarted,
  ScreenShareStopped,
  Mute,
  Unmute,
  Error,
  Ping,
  Pong,
};

// --- client to server --------------------------------------------------------

/// The password never leaves the client in any other message, and the server
/// never echoes it back.
struct Authenticate {
  std::string username;
  std::string password;

  friend bool operator==(const Authenticate&, const Authenticate&) = default;
};

struct CreateRoom {
  std::string user_id;
  std::string room_name;

  friend bool operator==(const CreateRoom&, const CreateRoom&) = default;
};

struct JoinRoom {
  std::string room_id;
  std::string user_id;
  std::string display_name;

  friend bool operator==(const JoinRoom&, const JoinRoom&) = default;
};

struct LeaveRoom {
  std::string room_id;
  std::string user_id;

  friend bool operator==(const LeaveRoom&, const LeaveRoom&) = default;
};

// --- server to client --------------------------------------------------------

/// Carries the identity the server assigned, plus the session token that every
/// later message is checked against.
struct Authenticated {
  models::User user;
  std::string token;
  /// Seconds until the token stops being accepted.
  int expires_in_seconds = 0;

  friend bool operator==(const Authenticated&, const Authenticated&) = default;
};

struct RoomCreated {
  std::string room_id;
  std::string room_name;

  friend bool operator==(const RoomCreated&, const RoomCreated&) = default;
};

struct UserJoined {
  std::string room_id;
  models::User user;

  friend bool operator==(const UserJoined&, const UserJoined&) = default;
};

struct UserLeft {
  std::string room_id;
  std::string user_id;

  friend bool operator==(const UserLeft&, const UserLeft&) = default;
};

// --- WebRTC negotiation, relayed by the server -------------------------------

struct Offer {
  std::string room_id;
  std::string from_user_id;
  std::string to_user_id;
  std::string sdp;

  friend bool operator==(const Offer&, const Offer&) = default;
};

struct Answer {
  std::string room_id;
  std::string from_user_id;
  std::string to_user_id;
  std::string sdp;

  friend bool operator==(const Answer&, const Answer&) = default;
};

struct IceCandidate {
  std::string room_id;
  std::string from_user_id;
  std::string to_user_id;
  std::string candidate;
  std::string sdp_mid;
  int sdp_mline_index = 0;

  friend bool operator==(const IceCandidate&, const IceCandidate&) = default;
};

// --- state changes, sent in both directions ----------------------------------

struct ScreenShareStarted {
  std::string room_id;
  std::string user_id;

  friend bool operator==(const ScreenShareStarted&, const ScreenShareStarted&) = default;
};

struct ScreenShareStopped {
  std::string room_id;
  std::string user_id;

  friend bool operator==(const ScreenShareStopped&, const ScreenShareStopped&) = default;
};

struct Mute {
  std::string room_id;
  std::string user_id;

  friend bool operator==(const Mute&, const Mute&) = default;
};

struct Unmute {
  std::string room_id;
  std::string user_id;

  friend bool operator==(const Unmute&, const Unmute&) = default;
};

// --- transport level ---------------------------------------------------------

/// Named ErrorMessage to avoid colliding with dv::Error, which represents a
/// local failure rather than a protocol frame.
struct ErrorMessage {
  std::string code;
  std::string message;

  friend bool operator==(const ErrorMessage&, const ErrorMessage&) = default;
};

struct Ping {
  std::string nonce;

  friend bool operator==(const Ping&, const Ping&) = default;
};

struct Pong {
  std::string nonce;

  friend bool operator==(const Pong&, const Pong&) = default;
};

using Message =
    std::variant<Authenticate, Authenticated, CreateRoom, RoomCreated, JoinRoom, LeaveRoom,
                 UserJoined, UserLeft, Offer, Answer, IceCandidate, ScreenShareStarted,
                 ScreenShareStopped, Mute, Unmute, ErrorMessage, Ping, Pong>;

/// The wire name of a message type, for example "join_room".
[[nodiscard]] std::string_view type_name(MessageType type) noexcept;

/// Inverse of type_name. Returns nothing for an unknown name.
[[nodiscard]] std::optional<MessageType> type_from_name(std::string_view name) noexcept;

[[nodiscard]] MessageType type_of(const Message& message) noexcept;

/// Never fails: every Message is representable on the wire.
[[nodiscard]] std::string serialize(const Message& message);

/// Parses a single frame. Failure codes:
///   invalid_json           the payload is not valid JSON, or is not an object
///   missing_field          a required field is absent
///   invalid_type           a field is present with the wrong JSON type
///   unknown_message_type   the "type" value is not part of this protocol
[[nodiscard]] Result<Message> parse(std::string_view json_text);

}  // namespace dv::protocol
