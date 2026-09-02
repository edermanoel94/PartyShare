#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>

#include <gtest/gtest.h>
#include <unistd.h>

#include <dv/models/chat.hpp>
#include <dv/models/notice.hpp>
#include <dv/models/user.hpp>

#include "signaling/authenticator.hpp"
#include "signaling/hub.hpp"
#include "store/mongo_store.hpp"

namespace {

using dv::models::Role;
using dv::server::Authenticator;
using dv::server::Hub;
using dv::server::store::Account;
using dv::server::store::MongoStores;
using dv::server::store::RoomRecord;

namespace proto = dv::protocol;

/// The same contract as MemoryUserStore in the unit tests, against a real
/// database.
///
/// Skipped rather than failed without one: a developer with no MongoDB should
/// still be able to run the whole suite, and a test that fails for the want of
/// a service teaches everyone to ignore it. CI sets DV_TEST_MONGO_URI when it
/// has a database to point at.
class MongoStoreTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* uri = std::getenv("DV_TEST_MONGO_URI");
    if (uri == nullptr || *uri == '\0') {
      GTEST_SKIP() << "DV_TEST_MONGO_URI is not set, so there is no database to test against";
    }

    // A database of its own per test and per run. Per test, so that the tests
    // do not depend on their order; per run, because the audit log has no way
    // to be emptied through its interface, and that is deliberate: a log an
    // administrator can erase is not evidence of anything. Reusing a name
    // across runs would mean each run reading the previous one's entries.
    //
    // The leftovers are all named partyshare_test_*, and dropping them is
    // `db.getMongo().getDBNames().filter(n => n.startsWith("partyshare_test_"))`.
    //
    // Truncated because MongoDB refuses a database name over 63 characters,
    // and the test names here are long enough to reach it.
    constexpr std::size_t kMaxNameLength = 30;
    const std::string name(::testing::UnitTest::GetInstance()->current_test_info()->name());
    database_ =
        "partyshare_test_" + std::to_string(::getpid()) + "_" + name.substr(0, kMaxNameLength);

    auto opened = MongoStores::open(uri, database_, 2000);
    ASSERT_TRUE(opened.ok()) << opened.error().message;
    stores_ = std::move(opened).take();

    clear();
  }

  void TearDown() override {
    if (stores_) {
      clear();
    }
  }

  /// Empties the collections through the interfaces themselves. Enough for a
  /// database this test owns, and it avoids reaching past the abstraction into
  /// the driver just to delete rows.
  void clear() {
    for (const Account& account : stores_->users().list()) {
      (void)stores_->users().remove(account.user.id);
    }
    for (const RoomRecord& room : stores_->rooms().list()) {
      (void)stores_->rooms().remove(room.id);
    }
    // By identifier, because ChatStore has no way to enumerate rooms and
    // should not: these are the only two any test here writes into.
    for (const char* room_id : {"8F42A1", "B00B00"}) {
      (void)stores_->chat().clear(room_id);
    }
    // The same reasoning for the notices: the store is asked about one account
    // at a time, and these are the accounts the tests below write to.
    for (const char* user_id : {"id-ana", "id-bruno", "id-carla"}) {
      (void)stores_->notices().clear_for(user_id);
    }
    // The sessions have no clear at all, on purpose - a presence history an
    // administrator can erase is not a history. Closing them is enough: every
    // question this collection answers is about the rows that are still open.
    (void)stores_->sessions().close_open();
  }

  static dv::models::ChatMessage chat_message(const std::string& room_id, const std::string& text,
                                              std::int64_t when) {
    dv::models::ChatMessage value;
    value.room_id = room_id;
    value.user_id = "id-ana";
    value.display_name = "Ana";
    value.text = text;
    value.timestamp_seconds = when;
    return value;
  }

  static dv::models::Notice notice(const std::string& user_id, const std::string& text) {
    dv::models::Notice value;
    value.user_id = user_id;
    value.from_user_id = "id-ana";
    value.from_display_name = "Ana";
    value.text = text;
    return value;
  }

  static dv::server::store::SessionRecord session(const std::string& user_id, const std::string& ip,
                                                  std::int64_t when = 0) {
    dv::server::store::SessionRecord value;
    value.user_id = user_id;
    value.ip = ip;
    value.connected_at = when;
    value.last_seen_at = when;
    return value;
  }

  static Account account(const std::string& username, Role role = Role::User) {
    Account value;
    value.username = username;
    value.user.id = "id-" + username;
    value.user.display_name = username;
    value.user.role = role;
    value.salt_hex = "73616c74";
    value.password_hash_hex = "68617368";
    return value;
  }

  std::string database_;
  std::unique_ptr<MongoStores> stores_;
};

