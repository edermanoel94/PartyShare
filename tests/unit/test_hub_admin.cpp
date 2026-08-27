#include <algorithm>
#include <chrono>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include <dv/models/user.hpp>
#include <dv/protocol/message.hpp>

#include "signaling/hub.hpp"

namespace {

using namespace std::chrono_literals;
using dv::models::Role;
using dv::protocol::Message;
using dv::server::ConnectionId;
using dv::server::Hub;
using dv::server::Outgoing;

namespace proto = dv::protocol;

/// Records what the Hub told the media layer, so that a kick can be checked to
/// tear the SFU connection down rather than only to change the room.
class RecordingMediaSignals final : public dv::server::MediaSignals {
 public:
  void on_participant_joined(const std::string& /*room_id*/, const std::string& /*room_name*/,
                             const dv::models::User& /*user*/,
                             const std::string& /*user_label*/) override {}

  void on_participant_left(const std::string& /*room_id*/, const std::string& user_id) override {
    left.push_back(user_id);
  }

  void on_media_signal(const std::string& /*room_id*/, const std::string& /*from_user_id*/,
                       const proto::Message& /*message*/) override {}

  std::vector<std::string> left;
};

class HubAdminTest : public ::testing::Test {
 protected:
  HubAdminTest() : hub_(Hub::Options{5, 5000ms, 15000ms, 4321u}) {
    hub_.set_media_signals(&media_);
  }

  ConnectionId connect() {
    const ConnectionId id = next_connection_++;
    hub_.on_connect(id, now_);
    return id;
  }

  std::vector<Outgoing> send(ConnectionId connection, const Message& message) {
    return hub_.on_message(connection, proto::serialize(message), now_);
  }

  std::pair<ConnectionId, dv::models::User> login(const std::string& username, Role role) {
    const auto registered = hub_.authenticator().add_user(username, "password", username, role);
    EXPECT_TRUE(registered.ok()) << registered.error().message;

    const ConnectionId connection = connect();
    const auto out = send(connection, proto::Authenticate{username, "password"});
    const auto authenticated = find<proto::Authenticated>(out, connection);
    EXPECT_TRUE(authenticated.has_value());
    return {connection, authenticated ? authenticated->user : dv::models::User{}};
  }

  /// An administrator and an ordinary user, both in one room. The shape almost
  /// every test below needs.
  void set_up_room() {
    std::tie(admin_connection_, admin_) = login("ana", Role::Admin);
    std::tie(user_connection_, user_) = login("bruno", Role::User);

    const auto created = find<proto::RoomCreated>(
        send(admin_connection_, proto::CreateRoom{admin_.id, ""}), admin_connection_);
    ASSERT_TRUE(created.has_value());
    room_ = created->room_id;

    (void)send(admin_connection_, proto::JoinRoom{room_, admin_.id, "Ana"});
    (void)send(user_connection_, proto::JoinRoom{room_, user_.id, "Bruno"});
  }

  template <typename T>
  static std::optional<T> find(const std::vector<Outgoing>& out, ConnectionId connection) {
    for (const Outgoing& outgoing : out) {
      if (outgoing.connection == connection && std::holds_alternative<T>(outgoing.message)) {
        return std::get<T>(outgoing.message);
      }
    }
    return std::nullopt;
  }

  RecordingMediaSignals media_;
  Hub hub_;
  ConnectionId next_connection_ = 1;
  Hub::Clock::time_point now_ = Hub::Clock::now();

  ConnectionId admin_connection_ = 0;
  ConnectionId user_connection_ = 0;
  dv::models::User admin_;
  dv::models::User user_;
  std::string room_;
};

// --- the gate ----------------------------------------------------------------

TEST_F(HubAdminTest, TheRoleReachesTheClientThatLoggedIn) {
  const auto [connection, user] = login("ana", Role::Admin);
  EXPECT_EQ(user.role, Role::Admin);
  EXPECT_TRUE(user.is_admin());

  const auto [other, plain] = login("bruno", Role::User);
  EXPECT_EQ(plain.role, Role::User);
}

TEST_F(HubAdminTest, AnOrdinaryUserIsRefusedEveryAdministrativeMessage) {
  set_up_room();

  for (const Message& message : std::vector<Message>{
           proto::KickUser{room_, admin_.id, "because"},
           proto::ForceMute{room_, admin_.id, true},
           proto::ListUsers{},
           proto::CreateUser{"carla", "password", "Carla", Role::Admin},
           proto::UpdateUser{admin_.id, Role::User, std::nullopt, std::nullopt},
           proto::DeleteUser{admin_.id},
           proto::DeleteRoom{room_},
           proto::ListAudit{},
       }) {
    const auto out = send(user_connection_, message);
    const auto error = find<proto::ErrorMessage>(out, user_connection_);
    ASSERT_TRUE(error.has_value()) << proto::type_name(proto::type_of(message));
    EXPECT_EQ(error->code, "forbidden") << proto::type_name(proto::type_of(message));
  }

  // And none of it happened: the administrator is still in the room, still
  // unmuted, and there is still an account for them.
  const auto* room = hub_.rooms().find(room_);
  ASSERT_NE(room, nullptr);
  ASSERT_TRUE(room->contains(admin_.id));
  EXPECT_FALSE(room->find(admin_.id)->muted);
}

TEST_F(HubAdminTest, DemotingAnAdministratorTakesEffectWithoutThemReconnecting) {
  set_up_room();
  const auto [second, other_admin] = login("carla", Role::Admin);

  // Ana is an administrator on a live, authenticated connection.
  ASSERT_TRUE(find<proto::UserList>(send(admin_connection_, proto::ListUsers{}), admin_connection_)
                  .has_value());

  // Carla demotes her while that connection stays open.
  const auto demoted =
      send(second, proto::UpdateUser{admin_.id, Role::User, std::nullopt, std::nullopt});
  ASSERT_TRUE(find<proto::UserList>(demoted, second).has_value());

  // The very next action on Ana's existing connection is refused. Without the
  // role being read back from the store, this would keep working until she
  // logged in again, which is not what revoking an administrator means.
  const auto out = send(admin_connection_, proto::ListUsers{});
  const auto error = find<proto::ErrorMessage>(out, admin_connection_);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "forbidden");
}

