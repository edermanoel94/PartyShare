#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <dv/core/result.hpp>
#include <dv/models/audit.hpp>
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
  // Administration, section 18 of SPEC.md. Every one of these is refused for a
  // connection whose account is not an administrator, by the table in
  // server/src/signaling/permissions.hpp.
  KickUser,
  UserKicked,
  ForceMute,
  ListUsers,
  UserList,
  CreateUser,
  UpdateUser,
  DeleteUser,
  ListRooms,
  RoomList,
  DeleteRoom,
  ListAudit,
  AuditList,
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
  /// Asks for a room that survives its last participant leaving. Accepted only
  /// from an administrator; anyone else asking for one is refused rather than
  /// quietly given an ordinary room, because a room that silently forgets it
  /// was meant to be permanent is worse than a refusal.
  bool persistent = false;

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

/// Sent by a participant about themselves, and by the server to confirm it to
/// the room.
///
/// `by_user_id` is empty in the client to server direction and ignored if it
/// is not: a participant does not get to claim who muted them. The server
/// fills it in when the mute came from an administrator, which is what lets
/// the interface say "muted by Ana" rather than leaving a microphone that
/// turned itself off.
struct Mute {
  std::string room_id;
  std::string user_id;
  std::string by_user_id;

  friend bool operator==(const Mute&, const Mute&) = default;
};

struct Unmute {
  std::string room_id;
  std::string user_id;
  std::string by_user_id;

  friend bool operator==(const Unmute&, const Unmute&) = default;
};

// --- administration ----------------------------------------------------------
//
// Everything below is refused unless the connection authenticated as an
// administrator. The check lives in one table rather than in each handler, see
// server/src/signaling/permissions.hpp.

/// Removes a participant from a room. They stay connected and authenticated,
/// so they can join another room, or the same one again if the administrator
/// has not also disabled the account.
struct KickUser {
  std::string room_id;
  std::string user_id;
  /// Shown to the person being removed. Optional, and empty is fine.
  std::string reason;

  friend bool operator==(const KickUser&, const KickUser&) = default;
};

/// Announced to the whole room, the person removed included: they learn it
/// from the same message everyone else does, so there is no state where the
/// room has forgotten them and they have not noticed.
struct UserKicked {
  std::string room_id;
  std::string user_id;
  std::string reason;

  friend bool operator==(const UserKicked&, const UserKicked&) = default;
};

/// Mutes or unmutes somebody else. Confirmed to the room as an ordinary `mute`
/// or `unmute` carrying `by_user_id`, so a client that knows nothing about
/// administration still renders the result correctly.
struct ForceMute {
  std::string room_id;
  std::string user_id;
  bool muted = true;

  friend bool operator==(const ForceMute&, const ForceMute&) = default;
};

/// An account as an administrator sees it.
///
/// Deliberately not the server's own account record: that one holds the salt
/// and the password hash, and the way to be sure neither is ever serialized is
/// for the type that crosses the wire not to have them at all.
struct UserSummary {
  models::User user;
  std::string username;
  /// Seconds since the Unix epoch, UTC. Zero when the store does not know.
  std::int64_t created_at = 0;
  /// Whether they have a connection right now.
  bool online = false;

  friend bool operator==(const UserSummary&, const UserSummary&) = default;
};

struct RoomSummary {
  std::string id;
  std::string name;
  std::string owner_id;
  bool persistent = false;
  int participant_count = 0;

  friend bool operator==(const RoomSummary&, const RoomSummary&) = default;
};

struct ListUsers {
  friend bool operator==(const ListUsers&, const ListUsers&) = default;
};

/// The answer to `list_users`, and also what the server sends back after
/// `create_user`, `update_user` and `delete_user`. Answering a mutation with
/// the new state costs one message instead of two and leaves no window in
/// which the panel shows something that is no longer true.
struct UserList {
  std::vector<UserSummary> users;

  friend bool operator==(const UserList&, const UserList&) = default;
};

struct CreateUser {
  std::string username;
  std::string password;
  std::string display_name;
  models::Role role = models::Role::User;

  friend bool operator==(const CreateUser&, const CreateUser&) = default;
};

/// Every field but the identifier is optional, and absent means unchanged.
/// That is why they are `std::optional` rather than empty strings: clearing a
/// display name and not touching it have to be different requests.
struct UpdateUser {
  std::string user_id;
  std::optional<models::Role> role;
  std::optional<std::string> display_name;
  std::optional<std::string> password;

  friend bool operator==(const UpdateUser&, const UpdateUser&) = default;
};

struct DeleteUser {
  std::string user_id;

  friend bool operator==(const DeleteUser&, const DeleteUser&) = default;
};

struct ListRooms {
  friend bool operator==(const ListRooms&, const ListRooms&) = default;
};

struct RoomList {
  std::vector<RoomSummary> rooms;

  friend bool operator==(const RoomList&, const RoomList&) = default;
};

/// Closes a room. Everyone in it is removed exactly as a kick removes one
/// person, and a persistent room is forgotten by the store as well.
struct DeleteRoom {
  std::string room_id;

  friend bool operator==(const DeleteRoom&, const DeleteRoom&) = default;
};

struct ListAudit {
  /// Newest first, capped by the server. Zero or absent asks for the default.
  int limit = 0;
  /// Empty means every actor.
  std::string actor_id;

  friend bool operator==(const ListAudit&, const ListAudit&) = default;
};

struct AuditList {
  std::vector<models::AuditEntry> entries;

  friend bool operator==(const AuditList&, const AuditList&) = default;
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
                 ScreenShareStopped, Mute, Unmute, ErrorMessage, Ping, Pong, KickUser, UserKicked,
                 ForceMute, ListUsers, UserList, CreateUser, UpdateUser, DeleteUser, ListRooms,
                 RoomList, DeleteRoom, ListAudit, AuditList>;

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
