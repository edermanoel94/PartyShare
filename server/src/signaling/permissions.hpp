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
    case protocol::MessageType::Ping:
    case protocol::MessageType::Pong:
      return Access::Authenticated;

    // Administration.
    case protocol::MessageType::KickUser:
    case protocol::MessageType::ForceMute:
    case protocol::MessageType::ListUsers:
    case protocol::MessageType::CreateUser:
    case protocol::MessageType::UpdateUser:
    case protocol::MessageType::DeleteUser:
    case protocol::MessageType::ListRooms:
    case protocol::MessageType::DeleteRoom:
    case protocol::MessageType::ListAudit:
      return Access::AdminOnly;

    // Answers and announcements. A client sending one of these is confused,
    // and is told so rather than ignored.
    case protocol::MessageType::Authenticated:
    case protocol::MessageType::RoomCreated:
    case protocol::MessageType::UserJoined:
    case protocol::MessageType::UserLeft:
    case protocol::MessageType::UserKicked:
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