// --- kicking -----------------------------------------------------------------

TEST_F(HubAdminTest, AnAdministratorRemovesAParticipant) {
  set_up_room();

  const auto out = send(admin_connection_, proto::KickUser{room_, user_.id, "off topic"});

  // The person removed is told, and so is everybody else.
  const auto to_target = find<proto::UserKicked>(out, user_connection_);
  ASSERT_TRUE(to_target.has_value());
  EXPECT_EQ(to_target->user_id, user_.id);
  EXPECT_EQ(to_target->reason, "off topic");
  ASSERT_TRUE(find<proto::UserKicked>(out, admin_connection_).has_value());

  // The room agrees, and so does the media layer: without the second one the
  // SFU keeps forwarding the audio of somebody who is no longer in the room.
  const auto* room = hub_.rooms().find(room_);
  ASSERT_NE(room, nullptr);
  EXPECT_FALSE(room->contains(user_.id));
  EXPECT_EQ(media_.left, std::vector<std::string>{user_.id});
}

TEST_F(HubAdminTest, AKickedParticipantCanJoinAnotherRoom) {
  set_up_room();
  (void)send(admin_connection_, proto::KickUser{room_, user_.id, {}});

  // Removed from the room, not from the server: the connection is still
  // authenticated and still usable.
  const auto created = find<proto::RoomCreated>(
      send(user_connection_, proto::CreateRoom{user_.id, "elsewhere"}), user_connection_);
  ASSERT_TRUE(created.has_value());

  const auto joined = send(user_connection_, proto::JoinRoom{created->room_id, user_.id, "Bruno"});
  EXPECT_FALSE(find<proto::ErrorMessage>(joined, user_connection_).has_value());
}

TEST_F(HubAdminTest, AnAdministratorCannotKickThemselves) {
  set_up_room();

  const auto out = send(admin_connection_, proto::KickUser{room_, admin_.id, {}});
  const auto error = find<proto::ErrorMessage>(out, admin_connection_);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "invalid_target");

  const auto* room = hub_.rooms().find(room_);
  ASSERT_NE(room, nullptr);
  EXPECT_TRUE(room->contains(admin_.id));
}

TEST_F(HubAdminTest, KickingSomebodyWhoIsNotInTheRoomIsRefused) {
  set_up_room();
  const auto [third, carla] = login("carla", Role::User);

  const auto out = send(admin_connection_, proto::KickUser{room_, carla.id, {}});
  const auto error = find<proto::ErrorMessage>(out, admin_connection_);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "not_in_room");
}

// --- forced muting -----------------------------------------------------------

TEST_F(HubAdminTest, AForcedMuteSaysWhoDidIt) {
  set_up_room();

  const auto out = send(admin_connection_, proto::ForceMute{room_, user_.id, true});

  const auto muted = find<proto::Mute>(out, user_connection_);
  ASSERT_TRUE(muted.has_value());
  EXPECT_EQ(muted->user_id, user_.id);
  // The difference between a microphone that turned itself off and one that
  // was turned off for you.
  EXPECT_EQ(muted->by_user_id, admin_.id);

  const auto* room = hub_.rooms().find(room_);
  ASSERT_NE(room, nullptr);
  EXPECT_TRUE(room->find(user_.id)->muted);
}

TEST_F(HubAdminTest, MutingYourselfNamesNobody) {
  set_up_room();

  const auto out = send(user_connection_, proto::Mute{room_, user_.id, {}});
  const auto muted = find<proto::Mute>(out, user_connection_);
  ASSERT_TRUE(muted.has_value());
  EXPECT_TRUE(muted->by_user_id.empty());
}

TEST_F(HubAdminTest, AParticipantStillCannotMuteAnother) {
  set_up_room();

  // The rule the ForceMute message exists to leave alone: an ordinary `mute`
  // only ever speaks about its sender, whatever the sender's role.
  const auto out = send(user_connection_, proto::Mute{room_, admin_.id, {}});
  const auto error = find<proto::ErrorMessage>(out, user_connection_);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
}

TEST_F(HubAdminTest, AForcedMuteCannotBeUndoneByItsTarget) {
  set_up_room();
  (void)send(admin_connection_, proto::ForceMute{room_, user_.id, true});

  // The obvious way out, and the one that makes a forced mute worth nothing if
  // it works: the muted participant presses their own unmute button.
  const auto out = send(user_connection_, proto::Unmute{room_, user_.id, {}});
  const auto error = find<proto::ErrorMessage>(out, user_connection_);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "forbidden");
  EXPECT_TRUE(hub_.rooms().find(room_)->find(user_.id)->muted);

  // An administrator releasing it works, and leaves the participant free to
  // mute themselves again afterwards.
  (void)send(admin_connection_, proto::ForceMute{room_, user_.id, false});
  EXPECT_FALSE(hub_.rooms().find(room_)->find(user_.id)->muted);

  const auto self = send(user_connection_, proto::Mute{room_, user_.id, {}});
  EXPECT_FALSE(find<proto::ErrorMessage>(self, user_connection_).has_value());
  const auto release = send(user_connection_, proto::Unmute{room_, user_.id, {}});
  EXPECT_FALSE(find<proto::ErrorMessage>(release, user_connection_).has_value());
}

