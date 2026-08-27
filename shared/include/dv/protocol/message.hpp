#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <dv/core/result.hpp>
#include <dv/models/audit.hpp>
#include <dv/models/chat.hpp>
#include <dv/models/user.hpp>

/// Signaling protocol, section 13 of SPEC.md.
///
/// The wire format is a flat JSON object carrying a "type" discriminator:
///
///   {"type": "join_room", "room_id": "8F42A1", "user_id": "user123"}
///
/// The protocol is deliberately independent of this C++ representation, so the
/// server can be reimplemented in another language later (section 14 of
/// SPEC.md). docs/06-protocol.md is the normative description.
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
  // An account looking after itself, section 4.8 of docs/06-protocol.md. Not
  // administration: the only account either of these can touch is the one that
  // sent the message.
  ChangePassword,
  PasswordChanged,
  // The room's conversation, section 4.5 of docs/06-protocol.md.
  ChatMessage,
  ListChat,
  ChatHistory,
  Error,
  Ping,
  Pong,
  // Administration, section 18 of SPEC.md. Every one of these is refused for a
  // connection whose account is not an administrator, by the table in
  // server/src/signaling/permissions.hpp.
  KickUser,
  UserKicked,
  ForceMute,
  RestrictUser,
  UserRestricted,
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

/// Replaces the password of the account that sent it.
///
/// There is no `user_id`, and that absence is the whole security argument for
/// this being its own message rather than an `update_user` opened up to
/// ordinary users. The account acted on is the one the connection's token
/// resolves to, so there is no field an attacker could aim somewhere else. A
/// message that named its target would need a rule saying the target must be
/// the sender, and a rule can be forgotten by the next handler; a message with
/// nowhere to write a target cannot be pointed at anybody.
///
/// `current_password` is required and checked. Without it, an unattended
/// session - a laptop left open for two minutes - is enough to lock the owner
/// out of their own account permanently, which is a bigger hole than the one
/// changing a password is meant to close.
struct ChangePassword {
  std::string current_password;
  std::string new_password;

  friend bool operator==(const ChangePassword&, const ChangePassword&) = default;
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

/// The password was replaced, and with it every session of that account - the
/// one that asked included.
///
/// Carries nothing. The client's only job on receiving it is to sign out and
/// say why, and anything this message could add about the account is already
/// stale: the token that would have been used to ask for it has just been
/// revoked.
struct PasswordChanged {
  friend bool operator==(const PasswordChanged&, const PasswordChanged&) = default;
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
  /// Whether what the sharer's machine is playing is going out with the
  /// picture.
  ///
  /// Nothing depends on this to hear the sound: it rides in the sharer's own
  /// audio track and arrives whether or not anybody was told. What it buys is
  /// the interface being able to say so - and, less obviously, being able to
  /// explain why the volume slider for that participant now controls two
  /// things at once. See docs/09-screen-audio.md, section 3.
  ///
  /// Absent from an older peer's message, and false is the right reading of
  /// that: a client that does not know about this cannot be sending it.
  bool has_audio = false;

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

// --- chat --------------------------------------------------------------------

/// One line of the room's conversation.
///
/// Travels in both directions, like the state changes above. From a client it
/// is a request; what the server broadcasts to the whole room, the sender
/// included, is the confirmation, and only then does anybody display it. That
/// is what makes every participant see the same messages in the same order,
/// with the same identifiers, rather than each client's own guess at both.
///
/// `message.id`, `message.display_name` and `message.timestamp_seconds` belong
/// to the server. They are ignored on the way in, for the same reason
/// `by_user_id` is on a mute: a participant does not get to decide when their
/// message was sent, what it is called, or whose name is on it.
///
/// The payload is a nested object rather than flat fields so that a live
/// message and one read back out of `chat_history` are the same object, and a
/// client needs one reader for both.
struct ChatMessage {
  models::ChatMessage message;

  friend bool operator==(const ChatMessage&, const ChatMessage&) = default;
};

/// Asks for what was said in a room. Open to any participant of that room, and
/// refused with `not_in_room` to anybody else: a conversation is readable by
/// the people it happened in front of.
struct ListChat {
  std::string room_id;
  /// How many of the most recent messages, capped by the server. Zero or
  /// absent asks for the default.
  int limit = 0;

  friend bool operator==(const ListChat&, const ListChat&) = default;
};

/// The answer to `list_chat`, and what a participant is sent as they join, so
/// that they arrive into a conversation rather than into an empty panel.
///
/// Oldest first, which is the order it is read in. The window is the newest
/// `limit` messages of the room, so the last element is always the most recent
/// thing said.
struct ChatHistory {
  std::string room_id;
  std::vector<models::ChatMessage> messages;

  friend bool operator==(const ChatHistory&, const ChatHistory&) = default;
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

/// Takes something away from an account, or gives it back, for longer than the
/// room they are in lasts. See models::Restrictions.
///
/// Every flag is optional and absent means unchanged, for the same reason
/// `update_user` spells its fields that way: lifting a ban and leaving it alone
/// have to be different requests, and a message that carried four plain
/// booleans would make every change a claim about all four. An administrator
/// who unmutes somebody would silently be lifting the ban a colleague applied a
/// minute earlier.
///
/// The answer is the whole `user_list`, as with the other account changes, plus
/// a `user_restricted` to the room the account is in and to the account itself.
struct RestrictUser {
  std::string user_id;
  std::optional<bool> banned;
  std::optional<bool> muted;
  std::optional<bool> silenced;
  std::optional<bool> screen_share_blocked;
  /// Shown to the person it is about. Optional, and empty is fine.
  std::string reason;

  friend bool operator==(const RestrictUser&, const RestrictUser&) = default;
};

/// What an account's restrictions became, announced rather than asked about.
///
/// Sent to the room the account is in, everybody in it included, so that a
/// microphone that will not turn back on has a reason next to it rather than
/// looking broken. Sent to the account's own connection whether or not they are
/// in a room, because a person told what they may no longer do can stop trying,
/// and one who is not told is left clicking a button that does nothing.
///
/// It carries the whole set and not what changed. A client that missed one of
/// these while it was reconnecting is then correct again on the next one,
/// instead of applying a delta to a state nobody agreed on.
struct UserRestricted {
  std::string user_id;
  models::Restrictions restrictions;
  /// The administrator who did it. Empty is not expected here, but a client
  /// renders "an administrator" for it rather than nothing.
  std::string by_user_id;
  std::string reason;
  /// Where they were when it happened, and empty when they were in no room.
  std::string room_id;

  friend bool operator==(const UserRestricted&, const UserRestricted&) = default;
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
                 ScreenShareStopped, Mute, Unmute, ChangePassword, PasswordChanged, ChatMessage,
                 ListChat, ChatHistory, ErrorMessage, Ping, Pong, KickUser, UserKicked, ForceMute,
                 RestrictUser, UserRestricted, ListUsers, UserList, CreateUser, UpdateUser,
                 DeleteUser, ListRooms, RoomList, DeleteRoom, ListAudit, AuditList>;

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
