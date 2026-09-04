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
#include "store/memory_store.hpp"

namespace {

using namespace std::chrono_literals;
using dv::models::Restrictions;
using dv::models::Role;
using dv::protocol::Message;
using dv::server::ConnectionId;
using dv::server::Hub;
using dv::server::Outgoing;

namespace proto = dv::protocol;

/// Records what the Hub told the media layer, so that a session ended out of
/// band can be checked to tear the SFU connection down and not only to change
/// the room.
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

/// A restriction that arrived without a message.
///
/// Everything in test_hub_admin.cpp goes through `restrict_user`, which is the
/// path an administrator's panel takes: the Hub is told, and it enforces in the
/// same breath. This file is about the other writer. tools/dbadmin edits the
/// accounts collection directly, on purpose, because the whole reason it exists
/// is to reach the data when no server is running to ask. When one *is*
/// running, nothing tells it, and its README says so in as many words:
///
///   "A running server re-reads the account on its next message from it... What
///   it does not do is what the server does in the same breath: end a session
///   already open, take a microphone already on, stop a share already running."
///
/// The store here is the test's and not the Hub's, which is what lets a test
/// write the same subdocument dbadmin writes and then ask the server what it
/// did about it. A MemoryUserStore rather than MongoDB for the reason the whole
/// suite uses one: the question is what the Hub does when an account changes
/// underneath it, and that has the same answer whichever store it is.
class HubRestrictionWatchTest : public ::testing::Test {
 protected:
  HubRestrictionWatchTest()
      : hub_(Hub::Options{.max_participants_per_room = 5,
                          .heartbeat_interval = 5000ms,
                          .heartbeat_timeout = 15000ms,
                          .room_id_seed = 4321u,
                          .users = &users_}) {
    hub_.set_media_signals(&media_);
  }

  /// One address per connection, from the documentation range, so that a test
  /// about presence can tell which socket a session record came from.
  static std::string address_of(ConnectionId id) { return "203.0.113." + std::to_string(id); }