TEST_F(HubAdminTest, KickingSomebodyWhoWasSharingFreesTheFloor) {
  set_up_room();
  ASSERT_FALSE(
      find<proto::ErrorMessage>(send(user_connection_, proto::ScreenShareStarted{room_, user_.id}),
                                user_connection_)
          .has_value());

  const auto out = send(admin_connection_, proto::KickUser{room_, user_.id, {}});

  // Without this the remaining clients keep showing a share that ended, and
  // each of them refuses to start its own because it believes the floor is
  // still taken.
  const auto stopped = find<proto::ScreenShareStopped>(out, admin_connection_);
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->user_id, user_.id);

  // And the floor really is free: the administrator can now share.
  const auto shared = send(admin_connection_, proto::ScreenShareStarted{room_, admin_.id});
  EXPECT_FALSE(find<proto::ErrorMessage>(shared, admin_connection_).has_value());
}

TEST_F(HubAdminTest, ClosingARoomTellsWhoeverIsStillInItThatTheShareEnded) {
  set_up_room();
  const auto [third, carla] = login("carla", Role::User);
  (void)send(third, proto::JoinRoom{room_, carla.id, "Carla"});
  (void)send(user_connection_, proto::ScreenShareStarted{room_, user_.id});

  const auto out = send(admin_connection_, proto::DeleteRoom{room_});

  // Carla is still in the room when the sharer is removed, so she is told.
  const auto stopped = find<proto::ScreenShareStopped>(out, third);
  ASSERT_TRUE(stopped.has_value());
  EXPECT_EQ(stopped->user_id, user_.id);

  // The last participant out has nobody left to be told by, which is why the
  // client also releases the floor when it sees a user_kicked for whoever held
  // it. That half lives in CallSession and is covered there.
  EXPECT_EQ(hub_.rooms().find(room_), nullptr);
}

TEST_F(HubAdminTest, TheAuditLogNamesTheUsernameAndNotTheDisplayName) {
  // Two accounts, different usernames, deliberately the same display name.
  // The log has to say which of them acted.
  const auto registered = hub_.authenticator().add_user("ana", "password", "Ana", Role::Admin);
  ASSERT_TRUE(registered.ok());
  const auto other = hub_.authenticator().add_user("ana2", "password", "Ana", Role::Admin);
  ASSERT_TRUE(other.ok());

  const ConnectionId connection = connect();
  const auto session = find<proto::Authenticated>(
      send(connection, proto::Authenticate{"ana2", "password"}), connection);
  ASSERT_TRUE(session.has_value());

  (void)send(connection, proto::CreateUser{"carla", "password", "Carla", Role::User});

  const auto log = find<proto::AuditList>(send(connection, proto::ListAudit{}), connection);
  ASSERT_TRUE(log.has_value());
  ASSERT_EQ(log->entries.size(), 1U);
  EXPECT_EQ(log->entries.front().actor_username, "ana2");
}

TEST_F(HubAdminTest, AFailedPasswordChangeLeavesTheAccountAlone) {
  const auto [connection, admin] = login("ana", Role::Admin);
  const auto created = find<proto::UserList>(
      send(connection, proto::CreateUser{"bruno", "old", "Bruno", Role::User}), connection);
  ASSERT_TRUE(created.has_value());
  const std::string bruno = created->users.back().user.id;

  // An empty password is refused by the derivation. The role change riding
  // along in the same message must not be committed on its own: an
  // administrator who sees an error and a table that disagrees with it has no
  // way to know what actually happened.
  const auto out =
      send(connection, proto::UpdateUser{bruno, Role::Admin, std::nullopt, std::string{}});
  const auto error = find<proto::ErrorMessage>(out, connection);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "invalid_value");

  const auto list = find<proto::UserList>(send(connection, proto::ListUsers{}), connection);
  ASSERT_TRUE(list.has_value());
  const auto entry = std::ranges::find_if(
      list->users, [&](const proto::UserSummary& summary) { return summary.user.id == bruno; });
  ASSERT_NE(entry, list->users.end());
  EXPECT_EQ(entry->user.role, Role::User);

  // And the old password still works, so nothing was half applied.
  const ConnectionId fresh = connect();
  EXPECT_TRUE(find<proto::Authenticated>(send(fresh, proto::Authenticate{"bruno", "old"}), fresh)
                  .has_value());
}

TEST_F(HubAdminTest, AForcedUnmuteReleasesTheMicrophone) {
  set_up_room();
  (void)send(admin_connection_, proto::ForceMute{room_, user_.id, true});

  const auto out = send(admin_connection_, proto::ForceMute{room_, user_.id, false});
  const auto unmuted = find<proto::Unmute>(out, user_connection_);
  ASSERT_TRUE(unmuted.has_value());
  EXPECT_EQ(unmuted->by_user_id, admin_.id);
  EXPECT_FALSE(hub_.rooms().find(room_)->find(user_.id)->muted);
}

// --- accounts ----------------------------------------------------------------

TEST_F(HubAdminTest, TheAccountListNeverCarriesASecret) {
  const auto [connection, admin] = login("ana", Role::Admin);

  const auto list = find<proto::UserList>(send(connection, proto::ListUsers{}), connection);
  ASSERT_TRUE(list.has_value());
  ASSERT_EQ(list->users.size(), 1U);
  EXPECT_EQ(list->users.front().username, "ana");
  EXPECT_EQ(list->users.front().user.role, Role::Admin);
  EXPECT_TRUE(list->users.front().online);

  // Whatever else changes about the summary, the salt and the hash have no
  // field to travel in. Serializing it and looking for them is the check that
  // survives somebody adding a field later.
  const std::string wire = proto::serialize(*list);
  EXPECT_EQ(wire.find("password"), std::string::npos);
  EXPECT_EQ(wire.find("salt"), std::string::npos);
}

