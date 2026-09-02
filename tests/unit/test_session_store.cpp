#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "store/memory_store.hpp"
#include "store/session_store.hpp"

namespace {

using dv::server::store::MemorySessionStore;
using dv::server::store::SessionRecord;

SessionRecord session(const std::string& user_id, const std::string& ip) {
  SessionRecord record;
  record.user_id = user_id;
  record.ip = ip;
  return record;
}

/// A session whose clock is pinned. The store only stamps what the caller left
/// at zero, which is what lets a test about `touch` and `close` say when the
/// row started rather than asserting against whatever second it ran in.
SessionRecord session_at(const std::string& user_id, const std::string& ip, std::int64_t when) {
  SessionRecord record = session(user_id, ip);
  record.connected_at = when;
  record.last_seen_at = when;
  return record;
}

std::vector<std::string> user_ids(const std::vector<SessionRecord>& sessions) {
  std::vector<std::string> result;
  result.reserve(sessions.size());
  for (const SessionRecord& record : sessions) {
    result.push_back(record.user_id);
  }
  return result;
}

TEST(MemorySessionStore, OpeningStampsAnIdentifierAndTheTimes) {
  MemorySessionStore store;

  const auto opened = store.open(session("user1", "203.0.113.7"));
  ASSERT_TRUE(opened.ok()) << opened.error().message;
  EXPECT_FALSE(opened.value().id.empty());
  EXPECT_GT(opened.value().connected_at, 0);
  EXPECT_EQ(opened.value().last_seen_at, opened.value().connected_at);
  EXPECT_EQ(opened.value().ended_at, 0);
  EXPECT_TRUE(opened.value().open());
  EXPECT_EQ(opened.value().ip, "203.0.113.7");
}

TEST(MemorySessionStore, TimesTheCallerSuppliedAreKept) {
  MemorySessionStore store;

  const auto opened = store.open(session_at("user1", "203.0.113.7", 1000));
  ASSERT_TRUE(opened.ok());
  EXPECT_EQ(opened.value().connected_at, 1000);
  EXPECT_EQ(opened.value().last_seen_at, 1000);
}

TEST(MemorySessionStore, OpenIsEverythingThatHasNotEnded) {
  MemorySessionStore store;

  const auto first = store.open(session("user1", "203.0.113.7"));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(store.open(session("user2", "203.0.113.8")).ok());

  EXPECT_EQ(user_ids(store.list_open()).size(), 2U);

  ASSERT_FALSE(store.close(first.value().id).has_value());
  EXPECT_EQ(user_ids(store.list_open()), std::vector<std::string>{"user2"});
}

TEST(MemorySessionStore, TouchingMovesTheTimeForward) {
  MemorySessionStore store;

  const auto opened = store.open(session_at("user1", "203.0.113.7", 1000));
  ASSERT_TRUE(opened.ok());

  ASSERT_FALSE(store.touch({opened.value().id}).has_value());

  const auto open = store.list_open();
  ASSERT_EQ(open.size(), 1U);
  EXPECT_GT(open.front().last_seen_at, 1000);
  // Only the heartbeat moves, so the row still says when the person arrived.
  EXPECT_EQ(open.front().connected_at, 1000);
}

TEST(MemorySessionStore, TouchingLeavesTheOthersAndTheClosedOnesAlone) {
  MemorySessionStore store;

  const auto touched = store.open(session_at("user1", "203.0.113.7", 1000));
  const auto untouched = store.open(session_at("user2", "203.0.113.8", 1000));
  const auto ended = store.open(session_at("user3", "203.0.113.9", 1000));
  ASSERT_TRUE(touched.ok() && untouched.ok() && ended.ok());
  ASSERT_FALSE(store.close(ended.value().id).has_value());

  ASSERT_FALSE(store.touch({touched.value().id, ended.value().id}).has_value());

  for (const SessionRecord& record : store.list_open()) {
    if (record.user_id == "user1") {
      EXPECT_GT(record.last_seen_at, 1000);
    } else {
      EXPECT_EQ(record.last_seen_at, 1000);
    }
  }
}

TEST(MemorySessionStore, AnIdentifierNothingAnswersToIsNotAFailure) {
  MemorySessionStore store;

  // A row somebody removed by hand is not a reason to fail the heartbeat of
  // everybody else on the server.
  EXPECT_FALSE(store.touch({"nothing"}).has_value());
  EXPECT_FALSE(store.touch({}).has_value());
  EXPECT_FALSE(store.close("nothing").has_value());
}

TEST(MemorySessionStore, ClosingTwiceKeepsTheFirstTime) {
  MemorySessionStore store;

  const auto opened = store.open(session_at("user1", "203.0.113.7", 1000));
  ASSERT_TRUE(opened.ok());

  ASSERT_FALSE(store.close(opened.value().id).has_value());
  // The row is gone from `list_open`, so this reads it back through the one
  // remaining question the interface answers: closing again must not fail, and
  // must not reopen anything.
  ASSERT_FALSE(store.close(opened.value().id).has_value());
  EXPECT_TRUE(store.list_open().empty());
}

TEST(MemorySessionStore, RecoveryClosesWhatIsOpenAtTheTimeItWasLastSeen) {
  MemorySessionStore store;

  ASSERT_TRUE(store.open(session_at("user1", "203.0.113.7", 1000)).ok());
  const auto already = store.open(session_at("user2", "203.0.113.8", 1000));
  ASSERT_TRUE(already.ok());
  ASSERT_FALSE(store.close(already.value().id).has_value());

  EXPECT_EQ(store.close_open(), 1U);
  EXPECT_TRUE(store.list_open().empty());
  // And nothing is left to recover on a second pass.
  EXPECT_EQ(store.close_open(), 0U);
}

TEST(MemorySessionStore, TheCapNeverDropsAnOpenSession) {
  MemorySessionStore store;

  // One open row, and then the store filled past its capacity with rows that
  // have ended. The open one is the only thing here anybody is asking about,
  // so it is the one that survives.
  const auto kept = store.open(session("user1", "203.0.113.7"));
  ASSERT_TRUE(kept.ok());

  for (std::size_t index = 0; index < MemorySessionStore::kCapacity + 10; ++index) {
    const auto churn = store.open(session("user2", "203.0.113.8"));
    ASSERT_TRUE(churn.ok());
    ASSERT_FALSE(store.close(churn.value().id).has_value());
  }

  EXPECT_EQ(user_ids(store.list_open()), std::vector<std::string>{"user1"});
}

}  // namespace