  ConnectionId connect() {
    const ConnectionId id = next_connection_++;
    hub_.on_connect(id, address_of(id), now_);
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

  /// An administrator and an ordinary user, both in one room, neither of them
  /// restricted. The state every test below starts from, and the state that
  /// makes the point: whatever the server does next, it does without anybody
  /// having sent it a message.
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

  /// What tools/dbadmin does: the account document, and nothing else. No
  /// message, no connection, no idea that a server exists.
  void write_behind_the_back(const std::string& user_id, const Restrictions& restrictions) {
    auto account = users_.find_by_id(user_id);
    ASSERT_TRUE(account.has_value()) << "no account " << user_id;
    account->user.restrictions = restrictions;
    ASSERT_FALSE(users_.update(*account).has_value());
  }

  /// One turn of the loop the server already runs. Whatever notices a change
  /// written elsewhere has to be reachable from here, because this is the only
  /// thing that happens to a Hub nobody is talking to.
  std::vector<Outgoing> tick() {
    std::vector<ConnectionId> timed_out;
    return hub_.tick(now_, timed_out);
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

  template <typename T>
  static std::size_t count(const std::vector<Outgoing>& out) {
    std::size_t total = 0;
    for (const Outgoing& outgoing : out) {
      if (std::holds_alternative<T>(outgoing.message)) {
        ++total;
      }
    }
    return total;
  }

  /// Where in `out` the first T for `connection` sits, so that a test can say
  /// which of two messages the same socket reads first. The end of `out` when
  /// there is none, which orders after everything that is there.
  template <typename T>
  static std::size_t index_of(const std::vector<Outgoing>& out, ConnectionId connection) {
    for (std::size_t i = 0; i < out.size(); ++i) {
      if (out[i].connection == connection && std::holds_alternative<T>(out[i].message)) {
        return i;
      }
    }
    return out.size();
  }

  dv::server::store::MemoryUserStore users_;
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

// --- what a restriction written elsewhere has to do --------------------------

TEST_F(HubRestrictionWatchTest, AMuteWrittenElsewhereTakesTheMicrophoneNow) {
  set_up_room();
  ASSERT_FALSE(hub_.rooms().find(room_)->find(user_.id)->muted);

  write_behind_the_back(user_.id, Restrictions{.muted = true});
  const auto out = tick();

  // The room's own copy first: without it the next person to join is told that
  // somebody with no microphone has one.
  const dv::models::Participant* participant = hub_.rooms().find(room_)->find(user_.id);
  ASSERT_NE(participant, nullptr);
  EXPECT_TRUE(participant->muted);
  // By an administrator and not by themselves, or one click on the microphone
  // button undoes what an administrator wrote into the database.
  EXPECT_TRUE(participant->muted_by_admin);

  // Announced as an ordinary mute, the same shape restrict_user produces, so a
  // client that knows nothing about where this came from still draws the
  // microphone correctly.
  EXPECT_TRUE(find<proto::Mute>(out, user_connection_).has_value());
  EXPECT_TRUE(find<proto::Mute>(out, admin_connection_).has_value());
}

TEST_F(HubRestrictionWatchTest, ABlockWrittenElsewhereStopsTheShareThatIsAlreadyRunning) {
  set_up_room();
  (void)send(user_connection_, proto::ScreenShareStarted{room_, user_.id});
  ASSERT_TRUE(hub_.rooms().find(room_)->find(user_.id)->sharing_screen);

  write_behind_the_back(user_.id, Restrictions{.screen_share_blocked = true});
  const auto out = tick();

  // A block that waited for the next attempt would leave whatever is on
  // everybody's screen there, which is the one thing somebody reaching for the
  // control was trying to stop.
  EXPECT_FALSE(hub_.rooms().find(room_)->find(user_.id)->sharing_screen);
  EXPECT_TRUE(find<proto::ScreenShareStopped>(out, user_connection_).has_value());
  EXPECT_TRUE(find<proto::ScreenShareStopped>(out, admin_connection_).has_value());
}

TEST_F(HubRestrictionWatchTest, ABanWrittenElsewhereEndsTheSessionThatIsAlreadyOpen) {
  set_up_room();

  write_behind_the_back(user_.id, Restrictions{.banned = true});
  const auto out = tick();

  // Out of the room, and the media layer told, exactly as a kick does it:
  // nobody is left talking to somebody the server has stopped accepting.
  EXPECT_FALSE(hub_.rooms().find(room_)->contains(user_.id));
  EXPECT_NE(std::find(media_.left.begin(), media_.left.end(), user_.id), media_.left.end());
  EXPECT_TRUE(find<proto::UserLeft>(out, admin_connection_).has_value());

  // The person is told their session is over, and told before the room is
  // told they left: the same exit a ban from the panel takes.
  const auto ended = find<proto::SessionEnded>(out, user_connection_);
  ASSERT_TRUE(ended.has_value());
  EXPECT_EQ(ended->reason, "the account was suspended");
  EXPECT_LT(index_of<proto::SessionEnded>(out, user_connection_),
            index_of<proto::UserKicked>(out, user_connection_));

  // And the session is no longer a session. The connection is still open,
  // because telling somebody why is worth more than the socket, but it is no
  // longer somebody: the next message it sends is refused for want of a login
  // rather than accepted on the strength of one the database has withdrawn.
  const auto refused = find<proto::ErrorMessage>(
      send(user_connection_, proto::JoinRoom{room_, user_.id, "Bruno"}), user_connection_);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "unauthorized");
}

TEST_F(HubRestrictionWatchTest, TheAccountAndItsRoomAreBothToldWhatChangedElsewhere) {
  set_up_room();

  write_behind_the_back(user_.id, Restrictions{.silenced = true, .screen_share_blocked = true});
  const auto out = tick();

  // The person it is about, so that a control which stopped working has an
  // explanation next to it rather than looking broken.
  const auto to_target = find<proto::UserRestricted>(out, user_connection_);
  ASSERT_TRUE(to_target.has_value());
  EXPECT_EQ(to_target->user_id, user_.id);
  EXPECT_TRUE(to_target->restrictions.silenced);
  EXPECT_TRUE(to_target->restrictions.screen_share_blocked);
  EXPECT_EQ(to_target->room_id, room_);
  // Nobody sent it, so there is nobody to name. An empty actor is what the
  // client already renders as "an administrator", which is exactly as much as
  // is true: the audit entry dbadmin wrote holds the name, and this message is
  // not the audit log.
  EXPECT_TRUE(to_target->by_user_id.empty());

  // And the room, so that a chat somebody has stopped using is explained to
  // the people watching them stop.
  EXPECT_TRUE(find<proto::UserRestricted>(out, admin_connection_).has_value());
}

TEST_F(HubRestrictionWatchTest, LiftingElsewhereGivesTheMicrophoneBack) {
  set_up_room();
  write_behind_the_back(user_.id, Restrictions{.muted = true});
  (void)tick();
  ASSERT_TRUE(hub_.rooms().find(room_)->find(user_.id)->muted);

  write_behind_the_back(user_.id, Restrictions{});
  const auto out = tick();

  EXPECT_FALSE(hub_.rooms().find(room_)->find(user_.id)->muted);
  EXPECT_FALSE(hub_.rooms().find(room_)->find(user_.id)->muted_by_admin);
  EXPECT_TRUE(find<proto::Unmute>(out, user_connection_).has_value());
}

// The half that decides whether this can run on a loop at all. A pass that
// re-announces what it announced last time turns the heartbeat into a
// broadcast storm, and a client told once a second that nothing changed cannot
// tell the one time something did.
TEST_F(HubRestrictionWatchTest, ATickThatFindsNothingNewSaysNothing) {
  set_up_room();

  write_behind_the_back(user_.id, Restrictions{.muted = true, .silenced = true});
  ASSERT_GT(count<proto::UserRestricted>(tick()), 0U);

  for (int pass = 0; pass < 3; ++pass) {
    const auto out = tick();
    EXPECT_EQ(count<proto::UserRestricted>(out), 0U) << "pass " << pass;
    EXPECT_EQ(count<proto::Mute>(out), 0U) << "pass " << pass;
  }
}

// An account nobody is connected as is an account with nothing to enforce.
// Reading every row of the collection on every tick to discover that is the
// difference between a pass costing one lookup per participant and one costing
// the whole table.
TEST_F(HubRestrictionWatchTest, AnAccountWithNoSessionCostsNoAnnouncement) {
  set_up_room();

  const auto registered = hub_.authenticator().add_user("carla", "password", "carla", Role::User);
  ASSERT_TRUE(registered.ok());
  const auto carla = users_.find_by_username("carla");
  ASSERT_TRUE(carla.has_value());

  write_behind_the_back(carla->user.id, Restrictions{.banned = true});
  const auto out = tick();

  EXPECT_EQ(count<proto::UserRestricted>(out), 0U);
}

// --- an account that stopped existing ----------------------------------------
//
// The other thing tools/dbadmin can do to an account somebody is signed in as.
// There is no restriction to read here and nothing left in the store to read it
// from, which is the point: what has to be taken away is the copy the server is
// still holding. `Connection::user` was loaded at login from a row that is now
// gone, and every handler goes on trusting it.

TEST_F(HubRestrictionWatchTest, AnAccountDeletedElsewhereLosesTheSessionItWasHolding) {
  set_up_room();

  ASSERT_FALSE(users_.remove(user_.id).has_value());
  const auto out = tick();

  // Out of the room, and the media layer told, exactly as delete_user does it.
  EXPECT_FALSE(hub_.rooms().find(room_)->contains(user_.id));
  EXPECT_NE(std::find(media_.left.begin(), media_.left.end(), user_.id), media_.left.end());
  EXPECT_TRUE(find<proto::UserKicked>(out, admin_connection_).has_value());
  EXPECT_TRUE(find<proto::UserLeft>(out, admin_connection_).has_value());

  // And the identity is gone from the connection, which is the half a store
  // cannot reach: without it the next message is accepted on the strength of a
  // login whose account no longer exists.
  const auto refused = find<proto::ErrorMessage>(
      send(user_connection_, proto::JoinRoom{room_, user_.id, "Bruno"}), user_connection_);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "unauthorized");
}

TEST_F(HubRestrictionWatchTest, ADeletedAccountThatWasSharingStopsTheShareForEverybody) {
  set_up_room();
  (void)send(user_connection_, proto::ScreenShareStarted{room_, user_.id});
  ASSERT_TRUE(hub_.rooms().find(room_)->find(user_.id)->sharing_screen);

  ASSERT_FALSE(users_.remove(user_.id).has_value());
  const auto out = tick();

  // Otherwise every remaining client keeps drawing a share that ended, and
  // refuses to start its own because it still believes the floor is taken.
  EXPECT_TRUE(find<proto::ScreenShareStopped>(out, admin_connection_).has_value());
}

TEST_F(HubRestrictionWatchTest, ADeletedAccountInNoRoomStillLosesItsSession) {
  const auto [connection, bruno] = login("bruno", Role::User);

  ASSERT_FALSE(users_.remove(bruno.id).has_value());
  const auto out = tick();

  // The one case where nothing else says anything: with no room there is no
  // `user_kicked`, and until this existed the person learnt they were signed
  // out from the `unauthorized` their next click was refused with.
  const auto ended = find<proto::SessionEnded>(out, connection);
  ASSERT_TRUE(ended.has_value());
  EXPECT_EQ(ended->reason, "the account was removed");
  EXPECT_EQ(count<proto::UserKicked>(out), 0U);

  const auto refused = find<proto::ErrorMessage>(send(connection, proto::ListRooms{}), connection);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "unauthorized");
}

// The connection is still open and still being ticked. Without the identity
// having actually been dropped, the pass would find the same missing account
// every five seconds and evict somebody who is already gone, once per
// heartbeat, forever.
TEST_F(HubRestrictionWatchTest, ADeletedAccountIsEndedOnceAndNotOncePerHeartbeat) {
  set_up_room();
  ASSERT_FALSE(users_.remove(user_.id).has_value());
  ASSERT_TRUE(find<proto::UserLeft>(tick(), admin_connection_).has_value());

  const std::size_t left_once = media_.left.size();
  for (int pass = 0; pass < 3; ++pass) {
    const auto out = tick();
    EXPECT_EQ(count<proto::UserKicked>(out), 0U) << "pass " << pass;
    EXPECT_EQ(count<proto::UserLeft>(out), 0U) << "pass " << pass;
  }
  EXPECT_EQ(media_.left.size(), left_once);
}

// --- a session ended from the terminal ---------------------------------------

/// What tools/dbadmin writes for "end this person's session": one field on
/// the account, and nothing about the restrictions. The value is a wall clock
/// second; any nonzero one is a request.
void request_session_end(dv::server::store::MemoryUserStore& users, const std::string& user_id) {
  auto account = users.find_by_id(user_id);
  ASSERT_TRUE(account.has_value()) << "no account " << user_id;
  account->session_end_requested_at = dv::server::store::unix_seconds_now();
  ASSERT_FALSE(users.update(*account).has_value());
}

TEST_F(HubRestrictionWatchTest, ASessionEndRequestedElsewhereEndsTheSessionThatIsOpen) {
  set_up_room();

  request_session_end(users_, user_.id);
  const auto out = tick();

  // Out of the room, the room told, the media layer told: the same exit a ban
  // gives, because it is the same call.
  EXPECT_FALSE(hub_.rooms().find(room_)->contains(user_.id));
  EXPECT_NE(std::find(media_.left.begin(), media_.left.end(), user_.id), media_.left.end());
  const auto kicked = find<proto::UserKicked>(out, user_connection_);
  ASSERT_TRUE(kicked.has_value());
  EXPECT_EQ(kicked->reason, "the session was ended by an administrator");
  EXPECT_TRUE(find<proto::UserLeft>(out, admin_connection_).has_value());

  // Told that the *session* is over, in the same words, and told that first.
  // The kick alone read to the client as "out of the room, still signed in":
  // it went home, asked for the room list, and was refused with `unauthorized`
  // - shown as a wrong password to somebody who had typed nothing. Sent after
  // the kick this would lose that race, because the client acts on the kick
  // the moment it reads it.
  const auto ended = find<proto::SessionEnded>(out, user_connection_);
  ASSERT_TRUE(ended.has_value());
  EXPECT_EQ(ended->reason, "the session was ended by an administrator");
  EXPECT_LT(index_of<proto::SessionEnded>(out, user_connection_),
            index_of<proto::UserKicked>(out, user_connection_));
  // Nobody else's business: the room hears the kick, not the sign-out.
  EXPECT_FALSE(find<proto::SessionEnded>(out, admin_connection_).has_value());

  // And no longer a session: the next message is refused for want of a login.
  const auto refused = find<proto::ErrorMessage>(
      send(user_connection_, proto::JoinRoom{room_, user_.id, "Bruno"}), user_connection_);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "unauthorized");