TEST_F(HubAdminTest, CreatingAnAccountAnswersWithTheNewList) {
  const auto [connection, admin] = login("ana", Role::Admin);

  const auto list = find<proto::UserList>(
      send(connection, proto::CreateUser{"bruno", "password", "Bruno", Role::User}), connection);
  ASSERT_TRUE(list.has_value());
  EXPECT_EQ(list->users.size(), 2U);

  // And the account works, which is the only proof that the password was
  // stored rather than merely accepted.
  const ConnectionId fresh = connect();
  EXPECT_TRUE(
      find<proto::Authenticated>(send(fresh, proto::Authenticate{"bruno", "password"}), fresh)
          .has_value());
}

TEST_F(HubAdminTest, ChangingAPasswordEndsTheOldOne) {
  const auto [connection, admin] = login("ana", Role::Admin);
  const auto created = find<proto::UserList>(
      send(connection, proto::CreateUser{"bruno", "old", "Bruno", Role::User}), connection);
  ASSERT_TRUE(created.has_value());
  const std::string bruno = created->users.back().user.id;

  (void)send(connection, proto::UpdateUser{bruno, std::nullopt, std::nullopt, "new"});

  const ConnectionId fresh = connect();
  EXPECT_TRUE(find<proto::ErrorMessage>(send(fresh, proto::Authenticate{"bruno", "old"}), fresh)
                  .has_value());
  const ConnectionId second = connect();
  EXPECT_TRUE(find<proto::Authenticated>(send(second, proto::Authenticate{"bruno", "new"}), second)
                  .has_value());
}

TEST_F(HubAdminTest, TheLastAdministratorCannotBeDemotedOrDeleted) {
  const auto [connection, admin] = login("ana", Role::Admin);
  const auto [other, bruno] = login("bruno", Role::User);

  // Ana is the only administrator, so promoting Bruno has to come first for
  // either of these to be possible at all.
  const auto demote =
      send(connection, proto::UpdateUser{admin.id, Role::User, std::nullopt, std::nullopt});
  auto error = find<proto::ErrorMessage>(demote, connection);
  ASSERT_TRUE(error.has_value());
  // Refused as "your own role", which is the rule that fires first.
  EXPECT_EQ(error->code, "invalid_target");

  const auto remove = send(connection, proto::DeleteUser{admin.id});
  error = find<proto::ErrorMessage>(remove, connection);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "invalid_target");

  // Now from a second administrator, where the self rule does not apply and
  // the last administrator rule is what has to hold.
  (void)send(connection, proto::UpdateUser{bruno.id, Role::Admin, std::nullopt, std::nullopt});
  const auto by_other = send(other, proto::DeleteUser{admin.id});
  EXPECT_FALSE(find<proto::ErrorMessage>(by_other, other).has_value());

  // Bruno is the last one left and cannot remove his own role either way.
  const auto last =
      send(other, proto::UpdateUser{bruno.id, Role::User, std::nullopt, std::nullopt});
  error = find<proto::ErrorMessage>(last, other);
  ASSERT_TRUE(error.has_value());
}

TEST_F(HubAdminTest, DeletingAnAccountRemovesThemFromTheirRoomAndEndsTheirSession) {
  set_up_room();

  const auto out = send(admin_connection_, proto::DeleteUser{user_.id});
  ASSERT_TRUE(find<proto::UserList>(out, admin_connection_).has_value());

  // Out of the room, and the media layer told.
  EXPECT_FALSE(hub_.rooms().find(room_)->contains(user_.id));
  EXPECT_EQ(media_.left, std::vector<std::string>{user_.id});

  // Their connection is no longer authenticated: a token issued a moment ago
  // must not outlive the account it belongs to.
  const auto after = send(user_connection_, proto::CreateRoom{user_.id, "anything"});
  const auto error = find<proto::ErrorMessage>(after, user_connection_);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "unauthorized");
}

// --- rooms -------------------------------------------------------------------

TEST_F(HubAdminTest, AnybodyCreatesARoomAndTheOldPersistentFlagIsIgnored) {
  // The gate this replaces refused `persistent` to anybody but an
  // administrator, because a room that outlived its participants was a favour.
  // Every room outlives them now, so the request asks for what it was going to
  // get. The field is still accepted on the wire so that a client built before
  // this still connects; it decides nothing.
  const auto [connection, admin] = login("ana", Role::Admin);
  const auto [other, bruno] = login("bruno", Role::User);

  const auto asked = send(other, proto::CreateRoom{bruno.id, "permanent", true});
  EXPECT_FALSE(find<proto::ErrorMessage>(asked, other).has_value());
  const auto created = find<proto::RoomCreated>(asked, other);
  ASSERT_TRUE(created.has_value());

  const auto* room = hub_.rooms().find(created->room_id);
  ASSERT_NE(room, nullptr);
  EXPECT_TRUE(room->persistent);

  // And asking for nothing gets a room exactly as permanent.
  const auto plain =
      find<proto::RoomCreated>(send(connection, proto::CreateRoom{admin.id, "ad hoc"}), connection);
  ASSERT_TRUE(plain.has_value());
  EXPECT_TRUE(hub_.rooms().find(plain->room_id)->persistent);
}

TEST_F(HubAdminTest, APersistentRoomOutlivesItsLastParticipant) {
  const auto [connection, admin] = login("ana", Role::Admin);

  const auto created = find<proto::RoomCreated>(
      send(connection, proto::CreateRoom{admin.id, "standup", true}), connection);
  ASSERT_TRUE(created.has_value());
  const std::string room = created->room_id;

  (void)send(connection, proto::JoinRoom{room, admin.id, "Ana"});
  (void)send(connection, proto::LeaveRoom{room, admin.id});

  // An ordinary room would be gone here, and its identifier would stop working.
  ASSERT_NE(hub_.rooms().find(room), nullptr);
  EXPECT_TRUE(hub_.rooms().find(room)->participants.empty());

  const auto rejoined = send(connection, proto::JoinRoom{room, admin.id, "Ana"});
  EXPECT_FALSE(find<proto::ErrorMessage>(rejoined, connection).has_value());
}