TEST_F(MongoStoreTest, StoresAndFindsAnAccountBothWays) {
  ASSERT_FALSE(stores_->users().create(account("ana", Role::Admin)).has_value());

  const auto by_name = stores_->users().find_by_username("ana");
  ASSERT_TRUE(by_name.has_value());
  EXPECT_EQ(by_name->user.id, "id-ana");
  EXPECT_EQ(by_name->user.role, Role::Admin);
  EXPECT_EQ(by_name->salt_hex, "73616c74");
  EXPECT_EQ(by_name->password_hash_hex, "68617368");
  EXPECT_GT(by_name->created_at, 0);

  const auto by_id = stores_->users().find_by_id("id-ana");
  ASSERT_TRUE(by_id.has_value());
  EXPECT_EQ(by_id->username, "ana");
}

TEST_F(MongoStoreTest, RefusesADuplicateUsername) {
  ASSERT_FALSE(stores_->users().create(account("ana")).has_value());

  const auto failure = stores_->users().create(account("ana"));
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "user_exists");
}

TEST_F(MongoStoreTest, UpdatingKeepsTheCreationTime) {
  ASSERT_FALSE(stores_->users().create(account("ana")).has_value());
  const auto created = stores_->users().find_by_username("ana");
  ASSERT_TRUE(created.has_value());

  auto changed = *created;
  changed.created_at = 1;
  changed.user.role = Role::Admin;
  changed.user.display_name = "Ana Maria";
  ASSERT_FALSE(stores_->users().update(changed).has_value());

  const auto after = stores_->users().find_by_username("ana");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->created_at, created->created_at);
  EXPECT_EQ(after->user.role, Role::Admin);
  EXPECT_EQ(after->user.display_name, "Ana Maria");
}

TEST_F(MongoStoreTest, CountsTheAccountsHoldingARole) {
  ASSERT_FALSE(stores_->users().create(account("ana", Role::Admin)).has_value());
  ASSERT_FALSE(stores_->users().create(account("bruno", Role::User)).has_value());

  EXPECT_EQ(stores_->users().count_with_role(Role::Admin), 1U);
  EXPECT_EQ(stores_->users().count_with_role(Role::User), 1U);
}

TEST_F(MongoStoreTest, UpdatingOrRemovingSomethingAbsentIsAnError) {
  EXPECT_EQ(stores_->users().update(account("ghost"))->code, "user_not_found");
  EXPECT_EQ(stores_->users().remove("id-ghost")->code, "user_not_found");
  EXPECT_EQ(stores_->rooms().remove("8F42A1")->code, "room_not_found");
}

TEST_F(MongoStoreTest, UpsertingARoomTwiceKeepsOneRecord) {
  ASSERT_FALSE(stores_->rooms()
                   .upsert(RoomRecord{.id = "8F42A1", .name = "standup", .persistent = true})
                   .has_value());
  const auto created = stores_->rooms().find("8F42A1");
  ASSERT_TRUE(created.has_value());

  ASSERT_FALSE(stores_->rooms()
                   .upsert(RoomRecord{.id = "8F42A1", .name = "retro", .persistent = true})
                   .has_value());

  const auto replaced = stores_->rooms().find("8F42A1");
  ASSERT_TRUE(replaced.has_value());
  EXPECT_EQ(replaced->name, "retro");
  EXPECT_EQ(replaced->created_at, created->created_at);
  EXPECT_EQ(stores_->rooms().list().size(), 1U);
}

TEST_F(MongoStoreTest, TheAuditLogComesBackNewestFirst) {
  dv::models::AuditEntry first;
  first.actor_id = "id-ana";
  first.action = "kick";
  first.timestamp_seconds = 1000;
  ASSERT_FALSE(stores_->audit().append(first).has_value());

  dv::models::AuditEntry second;
  second.actor_id = "id-carla";
  second.action = "force_mute";
  second.timestamp_seconds = 2000;
  ASSERT_FALSE(stores_->audit().append(second).has_value());

  const auto entries = stores_->audit().list(0, {});
  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries.front().action, "force_mute");
  EXPECT_FALSE(entries.front().id.empty());

  const auto filtered = stores_->audit().list(0, "id-ana");
  ASSERT_EQ(filtered.size(), 1U);
  EXPECT_EQ(filtered.front().action, "kick");
}