  // But nothing was taken from the account. That is the whole difference
  // between this and a ban, and it is what the operator chose.
  const auto account = users_.find_by_id(user_.id);
  ASSERT_TRUE(account.has_value());
  EXPECT_FALSE(account->user.restrictions.any());
  // The request is spent. Left standing, it would end the next session too.
  EXPECT_EQ(account->session_end_requested_at, 0);
  EXPECT_EQ(count<proto::UserRestricted>(out), 0U);
}

TEST_F(HubRestrictionWatchTest, SomebodySignedOutFromTheTerminalMaySignInAgainAtOnce) {
  set_up_room();
  request_session_end(users_, user_.id);
  ASSERT_TRUE(find<proto::UserLeft>(tick(), admin_connection_).has_value());

  const ConnectionId again = connect();
  const auto out = send(again, proto::Authenticate{"bruno", "password"});
  ASSERT_TRUE(find<proto::Authenticated>(out, again).has_value());

  // And stays signed in through the next heartbeats. A request that ended
  // every session it saw would be a ban that nobody wrote down as one.
  for (int pass = 0; pass < 3; ++pass) {
    const auto later = tick();
    EXPECT_EQ(count<proto::UserKicked>(later), 0U) << "pass " << pass;
    EXPECT_EQ(count<proto::UserLeft>(later), 0U) << "pass " << pass;
  }
  const auto joined = send(again, proto::JoinRoom{room_, user_.id, "Bruno"});
  EXPECT_FALSE(find<proto::ErrorMessage>(joined, again).has_value());
  EXPECT_TRUE(hub_.rooms().find(room_)->contains(user_.id));
}