TEST_F(HubAdminTest, AnOrdinaryUserGetsOneRoomAndAnAdministratorGetsSeveral) {
  const auto [admin, admin_user] = login("ana", Role::Admin);
  const auto [plain, bruno] = login("bruno", Role::User);

  const auto first =
      find<proto::RoomCreated>(send(plain, proto::CreateRoom{bruno.id, "one"}), plain);
  ASSERT_TRUE(first.has_value());

  // The second is refused, and it names the room that is in the way.
  const auto refused = send(plain, proto::CreateRoom{bruno.id, "two"});
  const auto error = find<proto::ErrorMessage>(refused, plain);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "room_limit_reached");
  EXPECT_NE(error->message.find(first->room_id), std::string::npos)
      << "the refusal should say which room is in the way: " << error->message;
  EXPECT_FALSE(find<proto::RoomCreated>(refused, plain).has_value());

  // An administrator is not held to it.
  ASSERT_TRUE(find<proto::RoomCreated>(send(admin, proto::CreateRoom{admin_user.id, "a"}), admin)
                  .has_value());
  ASSERT_TRUE(find<proto::RoomCreated>(send(admin, proto::CreateRoom{admin_user.id, "b"}), admin)
                  .has_value());
}

TEST_F(HubAdminTest, ClosingSomebodysRoomLetsThemMakeAnotherOne) {
  // The way out of the limit. Only an administrator can close a room, so this
  // is the only way out, and it has to work.
  const auto [admin, admin_user] = login("ana", Role::Admin);
  const auto [plain, bruno] = login("bruno", Role::User);

  const auto first =
      find<proto::RoomCreated>(send(plain, proto::CreateRoom{bruno.id, "one"}), plain);
  ASSERT_TRUE(first.has_value());
  (void)send(admin, proto::DeleteRoom{.room_id = first->room_id});

  const auto second = send(plain, proto::CreateRoom{bruno.id, "two"});
  EXPECT_FALSE(find<proto::ErrorMessage>(second, plain).has_value());
  EXPECT_TRUE(find<proto::RoomCreated>(second, plain).has_value());
}

TEST_F(HubAdminTest, LeavingYourOwnRoomDoesNotFreeTheLimit) {
  // Walking out is not closing. The room is still theirs and still on
  // everybody's list, so it still counts against them: a limit that a leave
  // reset would be no limit at all.
  const auto [plain, bruno] = login("bruno", Role::User);
  const auto first =
      find<proto::RoomCreated>(send(plain, proto::CreateRoom{bruno.id, "one"}), plain);
  ASSERT_TRUE(first.has_value());

  (void)send(plain, proto::JoinRoom{first->room_id, bruno.id, "Bruno"});
  (void)send(plain, proto::LeaveRoom{first->room_id, bruno.id});

  const auto error =
      find<proto::ErrorMessage>(send(plain, proto::CreateRoom{bruno.id, "two"}), plain);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "room_limit_reached");
}

TEST_F(HubAdminTest, ARoomStaysWhenItEmptiesAndCanBeWalkedBackInto) {
  // This asserted the opposite: leaving your own room destroyed it, which is
  // what made a room appear in the list and then refuse the join.
  const auto [connection, admin] = login("ana", Role::Admin);
  const auto created =
      find<proto::RoomCreated>(send(connection, proto::CreateRoom{admin.id, "ad hoc"}), connection);
  ASSERT_TRUE(created.has_value());

  (void)send(connection, proto::JoinRoom{created->room_id, admin.id, "Ana"});
  (void)send(connection, proto::LeaveRoom{created->room_id, admin.id});

  ASSERT_NE(hub_.rooms().find(created->room_id), nullptr);
  const auto rejoined = send(connection, proto::JoinRoom{created->room_id, admin.id, "Ana"});
  EXPECT_FALSE(find<proto::ErrorMessage>(rejoined, connection).has_value());
  EXPECT_EQ(hub_.rooms().find(created->room_id)->size(), 1U);
}

TEST_F(HubAdminTest, ClosingARoomEmptiesItAndTellsEveryone) {
  set_up_room();

  const auto out = send(admin_connection_, proto::DeleteRoom{room_});

  const auto kicked = find<proto::UserKicked>(out, user_connection_);
  ASSERT_TRUE(kicked.has_value());
  EXPECT_FALSE(kicked->reason.empty());

  EXPECT_EQ(hub_.rooms().find(room_), nullptr);
  ASSERT_TRUE(find<proto::RoomList>(out, admin_connection_).has_value());
}

TEST_F(HubAdminTest, ClosingARoomThatNeverExistedIsAnError) {
  const auto [connection, admin] = login("ana", Role::Admin);

  const auto out = send(connection, proto::DeleteRoom{"ABCDEF"});
  const auto error = find<proto::ErrorMessage>(out, connection);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "room_not_found");
}

TEST_F(HubAdminTest, ClosingARoomReachesEverybodyElsesList) {
  // The other half of CreatingARoomReachesEverybodyElsesList in test_hub.cpp:
  // a closed room used to stay on every other client's screen until somebody
  // tried to join it and was told it did not exist.
  const auto [admin, admin_user] = login("ana", Role::Admin);
  const auto [bruno, bruno_user] = login("bruno", Role::User);
  const auto created = send(admin, proto::CreateRoom{admin_user.id, "dev-room"});
  const auto room = find<proto::RoomCreated>(created, admin);
  ASSERT_TRUE(room.has_value());

  const auto out = send(admin, proto::DeleteRoom{.room_id = room->room_id});

  const auto bruno_list = find<proto::RoomList>(out, bruno);
  ASSERT_TRUE(bruno_list.has_value()) << "bruno was not told the room is gone";
  EXPECT_TRUE(bruno_list->rooms.empty());
}

