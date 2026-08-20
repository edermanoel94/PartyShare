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
  void on_participant_joined(const std::string& /*room_id*/,
                             const dv::models::User& /*user*/) override {}

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
        send(admin_connection_, proto::CreateRoom{admin_.id, "room"}), admin_connection_);
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
           proto::ListRooms{},
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

TEST_F(HubAdminTest, OnlyAnAdministratorCreatesAPersistentRoom) {
  const auto [connection, admin] = login("ana", Role::Admin);
  const auto [other, bruno] = login("bruno", Role::User);

  const auto refused = send(other, proto::CreateRoom{bruno.id, "permanent", true});
  const auto error = find<proto::ErrorMessage>(refused, other);
  ASSERT_TRUE(error.has_value());
  EXPECT_EQ(error->code, "forbidden");

  const auto allowed = find<proto::RoomCreated>(
      send(connection, proto::CreateRoom{admin.id, "permanent", true}), connection);
  ASSERT_TRUE(allowed.has_value());
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

TEST_F(HubAdminTest, AnOrdinaryRoomStillDisappearsWhenItEmpties) {
  const auto [connection, admin] = login("ana", Role::Admin);
  const auto created =
      find<proto::RoomCreated>(send(connection, proto::CreateRoom{admin.id, "ad hoc"}), connection);
  ASSERT_TRUE(created.has_value());

  (void)send(connection, proto::JoinRoom{created->room_id, admin.id, "Ana"});
  (void)send(connection, proto::LeaveRoom{created->room_id, admin.id});

  EXPECT_EQ(hub_.rooms().find(created->room_id), nullptr);
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

TEST_F(HubAdminTest, TheRoomListCountsWhoIsInside) {
  set_up_room();

  const auto list =
      find<proto::RoomList>(send(admin_connection_, proto::ListRooms{}), admin_connection_);
  ASSERT_TRUE(list.has_value());
  ASSERT_EQ(list->rooms.size(), 1U);
  EXPECT_EQ(list->rooms.front().id, room_);
  EXPECT_EQ(list->rooms.front().participant_count, 2);
  EXPECT_EQ(list->rooms.front().owner_id, admin_.id);
  EXPECT_FALSE(list->rooms.front().persistent);
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