TEST_F(MongoStoreTest, AConversationComesBackOldestFirst) {
  for (const auto& [text, when] : {std::pair{"one", 1000}, {"two", 2000}, {"three", 3000}}) {
    ASSERT_TRUE(stores_->chat().append(chat_message("8F42A1", text, when)).ok());
  }

  const auto messages = stores_->chat().list("8F42A1", 0);
  ASSERT_EQ(messages.size(), 3U);
  EXPECT_EQ(messages.front().text, "one");
  EXPECT_EQ(messages.back().text, "three");
  EXPECT_FALSE(messages.front().id.empty());
}

TEST_F(MongoStoreTest, AppendReportsTheIdentifierTheDatabaseAssigned) {
  // What comes back is what the room is shown, so it has to be the row that
  // exists. An identifier invented here would be one `list` never reports.
  const auto written = stores_->chat().append(chat_message("8F42A1", "hello", 1000));
  ASSERT_TRUE(written.ok()) << written.error().message;

  const auto messages = stores_->chat().list("8F42A1", 0);
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages.front().id, written.value().id);
}

TEST_F(MongoStoreTest, TheWindowIsTheNewestMessagesInOrder) {
  for (const auto& [text, when] : {std::pair{"one", 1000}, {"two", 2000}, {"three", 3000}}) {
    ASSERT_TRUE(stores_->chat().append(chat_message("8F42A1", text, when)).ok());
  }

  const auto messages = stores_->chat().list("8F42A1", 2);
  ASSERT_EQ(messages.size(), 2U);
  EXPECT_EQ(messages.front().text, "two");
  EXPECT_EQ(messages.back().text, "three");
}

TEST_F(MongoStoreTest, MessagesSentInTheSameSecondKeepTheirOrder) {
  // A conversation is faster than the resolution of its timestamps, so the
  // identifier is what breaks the tie. Without that the order of two lines
  // typed a moment apart would be whatever the database felt like.
  for (const char* text : {"one", "two", "three"}) {
    ASSERT_TRUE(stores_->chat().append(chat_message("8F42A1", text, 1000)).ok());
  }

  const auto messages = stores_->chat().list("8F42A1", 0);
  ASSERT_EQ(messages.size(), 3U);
  EXPECT_EQ(messages.front().text, "one");
  EXPECT_EQ(messages.back().text, "three");
}

TEST_F(MongoStoreTest, ClearingOneRoomLeavesTheOthersAlone) {
  ASSERT_TRUE(stores_->chat().append(chat_message("8F42A1", "ours", 1000)).ok());
  ASSERT_TRUE(stores_->chat().append(chat_message("B00B00", "theirs", 1000)).ok());

  EXPECT_FALSE(stores_->chat().clear("8F42A1").has_value());
  EXPECT_TRUE(stores_->chat().list("8F42A1", 0).empty());
  EXPECT_EQ(stores_->chat().list("B00B00", 0).size(), 1U);

  // A room where nobody spoke has nothing to forget, and that is not an error:
  // it is the ordinary case every time a room closes.
  EXPECT_FALSE(stores_->chat().clear("CCCCCC").has_value());
}

TEST_F(MongoStoreTest, AConversationSurvivesTheProcessThatWroteIt) {
  const char* uri = std::getenv("DV_TEST_MONGO_URI");
  ASSERT_NE(uri, nullptr);

  ASSERT_TRUE(stores_->chat().append(chat_message("8F42A1", "notes from today", 1000)).ok());

  // A second connection that shares nothing with the first but the server.
  auto reopened = MongoStores::open(uri, database_, 2000);
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;

  const auto messages = reopened.value()->chat().list("8F42A1", 0);
  ASSERT_EQ(messages.size(), 1U);
  EXPECT_EQ(messages.front().text, "notes from today");
}