TEST_F(HubAdminTest, TheRoomListCountsWhoIsInside) {
  set_up_room();

  const auto list =
      find<proto::RoomList>(send(admin_connection_, proto::ListRooms{}), admin_connection_);
  ASSERT_TRUE(list.has_value());
  ASSERT_EQ(list->rooms.size(), 1U);
  EXPECT_EQ(list->rooms.front().id, room_);
  EXPECT_EQ(list->rooms.front().participant_count, 2);
  EXPECT_EQ(list->rooms.front().owner_id, admin_.id);
  // Every room outlives its participants now; there is no other kind.
  EXPECT_TRUE(list->rooms.front().persistent);
}

// --- account restrictions ----------------------------------------------------

TEST_F(HubAdminTest, ASilencedAccountCannotSayAnything) {
  set_up_room();

  proto::RestrictUser silence;
  silence.user_id = user_.id;
  silence.silenced = true;
  (void)send(admin_connection_, silence);

  proto::ChatMessage said;
  said.message.room_id = room_;
  said.message.user_id = user_.id;
  said.message.text = "hello";
  const auto out = send(user_connection_, said);

  const auto refusal = find<proto::ErrorMessage>(out, user_connection_);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "forbidden");
  // Nobody else was told anything, which is the half that matters: a message
  // refused after it was broadcast is not refused.
  EXPECT_FALSE(find<proto::ChatMessage>(out, admin_connection_).has_value());

  // And lifting it gives the chat back.
  proto::RestrictUser lift;
  lift.user_id = user_.id;
  lift.silenced = false;
  (void)send(admin_connection_, lift);
  EXPECT_TRUE(
      find<proto::ChatMessage>(send(user_connection_, said), admin_connection_).has_value());
}

TEST_F(HubAdminTest, ABlockedAccountCannotStartAShare) {
  set_up_room();

  proto::RestrictUser block;
  block.user_id = user_.id;
  block.screen_share_blocked = true;
  (void)send(admin_connection_, block);

  const auto out = send(user_connection_, proto::ScreenShareStarted{room_, user_.id});
  const auto refusal = find<proto::ErrorMessage>(out, user_connection_);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "forbidden");
  EXPECT_FALSE(find<proto::ScreenShareStarted>(out, admin_connection_).has_value());
}

TEST_F(HubAdminTest, BlockingSharingStopsTheShareThatIsAlreadyRunning) {
  set_up_room();
  (void)send(user_connection_, proto::ScreenShareStarted{room_, user_.id});
  ASSERT_NE(hub_.rooms().find(room_)->find(user_.id), nullptr);
  ASSERT_TRUE(hub_.rooms().find(room_)->find(user_.id)->sharing_screen);

  proto::RestrictUser block;
  block.user_id = user_.id;
  block.screen_share_blocked = true;
  const auto out = send(admin_connection_, block);

  // A block that waited for the next attempt would leave whatever is on
  // everybody's screen there, which is the thing being reached for.
  EXPECT_FALSE(hub_.rooms().find(room_)->find(user_.id)->sharing_screen);
  EXPECT_TRUE(find<proto::ScreenShareStopped>(out, admin_connection_).has_value());
  EXPECT_TRUE(find<proto::ScreenShareStopped>(out, user_connection_).has_value());
}

TEST_F(HubAdminTest, ARestrictedAccountAndItsRoomAreBothTold) {
  set_up_room();

  proto::RestrictUser change;
  change.user_id = user_.id;
  change.silenced = true;
  change.reason = "off topic";
  const auto out = send(admin_connection_, change);

  const auto to_target = find<proto::UserRestricted>(out, user_connection_);
  ASSERT_TRUE(to_target.has_value());
  EXPECT_EQ(to_target->user_id, user_.id);
  EXPECT_TRUE(to_target->restrictions.silenced);
  EXPECT_EQ(to_target->by_user_id, admin_.id);
  EXPECT_EQ(to_target->reason, "off topic");
  EXPECT_EQ(to_target->room_id, room_);

  // Everybody in the room, so that a chat somebody has stopped using has an
  // explanation next to it.
  EXPECT_TRUE(find<proto::UserRestricted>(out, admin_connection_).has_value());
}

TEST_F(HubAdminTest, AMutedAccountArrivesInTheRoomMutedAndStaysThere) {
  const auto [admin, ana] = login("ana", Role::Admin);
  const auto [connection, bruno] = login("bruno", Role::User);

  proto::RestrictUser mute;
  mute.user_id = bruno.id;
  mute.muted = true;
  (void)send(admin, mute);

  const auto created =
      find<proto::RoomCreated>(send(admin, proto::CreateRoom{ana.id, "room"}), admin);
  ASSERT_TRUE(created.has_value());
  (void)send(connection, proto::JoinRoom{created->room_id, bruno.id, "Bruno"});

  const dv::models::Participant* participant = hub_.rooms().find(created->room_id)->find(bruno.id);
  ASSERT_NE(participant, nullptr);
  EXPECT_TRUE(participant->muted);
  // By the administrator and not by themselves, which is what makes it hold.
  EXPECT_TRUE(participant->muted_by_admin);

  const auto refusal = find<proto::ErrorMessage>(
      send(connection, proto::Unmute{created->room_id, bruno.id, {}}), connection);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "forbidden");
}

