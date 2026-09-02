#pragma once

#include <dv/models/user.hpp>
#include <dv/protocol/message.hpp>

namespace dv::server {

/// What each role is allowed to send, section 18 of SPEC.md.
///
/// One table, consulted once, in Hub::on_message. The alternative is a check
/// at the top of every handler, and the trouble with that is not the
/// repetition: it is that a handler added later without one looks exactly like
/// a handler that is meant to be open to everybody. Here, a message type that
/// nobody classified is a compile error, because the switch below covers the
/// enum and the build treats a missing case as a warning that is an error.
///
/// This says nothing about *which* user may be acted on. That a participant
/// may only mute themselves, that an administrator may not delete their own
/// account, and that the last administrator may not be demoted are rules about
/// the arguments rather than about the message, and they live in the handlers.
enum class Access : std::uint8_t {
  /// Any authenticated connection.
  Authenticated,
  /// Administrators only.
  AdminOnly,
  /// The server sends it and never accepts it. Receiving one is a client bug.
  ServerToClient,
};

[[nodiscard]] constexpr Access access_for(protocol::MessageType type) noexcept {
  switch (type) {
    // What anybody who has logged in may do. This is the whole of the ordinary
    // user's vocabulary: join a room, create one, negotiate media, share a
    // screen, mute themselves.
    case protocol::MessageType::Authenticate:
    case protocol::MessageType::CreateRoom:
    case protocol::MessageType::JoinRoom:
    case protocol::MessageType::LeaveRoom:
    case protocol::MessageType::Offer:
    case protocol::MessageType::Answer:
    case protocol::MessageType::IceCandidate:
    case protocol::MessageType::ScreenShareStarted:
    case protocol::MessageType::ScreenShareStopped:
    case protocol::MessageType::Mute:
    case protocol::MessageType::Unmute:
    // Replacing one's own password. Deliberately here and not in the
    // administration block below: an ordinary user who cannot change their own
    // password has to ask an administrator, which means saying the new one out
    // loud to somebody, and the administrator ends up knowing a password they
    // have no business knowing.
    //
    // Nothing about this grants power over another account. The message has no
    // target field at all - see protocol::ChangePassword - so "any
    // authenticated connection may send it" and "any authenticated connection
    // may change its own password" are the same sentence here.
    case protocol::MessageType::ChangePassword:
    // Talking in a room and reading what was said in it. Open to everybody who
    // has logged in, and narrowed to the room's own participants by the
    // handlers: this table says what a role may send, not who they may send it
    // about.
    case protocol::MessageType::ChatMessage:
    case protocol::MessageType::ListChat:
    // Saying you read what an administrator sent you. Deliberately here and
    // not in the administration block: a notice is a box on somebody's screen
    // that only their own answer takes away, and an ordinary user who could
    // not send this would be looking at that box forever.
    //
    // Nothing about it grants power over another account. It names a notice
    // and not a person, and the handler accepts it only for a notice addressed
    // to the connection's own account, so a client that guesses somebody
    // else's identifier is refused as though it had guessed nothing.
    case protocol::MessageType::AcknowledgeNotice:
    case protocol::MessageType::Ping:
    case protocol::MessageType::Pong:
    // Which rooms exist, and how many people are in each. This was
    // administration for as long as the only place a room list appeared was an
    // administrator's panel, and knowing a room existed meant having been told
    // its six characters. The list is now the first thing anybody sees after
    // signing in, which is the whole point of it: somebody who has to be told
    // a code before they can go anywhere has no use for a list.
    //
    // Closing a room stays administration. Seeing one and ending one are not
    // the same power.
    case protocol::MessageType::ListRooms:
      return Access::Authenticated;

    // Administration.
    case protocol::MessageType::KickUser:
    case protocol::MessageType::ForceMute:
    case protocol::MessageType::RestrictUser:
    case protocol::MessageType::ListUsers:
    case protocol::MessageType::CreateUser:
    case protocol::MessageType::UpdateUser:
    case protocol::MessageType::DeleteUser:
    case protocol::MessageType::DeleteRoom:
    case protocol::MessageType::ListAudit:
    // Telling one account something, in a box they have to dismiss. It reaches
    // a person who is in no room and, if they are not connected, a person who
    // is not there at all, which is exactly the reach that makes it
    // administration rather than a message.
    case protocol::MessageType::SendNotice:
      return Access::AdminOnly;

    // Answers and announcements. A client sending one of these is confused,
    // and is told so rather than ignored.
    case protocol::MessageType::Authenticated:
    case protocol::MessageType::RoomCreated:
    case protocol::MessageType::UserJoined:
    case protocol::MessageType::UserLeft:
    case protocol::MessageType::UserKicked:
    case protocol::MessageType::UserRestricted:
    case protocol::MessageType::PasswordChanged:
    case protocol::MessageType::ChatHistory:
    case protocol::MessageType::Notice:
    case protocol::MessageType::UserList:
    case protocol::MessageType::RoomList:
    case protocol::MessageType::AuditList:
    case protocol::MessageType::Error:
      return Access::ServerToClient;
  }

  // Unreachable for any value of the enum. A cast from an integer could still
  // land here, and the answer to a value nobody defined is the one that grants
  // nothing.
  return Access::ServerToClient;
}

/// Whether `role` may send `type`.
[[nodiscard]] constexpr bool is_allowed(models::Role role, protocol::MessageType type) noexcept {
  switch (access_for(type)) {
    case Access::Authenticated:
      return true;
    case Access::AdminOnly:
      return role == models::Role::Admin;
    case Access::ServerToClient:
      return false;
  }
  return false;
}

}  // namespace dv::server