TEST_F(MongoStoreTest, AnAccountSurvivesTheProcessThatCreatedIt) {
  // The whole point of the database, checked the only way it can be: write
  // through one connection and read through another one that shares nothing
  // with it but the server.
  {
    Authenticator authenticator(Authenticator::Options{}, stores_->users());
    const auto created = authenticator.add_user("ana", "password", "Ana", Role::Admin);
    ASSERT_TRUE(created.ok()) << created.error().message;
  }

  auto reopened = MongoStores::open(std::getenv("DV_TEST_MONGO_URI"), database_, 2000);
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;
  auto second = std::move(reopened).take();

  Authenticator authenticator(Authenticator::Options{}, second->users());
  const auto session = authenticator.authenticate("ana", "password", Authenticator::Clock::now());
  ASSERT_TRUE(session.ok()) << session.error().message;
  EXPECT_EQ(session.value().user.role, Role::Admin);

  // And the wrong password still fails, which is what proves the hash was
  // stored rather than the password.
  const auto refused = authenticator.authenticate("ana", "wrong", Authenticator::Clock::now());
  EXPECT_FALSE(refused.ok());
}

TEST_F(MongoStoreTest, ARestrictionSurvivesTheProcessThatAppliedIt) {
  // The whole reason restrictions are on the account rather than in the room:
  // a ban that a restart lifts is not a ban, and neither is one that only the
  // running server remembers. Checked the only way it can be, through a second
  // connection that shares nothing with the first but the server.
  std::string user_id;
  {
    Authenticator authenticator(Authenticator::Options{}, stores_->users());
    const auto created = authenticator.add_user("ana", "password", "Ana", Role::User);
    ASSERT_TRUE(created.ok()) << created.error().message;
    user_id = created.value().id;

    auto account = stores_->users().find_by_id(user_id);
    ASSERT_TRUE(account.has_value());
    account->user.restrictions.banned = true;
    account->user.restrictions.silenced = true;
    ASSERT_FALSE(stores_->users().update(*account).has_value());
  }

  auto reopened = MongoStores::open(std::getenv("DV_TEST_MONGO_URI"), database_, 2000);
  ASSERT_TRUE(reopened.ok()) << reopened.error().message;
  auto second = std::move(reopened).take();

  const auto account = second->users().find_by_id(user_id);
  ASSERT_TRUE(account.has_value());
  EXPECT_TRUE(account->user.restrictions.banned);
  EXPECT_TRUE(account->user.restrictions.silenced);
  EXPECT_FALSE(account->user.restrictions.muted);
  EXPECT_FALSE(account->user.restrictions.screen_share_blocked);

  // And the account cannot log in through it, which is the thing the flag was
  // written for.
  Authenticator authenticator(Authenticator::Options{}, second->users());
  const auto refused = authenticator.authenticate("ana", "password", Authenticator::Clock::now());
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "account_banned");
}

// An account written before restrictions existed has no `restrictions`
// subdocument at all, and reading that as a ban would be an upgrade that locks
// everybody out. There is no test of it here, because there is no way to write
// such a document through this interface: MongoUserStore always writes all four
// flags, and an older release is the only other way to produce one.
//
// Where it is checked instead: restrictions_from reads through bool_field, the
// same tolerant reader every other optional field in mongo_store.cpp uses; the
// wire parser's half is tests/unit/test_protocol.cpp, and the document's half
// is TestAnAccountWithoutRestrictionsHasNothingTakenAway in
// tools/dbadmin/internal/store, which strips the field from a real database and
// reads it back.

TEST_F(MongoStoreTest, ARoomComesBackAfterARestart) {
  const std::string room_id = [this] {
    Hub hub(Hub::Options{
        .users = &stores_->users(), .rooms = &stores_->rooms(), .audit = &stores_->audit()});
    const auto created = hub.rooms().create_room("standup", "id-ana");
    EXPECT_TRUE(created.ok());
    return created.ok() ? created.value() : std::string{};
  }();
  ASSERT_FALSE(room_id.empty());

  // A second Hub over the same database, which is what the next start of the
  // server is.
  Hub restarted(Hub::Options{
      .users = &stores_->users(), .rooms = &stores_->rooms(), .audit = &stores_->audit()});
  const auto* room = restarted.rooms().find(room_id);
  ASSERT_NE(room, nullptr);
  EXPECT_EQ(room->name, "standup");
  EXPECT_TRUE(room->persistent);
  // Empty, because who was inside did not survive the process and pretending
  // otherwise would be a participant list nobody is connected to.
  EXPECT_TRUE(room->participants.empty());
}