TEST_F(HubAdminTest, MutingAnAccountTakesTheMicrophoneNow) {
  set_up_room();

  proto::RestrictUser mute;
  mute.user_id = user_.id;
  mute.muted = true;
  const auto out = send(admin_connection_, mute);

  // Announced as an ordinary mute naming who did it, so a client that knows
  // nothing about restrictions still draws the microphone correctly.
  const auto announced = find<proto::Mute>(out, user_connection_);
  ASSERT_TRUE(announced.has_value());
  EXPECT_EQ(announced->by_user_id, admin_.id);
  EXPECT_TRUE(hub_.rooms().find(room_)->find(user_.id)->muted_by_admin);
}

// A forced unmute is about one room and the restriction is about the account.
// Without this, the weaker of the two would win and somebody muted everywhere
// would get their microphone back with one click on the wrong control.
TEST_F(HubAdminTest, AForcedUnmuteCannotReleaseAnAccountLevelMute) {
  set_up_room();

  proto::RestrictUser mute;
  mute.user_id = user_.id;
  mute.muted = true;
  (void)send(admin_connection_, mute);

  const auto refusal = find<proto::ErrorMessage>(
      send(admin_connection_, proto::ForceMute{room_, user_.id, false}), admin_connection_);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "invalid_target");
  EXPECT_TRUE(hub_.rooms().find(room_)->find(user_.id)->muted);

  // Lifting the restriction is what gives it back, and it does so on its own.
  proto::RestrictUser lift;
  lift.user_id = user_.id;
  lift.muted = false;
  const auto out = send(admin_connection_, lift);
  EXPECT_TRUE(find<proto::Unmute>(out, user_connection_).has_value());
  EXPECT_FALSE(hub_.rooms().find(room_)->find(user_.id)->muted);
  EXPECT_FALSE(hub_.rooms().find(room_)->find(user_.id)->muted_by_admin);
}

TEST_F(HubAdminTest, ABannedAccountLosesItsSessionAndCannotLogInAgain) {
  set_up_room();

  proto::RestrictUser ban;
  ban.user_id = user_.id;
  ban.banned = true;
  ban.reason = "again";
  const auto out = send(admin_connection_, ban);

  // Told before being removed: a ban that closed the connection first would
  // leave the person it is about the only one who never heard why.
  ASSERT_TRUE(find<proto::UserRestricted>(out, user_connection_).has_value());
  const auto kicked = find<proto::UserKicked>(out, user_connection_);
  ASSERT_TRUE(kicked.has_value());
  EXPECT_EQ(kicked->reason, "again");
  EXPECT_TRUE(find<proto::UserLeft>(out, admin_connection_).has_value());
  EXPECT_EQ(hub_.rooms().find(room_)->size(), 1U);

  // The session is gone, not merely emptied out.
  const auto refused = find<proto::ErrorMessage>(send(user_connection_, proto::ListChat{room_, 0}),
                                                 user_connection_);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "unauthorized");

  const auto again = send(connect(), proto::Authenticate{"bruno", "password"});
  ASSERT_EQ(again.size(), 1U);
  const auto* error = std::get_if<proto::ErrorMessage>(&again.front().message);
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->code, "account_banned");
}

TEST_F(HubAdminTest, ARestrictionSurvivesIntoTheAccountList) {
  set_up_room();

  proto::RestrictUser change;
  change.user_id = user_.id;
  change.silenced = true;
  change.screen_share_blocked = true;
  const auto list = find<proto::UserList>(send(admin_connection_, change), admin_connection_);
  ASSERT_TRUE(list.has_value());

  bool seen = false;
  for (const proto::UserSummary& summary : list->users) {
    if (summary.user.id == user_.id) {
      seen = true;
      EXPECT_EQ(dv::models::describe(summary.user.restrictions), "silenced screen_share_blocked");
    }
    // The list an administrator receives still carries no secrets, whatever
    // else was added to the account.
    EXPECT_FALSE(summary.username.empty());
  }
  EXPECT_TRUE(seen);
}

TEST_F(HubAdminTest, AnAbsentFlagDoesNotLiftARestrictionSomebodyElseApplied) {
  set_up_room();

  proto::RestrictUser first;
  first.user_id = user_.id;
  first.banned = false;
  first.silenced = true;
  (void)send(admin_connection_, first);

  // A second administrator unmutes, saying nothing about the silence. The
  // failure this guards against is silent: the person can talk again and only
  // the audit log would ever show why.
  proto::RestrictUser second;
  second.user_id = user_.id;
  second.muted = false;
  const auto list = find<proto::UserList>(send(admin_connection_, second), admin_connection_);
  ASSERT_TRUE(list.has_value());
  for (const proto::UserSummary& summary : list->users) {
    if (summary.user.id == user_.id) {
      EXPECT_TRUE(summary.user.restrictions.silenced);
    }
  }
}

TEST_F(HubAdminTest, AnAdministratorCannotRestrictThemselves) {
  set_up_room();

  proto::RestrictUser change;
  change.user_id = admin_.id;
  change.banned = true;
  const auto refusal =
      find<proto::ErrorMessage>(send(admin_connection_, change), admin_connection_);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "invalid_target");
}

TEST_F(HubAdminTest, OneAdministratorCanBanAnother) {
  set_up_room();
  const auto [second, carla] = login("carla", Role::Admin);

  // Not the last one, so this is allowed, exactly as deleting them would be.
  // The refusal that protects the last administrator sits behind the self
  // check: whoever is sending this is an administrator too, so a target who is
  // not them is never the only one left.
  proto::RestrictUser ban;
  ban.user_id = admin_.id;
  ban.banned = true;
  ASSERT_TRUE(find<proto::UserList>(send(second, ban), second).has_value());

  const auto refused = send(connect(), proto::Authenticate{"ana", "password"});
  ASSERT_EQ(refused.size(), 1U);
  const auto* error = std::get_if<proto::ErrorMessage>(&refused.front().message);
  ASSERT_NE(error, nullptr);
  EXPECT_EQ(error->code, "account_banned");
}

