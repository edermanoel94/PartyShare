#include <gtest/gtest.h>

#include <dv/models/user.hpp>
#include <dv/protocol/message.hpp>

#include "signaling/permissions.hpp"

namespace {

using dv::models::Role;
using dv::server::Access;
using dv::server::access_for;
using dv::server::is_allowed;

namespace proto = dv::protocol;

TEST(Permissions, AnOrdinaryUserCanDoEverythingACallNeeds) {
  // The whole vocabulary of a participant. If any of these ever needs an
  // administrator, a normal call has stopped working.
  for (const proto::MessageType type : {
           proto::MessageType::CreateRoom,
           proto::MessageType::JoinRoom,
           proto::MessageType::LeaveRoom,
           proto::MessageType::Offer,
           proto::MessageType::Answer,
           proto::MessageType::IceCandidate,
           proto::MessageType::ScreenShareStarted,
           proto::MessageType::ScreenShareStopped,
           proto::MessageType::Mute,
           proto::MessageType::Unmute,
           proto::MessageType::ChatMessage,
           proto::MessageType::ListChat,
           proto::MessageType::Ping,
           proto::MessageType::Pong,
       }) {
    EXPECT_TRUE(is_allowed(Role::User, type)) << proto::type_name(type);
    EXPECT_TRUE(is_allowed(Role::Admin, type)) << proto::type_name(type);
  }
}

TEST(Permissions, AdministrationIsRefusedToAnOrdinaryUser) {
  for (const proto::MessageType type : {
           proto::MessageType::KickUser,
           proto::MessageType::ForceMute,
           proto::MessageType::RestrictUser,
           proto::MessageType::ListUsers,
           proto::MessageType::CreateUser,
           proto::MessageType::UpdateUser,
           proto::MessageType::DeleteUser,
           proto::MessageType::ListRooms,
           proto::MessageType::DeleteRoom,
           proto::MessageType::ListAudit,
       }) {
    EXPECT_EQ(access_for(type), Access::AdminOnly) << proto::type_name(type);
    EXPECT_FALSE(is_allowed(Role::User, type)) << proto::type_name(type);
    EXPECT_TRUE(is_allowed(Role::Admin, type)) << proto::type_name(type);
  }
}

TEST(Permissions, AnnouncementsAreRefusedToEverybody) {
  // These are what the server says, not what it accepts. An administrator
  // sending one is as much a client bug as anybody else doing it, so the role
  // makes no difference here.
  for (const proto::MessageType type : {
           proto::MessageType::Authenticated,
           proto::MessageType::RoomCreated,
           proto::MessageType::UserJoined,
           proto::MessageType::UserLeft,
           proto::MessageType::UserKicked,
           proto::MessageType::UserRestricted,
           proto::MessageType::ChatHistory,
           proto::MessageType::UserList,
           proto::MessageType::RoomList,
           proto::MessageType::AuditList,
           proto::MessageType::Error,
       }) {
    EXPECT_EQ(access_for(type), Access::ServerToClient) << proto::type_name(type);
    EXPECT_FALSE(is_allowed(Role::User, type)) << proto::type_name(type);
    EXPECT_FALSE(is_allowed(Role::Admin, type)) << proto::type_name(type);
  }
}

TEST(Permissions, ReadingAConversationIsNotAnAdministrativePower) {
  // An administrator manages accounts, rooms and who is in them. What people
  // said to each other is not on that list, and `list_chat` is answered for
  // participants of the room and refused to everybody else regardless of role.
  // The table only says the message may be sent; Hub::handle_list_chat is what
  // narrows it, and this is the half that belongs here.
  EXPECT_EQ(access_for(proto::MessageType::ListChat), Access::Authenticated);
  EXPECT_EQ(access_for(proto::MessageType::ChatMessage), Access::Authenticated);
}

TEST(Permissions, AValueOutsideTheEnumGrantsNothing) {
  // A cast from an integer is the one way to reach the table with something it
  // does not know about. The answer has to be the role that can do least,
  // rather than whatever the switch happened to fall through to.
  const auto invented = static_cast<proto::MessageType>(200);
  EXPECT_EQ(access_for(invented), Access::ServerToClient);
  EXPECT_FALSE(is_allowed(Role::Admin, invented));
}

TEST(Permissions, EveryMessageTypeIsClassified) {
  // The switch in access_for covers the enum, so this cannot fail while the
  // build is warning free. It is here for the case where somebody silences
  // that warning: an unclassified type would then answer ServerToClient by
  // falling off the end, and a message nobody can send is a feature that
  // quietly does nothing.
  //
  // Walks by name rather than by value, because type_from_name only answers
  // for names the protocol actually defines.
  for (int raw = 0; raw <= static_cast<int>(proto::MessageType::AuditList); ++raw) {
    const auto type = static_cast<proto::MessageType>(raw);
    EXPECT_NE(proto::type_name(type), "unknown") << "message type " << raw << " has no wire name";
  }
}

}  // namespace