TEST_F(HubRestrictionWatchTest, ARequestWrittenWhileNobodyWasSignedInIsDiscardedAtTheNextLogin) {
  set_up_room();
  ASSERT_TRUE(hub_.authenticator().add_user("carla", "password", "carla", Role::User).ok());
  const auto carla = users_.find_by_username("carla");
  ASSERT_TRUE(carla.has_value());

  // Written between two of Carla's visits: the operator pressed the key a
  // moment after she left, or the screen was a heartbeat out of date. The
  // session it was about has ended on its own, and the next one is not the
  // one anybody asked to end.
  request_session_end(users_, carla->user.id);

  const ConnectionId connection = connect();
  ASSERT_TRUE(find<proto::Authenticated>(send(connection, proto::Authenticate{"carla", "password"}),
                                         connection)
                  .has_value());
  EXPECT_EQ(users_.find_by_id(carla->user.id)->session_end_requested_at, 0);

  const auto out = tick();
  EXPECT_EQ(count<proto::UserKicked>(out), 0U);
  const auto joined = send(connection, proto::JoinRoom{room_, carla->user.id, "Carla"});
  EXPECT_FALSE(find<proto::ErrorMessage>(joined, connection).has_value());
}

TEST_F(HubRestrictionWatchTest, ABanAndASessionEndOnTheSamePassEndTheSessionOnce) {
  set_up_room();
  write_behind_the_back(user_.id, Restrictions{.banned = true});
  request_session_end(users_, user_.id);

  const auto out = tick();
  // One eviction: announced to each of the two people in the room while both
  // are still in it, a single departure for whoever is left, and one for the
  // media layer to tear down.
  EXPECT_EQ(count<proto::UserKicked>(out), 2U);
  EXPECT_EQ(count<proto::UserLeft>(out), 1U);
  EXPECT_EQ(std::count(media_.left.begin(), media_.left.end(), user_.id), 1);
  EXPECT_FALSE(hub_.rooms().find(room_)->contains(user_.id));
  // And told once that the session is over. The ban got there first, so the
  // reason is the ban's.
  EXPECT_EQ(count<proto::SessionEnded>(out), 1U);
  EXPECT_EQ(find<proto::SessionEnded>(out, user_connection_)->reason, "the account was suspended");

  // The ban stays, the request does not: one is a statement about the
  // account and the other was an instruction about a session that is over.
  const auto account = users_.find_by_id(user_.id);
  ASSERT_TRUE(account.has_value());
  EXPECT_TRUE(account->user.restrictions.banned);
  EXPECT_EQ(account->session_end_requested_at, 0);
}

}  // namespace