TEST_F(HubAdminTest, AFormSubmittedUnchangedIsNotAnAdministrativeAction) {
  set_up_room();

  proto::RestrictUser nothing;
  nothing.user_id = user_.id;
  nothing.banned = false;
  nothing.muted = false;
  nothing.silenced = false;
  nothing.screen_share_blocked = false;
  EXPECT_TRUE(
      find<proto::UserList>(send(admin_connection_, nothing), admin_connection_).has_value());

  const auto log =
      find<proto::AuditList>(send(admin_connection_, proto::ListAudit{}), admin_connection_);
  ASSERT_TRUE(log.has_value());
  EXPECT_TRUE(log->entries.empty()) << "an audit log full of these is a log nobody reads";
}

TEST_F(HubAdminTest, ARestrictionNamesWhatMovedInTheLog) {
  set_up_room();

  proto::RestrictUser change;
  change.user_id = user_.id;
  change.silenced = true;
  change.muted = false;
  (void)send(admin_connection_, change);

  const auto log =
      find<proto::AuditList>(send(admin_connection_, proto::ListAudit{}), admin_connection_);
  ASSERT_TRUE(log.has_value());
  ASSERT_EQ(log->entries.size(), 1U);
  EXPECT_EQ(log->entries.front().action, "restrict_user");
  EXPECT_EQ(log->entries.front().target_id, user_.id);
  EXPECT_EQ(log->entries.front().room_id, room_);
  // Only `silenced` moved, so only `silenced` is named.
  EXPECT_EQ(log->entries.front().detail, "silenced=true");
}

// --- the audit log -----------------------------------------------------------

TEST_F(HubAdminTest, EveryAdministrativeActionIsRecorded) {
  set_up_room();

  (void)send(admin_connection_, proto::ForceMute{room_, user_.id, true});
  (void)send(admin_connection_, proto::KickUser{room_, user_.id, "off topic"});

  const auto log =
      find<proto::AuditList>(send(admin_connection_, proto::ListAudit{}), admin_connection_);
  ASSERT_TRUE(log.has_value());
  ASSERT_EQ(log->entries.size(), 2U);

  // Newest first.
  EXPECT_EQ(log->entries.front().action, "kick");
  EXPECT_EQ(log->entries.front().target_id, user_.id);
  EXPECT_EQ(log->entries.front().room_id, room_);
  EXPECT_EQ(log->entries.front().detail, "off topic");
  EXPECT_EQ(log->entries.front().actor_id, admin_.id);
  EXPECT_GT(log->entries.front().timestamp_seconds, 0);

  EXPECT_EQ(log->entries.back().action, "force_mute");
}

TEST_F(HubAdminTest, ChangingOnesOwnPasswordIsRecordedWithoutThePassword) {
  set_up_room();

  (void)send(user_connection_, proto::ChangePassword{"password", "new-password"});

  const auto log =
      find<proto::AuditList>(send(admin_connection_, proto::ListAudit{}), admin_connection_);
  ASSERT_TRUE(log.has_value());
  ASSERT_EQ(log->entries.size(), 1U);

  const auto& entry = log->entries.front();
  EXPECT_EQ(entry.action, "change_password");
  // Actor and target are the same account, which is the only shape this action
  // has: the message it comes from cannot name anybody else.
  EXPECT_EQ(entry.actor_id, user_.id);
  EXPECT_EQ(entry.target_id, user_.id);

  // Neither password, in any form. What an audit log has business knowing here
  // is that the change happened and who made it.
  EXPECT_EQ(entry.detail.find("password"), 0U) << "the detail says what changed, not to what";
  EXPECT_EQ(entry.detail, "password");
}

TEST_F(HubAdminTest, AnOrdinaryUserMayChangeTheirOwnPasswordAndNobodyElses) {
  set_up_room();

  // The ordinary user's own change goes through, while update_user - the
  // administrator's way of setting a password - stays refused for them. Both
  // halves in one test because it is the pair that matters: opening the second
  // one up would have been the easy way to deliver the first.
  EXPECT_TRUE(find<proto::PasswordChanged>(
                  send(user_connection_, proto::ChangePassword{"password", "new-password"}),
                  user_connection_)
                  .has_value());

  const auto [again, bruno] = login("carla", Role::User);
  const auto refused = find<proto::ErrorMessage>(
      send(again, proto::UpdateUser{admin_.id, std::nullopt, std::nullopt, "hijacked"}), again);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "forbidden");
}

TEST_F(HubAdminTest, TheLogCanBeNarrowedToOneActor) {
  set_up_room();
  const auto [second, carla] = login("carla", Role::Admin);

  (void)send(admin_connection_, proto::ForceMute{room_, user_.id, true});
  (void)send(second, proto::ForceMute{room_, user_.id, false});

  const auto mine = find<proto::AuditList>(send(admin_connection_, proto::ListAudit{0, admin_.id}),
                                           admin_connection_);
  ASSERT_TRUE(mine.has_value());
  ASSERT_EQ(mine->entries.size(), 1U);
  EXPECT_EQ(mine->entries.front().actor_id, admin_.id);
}

TEST_F(HubAdminTest, NothingAParticipantDoesReachesTheLog) {
  set_up_room();

  // An ordinary call: joining, muting yourself, sharing a screen. None of it
  // is an administrative action, and a log full of them is a log nobody reads.
  (void)send(user_connection_, proto::Mute{room_, user_.id, {}});
  (void)send(user_connection_, proto::ScreenShareStarted{room_, user_.id});
  (void)send(user_connection_, proto::LeaveRoom{room_, user_.id});

  const auto log =
      find<proto::AuditList>(send(admin_connection_, proto::ListAudit{}), admin_connection_);
  ASSERT_TRUE(log.has_value());
  EXPECT_TRUE(log->entries.empty());
}

}  // namespace