TEST_F(MongoStoreTest, EveryRoomIsWrittenAsItIsCreated) {
  // This asserted the opposite until rooms stopped being of two kinds: an
  // ordinary room reached no database at all, so it could not be listed at the
  // next start and its identifier stopped working the moment it emptied.
  Hub hub(Hub::Options{
      .users = &stores_->users(), .rooms = &stores_->rooms(), .audit = &stores_->audit()});
  const auto created = hub.rooms().create_room("ad hoc", "id-ana");
  ASSERT_TRUE(created.ok());

  const auto record = stores_->rooms().find(created.value());
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->name, "ad hoc");
  EXPECT_EQ(record->owner_id, "id-ana");
  EXPECT_EQ(stores_->rooms().list().size(), 1U);
}

// --- notices -----------------------------------------------------------------
//
// The same contract MemoryNoticeStore is held to in the unit tests. Both
// implementations answer the same questions, and a client cannot see which one
// is behind the server, so a difference between them is a bug either way round.

TEST_F(MongoStoreTest, ANoticeIsWrittenAndComesBackPending) {
  const auto written = stores_->notices().append(notice("id-bruno", "the meeting moved"));
  ASSERT_TRUE(written.ok()) << written.error().message;
  EXPECT_FALSE(written.value().id.empty());
  EXPECT_GT(written.value().created_at, 0);

  const auto pending = stores_->notices().pending_for("id-bruno");
  ASSERT_EQ(pending.size(), 1U);
  // The identifier the caller was handed is the identifier of the row, or the
  // recipient would be acknowledging something nothing answers to.
  EXPECT_EQ(pending.front().id, written.value().id);
  EXPECT_EQ(pending.front().text, "the meeting moved");
  EXPECT_EQ(pending.front().from_display_name, "Ana");
  EXPECT_FALSE(pending.front().acknowledged());
}

TEST_F(MongoStoreTest, PendingIsPerAccountAndOldestFirst) {
  ASSERT_TRUE(stores_->notices().append(notice("id-bruno", "one")).ok());
  ASSERT_TRUE(stores_->notices().append(notice("id-carla", "not yours")).ok());
  ASSERT_TRUE(stores_->notices().append(notice("id-bruno", "two")).ok());

  const auto pending = stores_->notices().pending_for("id-bruno");
  ASSERT_EQ(pending.size(), 2U);
  EXPECT_EQ(pending[0].text, "one");
  EXPECT_EQ(pending[1].text, "two");
}

TEST_F(MongoStoreTest, AcknowledgingTakesItOutOfPending) {
  const auto written = stores_->notices().append(notice("id-bruno", "read this"));
  ASSERT_TRUE(written.ok());

  const auto acknowledged = stores_->notices().acknowledge(written.value().id, "id-bruno");
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  EXPECT_TRUE(acknowledged.value().acknowledged());
  EXPECT_EQ(acknowledged.value().text, "read this");
  EXPECT_TRUE(stores_->notices().pending_for("id-bruno").empty());

  // And twice is not a failure, which is what a client that reconnected and
  // was handed the same notice again will do.
  const auto again = stores_->notices().acknowledge(written.value().id, "id-bruno");
  ASSERT_TRUE(again.ok()) << again.error().message;
}

TEST_F(MongoStoreTest, ANoticeIsAcknowledgedOnlyByWhoItWasWrittenTo) {
  const auto written = stores_->notices().append(notice("id-bruno", "for bruno"));
  ASSERT_TRUE(written.ok());

  const auto stolen = stores_->notices().acknowledge(written.value().id, "id-carla");
  ASSERT_FALSE(stolen.ok());
  EXPECT_EQ(stolen.error().code, "notice_not_found");
  // Nonsense that is not even an identifier answers the same way, so being
  // refused says nothing about whether the notice exists.
  EXPECT_EQ(stores_->notices().acknowledge("not-an-oid", "id-carla").error().code,
            "notice_not_found");

  EXPECT_EQ(stores_->notices().pending_for("id-bruno").size(), 1U);
}

TEST_F(MongoStoreTest, ClearingTakesOneAccountsNotices) {
  ASSERT_TRUE(stores_->notices().append(notice("id-bruno", "one")).ok());
  ASSERT_TRUE(stores_->notices().append(notice("id-carla", "two")).ok());

  ASSERT_FALSE(stores_->notices().clear_for("id-bruno").has_value());
  EXPECT_TRUE(stores_->notices().pending_for("id-bruno").empty());
  EXPECT_EQ(stores_->notices().pending_for("id-carla").size(), 1U);
}

// --- sessions ----------------------------------------------------------------

TEST_F(MongoStoreTest, ASessionIsOpenedAndClosed) {
  const auto opened = stores_->sessions().open(session("id-ana", "203.0.113.7"));
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_FALSE(opened.value().id.empty());
  EXPECT_GT(opened.value().connected_at, 0);
  EXPECT_TRUE(opened.value().open());

  auto open = stores_->sessions().list_open();
  ASSERT_EQ(open.size(), 1U);
  EXPECT_EQ(open.front().user_id, "id-ana");
  EXPECT_EQ(open.front().ip, "203.0.113.7");

  ASSERT_FALSE(stores_->sessions().close(opened.value().id).has_value());
  EXPECT_TRUE(stores_->sessions().list_open().empty());
}

TEST_F(MongoStoreTest, TouchingMovesTheHeartbeatAndNothingElse) {
  const auto opened = stores_->sessions().open(session("id-ana", "203.0.113.7", 1000));
  ASSERT_TRUE(opened.ok());

  ASSERT_FALSE(stores_->sessions().touch({opened.value().id}).has_value());

  const auto open = stores_->sessions().list_open();
  ASSERT_EQ(open.size(), 1U);
  EXPECT_GT(open.front().last_seen_at, 1000);
  EXPECT_EQ(open.front().connected_at, 1000);
}

TEST_F(MongoStoreTest, TouchingAnIdentifierNothingAnswersToIsNotAFailure) {
  // A row somebody removed by hand, and a client's invention that is not even
  // an object identifier. Neither is a reason to fail the heartbeat of
  // everybody else on the server.
  EXPECT_FALSE(stores_->sessions().touch({"64b7f0c2a1e4d3b2c1a09876"}).has_value());
  EXPECT_FALSE(stores_->sessions().touch({"not-an-oid"}).has_value());
  EXPECT_FALSE(stores_->sessions().touch({}).has_value());
  EXPECT_FALSE(stores_->sessions().close("not-an-oid").has_value());
}

TEST_F(MongoStoreTest, ClosingTwiceKeepsTheFirstTime) {
  const auto opened = stores_->sessions().open(session("id-ana", "203.0.113.7", 1000));
  ASSERT_TRUE(opened.ok());

  ASSERT_FALSE(stores_->sessions().close(opened.value().id).has_value());
  ASSERT_FALSE(stores_->sessions().close(opened.value().id).has_value());
  EXPECT_TRUE(stores_->sessions().list_open().empty());
}

TEST_F(MongoStoreTest, RecoveryClosesWhatAKilledServerLeftOpen) {
  ASSERT_TRUE(stores_->sessions().open(session("id-ana", "203.0.113.7", 1000)).ok());
  ASSERT_TRUE(stores_->sessions().open(session("id-bruno", "203.0.113.8", 1000)).ok());

  EXPECT_EQ(stores_->sessions().close_open(), 2U);
  EXPECT_TRUE(stores_->sessions().list_open().empty());
  // Nothing left to recover, which is what a second start finds.
  EXPECT_EQ(stores_->sessions().close_open(), 0U);
}

// What a recovered row is stamped with - the moment it was last seen, not the
// moment of recovery - has no test here, and deliberately no method to give it
// one. SessionStore answers about open sessions, because that is the only
// question the server has; the closed ones are read by tools/dbadmin, and
// TestSessionsAreReadNewestFirst there is where that timestamp is checked.
// Widening this interface to reach a field no server ever reads would be
// paying for a test with an API somebody later has to implement twice.

TEST_F(MongoStoreTest, ARefusedConnectionFailsRatherThanFallingBackToMemory) {
  // Port 1 has nothing listening on it. What matters is that this returns a
  // failure quickly, because these calls are made with the server's lock held:
  // the driver's own default would block for tens of seconds.
  const auto started = std::chrono::steady_clock::now();
  auto opened = MongoStores::open("mongodb://127.0.0.1:1", "partyshare_unreachable", 1000);
  const auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_FALSE(opened.ok());
  EXPECT_LT(elapsed, std::chrono::seconds(10));
}

}  // namespace
