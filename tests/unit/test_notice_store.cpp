#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <dv/models/notice.hpp>

#include "store/memory_store.hpp"
#include "store/notice_store.hpp"

namespace {

using dv::models::Notice;
using dv::server::store::MemoryNoticeStore;
using dv::server::store::NoticeStore;

Notice notice(const std::string& user_id, const std::string& text) {
  Notice value;
  value.user_id = user_id;
  value.from_user_id = "admin1";
  value.from_display_name = "Ana";
  value.text = text;
  return value;
}

/// The text of everything that came back, which is what the ordering
/// assertions below are really about.
std::vector<std::string> texts(const std::vector<Notice>& notices) {
  std::vector<std::string> result;
  result.reserve(notices.size());
  for (const Notice& value : notices) {
    result.push_back(value.text);
  }
  return result;
}

TEST(MemoryNoticeStore, AppendStampsAnIdentifierAndATime) {
  MemoryNoticeStore store;

  const auto written = store.append(notice("user1", "the meeting moved"));
  ASSERT_TRUE(written.ok()) << written.error().message;
  EXPECT_FALSE(written.value().id.empty());
  EXPECT_GT(written.value().created_at, 0);
  EXPECT_EQ(written.value().acknowledged_at, 0);
  EXPECT_FALSE(written.value().acknowledged());
}

TEST(MemoryNoticeStore, ATimeTheCallerSuppliedIsKept) {
  MemoryNoticeStore store;

  Notice supplied = notice("user1", "read this");
  supplied.created_at = 1000;

  const auto written = store.append(supplied);
  ASSERT_TRUE(written.ok()) << written.error().message;
  EXPECT_EQ(written.value().created_at, 1000);
}

TEST(MemoryNoticeStore, PendingIsOnlyThisAccountAndOnlyWhatIsUnanswered) {
  MemoryNoticeStore store;

  const auto first = store.append(notice("user1", "one"));
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(store.append(notice("user2", "not yours")).ok());
  ASSERT_TRUE(store.append(notice("user1", "two")).ok());

  EXPECT_EQ(texts(store.pending_for("user1")), (std::vector<std::string>{"one", "two"}));

  ASSERT_TRUE(store.acknowledge(first.value().id, "user1").ok());
  EXPECT_EQ(texts(store.pending_for("user1")), std::vector<std::string>{"two"});
}

TEST(MemoryNoticeStore, PendingIsOldestFirst) {
  MemoryNoticeStore store;

  for (int index = 0; index < 5; ++index) {
    ASSERT_TRUE(store.append(notice("user1", std::to_string(index))).ok());
  }

  EXPECT_EQ(texts(store.pending_for("user1")), (std::vector<std::string>{"0", "1", "2", "3", "4"}));
}

TEST(MemoryNoticeStore, OneDeliveryIsCapped) {
  MemoryNoticeStore store;

  const int written = NoticeStore::kMaxPendingPerDelivery + 5;
  for (int index = 0; index < written; ++index) {
    ASSERT_TRUE(store.append(notice("user1", std::to_string(index))).ok());
  }

  const auto pending = store.pending_for("user1");
  ASSERT_EQ(static_cast<int>(pending.size()), NoticeStore::kMaxPendingPerDelivery);
  // The oldest, not the newest: nothing is dropped, so the ones left over are
  // the ones that arrive next time.
  EXPECT_EQ(pending.front().text, "0");
}

TEST(MemoryNoticeStore, AcknowledgingStampsTheTimeAndAnswersTheRow) {
  MemoryNoticeStore store;

  const auto written = store.append(notice("user1", "read this"));
  ASSERT_TRUE(written.ok());

  const auto acknowledged = store.acknowledge(written.value().id, "user1");
  ASSERT_TRUE(acknowledged.ok()) << acknowledged.error().message;
  EXPECT_TRUE(acknowledged.value().acknowledged());
  EXPECT_GT(acknowledged.value().acknowledged_at, 0);
  EXPECT_EQ(acknowledged.value().text, "read this");
}

TEST(MemoryNoticeStore, AcknowledgingTwiceKeepsTheFirstTime) {
  MemoryNoticeStore store;

  const auto written = store.append(notice("user1", "read this"));
  ASSERT_TRUE(written.ok());

  const auto first = store.acknowledge(written.value().id, "user1");
  ASSERT_TRUE(first.ok());
  // Not a failure, because a client that reconnected and was handed the same
  // notice again is doing the only correct thing with it.
  const auto second = store.acknowledge(written.value().id, "user1");
  ASSERT_TRUE(second.ok()) << second.error().message;
  EXPECT_EQ(second.value().acknowledged_at, first.value().acknowledged_at);
}

TEST(MemoryNoticeStore, SomebodyElsesNoticeCannotBeAcknowledged) {
  MemoryNoticeStore store;

  const auto written = store.append(notice("user1", "for user1 only"));
  ASSERT_TRUE(written.ok());

  const auto stolen = store.acknowledge(written.value().id, "user2");
  ASSERT_FALSE(stolen.ok());
  // The same code an identifier belonging to nobody gets, so that being
  // refused says nothing about whether the notice exists.
  EXPECT_EQ(stolen.error().code, "notice_not_found");
  EXPECT_EQ(store.acknowledge("nothing", "user2").error().code, "notice_not_found");

  // And it is still waiting for the person it was addressed to.
  EXPECT_EQ(texts(store.pending_for("user1")), std::vector<std::string>{"for user1 only"});
}

TEST(MemoryNoticeStore, ClearingAnAccountLeavesTheOthersAlone) {
  MemoryNoticeStore store;

  ASSERT_TRUE(store.append(notice("user1", "one")).ok());
  ASSERT_TRUE(store.append(notice("user2", "two")).ok());

  ASSERT_FALSE(store.clear_for("user1").has_value());
  EXPECT_TRUE(store.pending_for("user1").empty());
  EXPECT_EQ(texts(store.pending_for("user2")), std::vector<std::string>{"two"});
}

TEST(MemoryNoticeStore, ClearingAnAccountThatWasNeverWrittenToIsNotAFailure) {
  MemoryNoticeStore store;
  EXPECT_FALSE(store.clear_for("nobody").has_value());
}

TEST(MemoryNoticeStore, TheCapTakesTheReadOnesFirst) {
  MemoryNoticeStore store;

  // One that has been read, at the front, and then the store filled to its
  // capacity. The acknowledged one is what the next write pushes out, and the
  // unread one written beside it survives.
  const auto read = store.append(notice("user1", "already read"));
  ASSERT_TRUE(read.ok());
  ASSERT_TRUE(store.acknowledge(read.value().id, "user1").ok());

  ASSERT_TRUE(store.append(notice("user1", "the oldest unread")).ok());
  for (std::size_t index = 0; index < MemoryNoticeStore::kCapacity - 1; ++index) {
    ASSERT_TRUE(store.append(notice("user2", "filler")).ok());
  }

  const auto pending = store.pending_for("user1");
  ASSERT_EQ(pending.size(), 1U);
  EXPECT_EQ(pending.front().text, "the oldest unread");
}

TEST(NoticeText, EmptyAndOversizedAreRefused) {
  EXPECT_FALSE(dv::models::is_valid_notice_text(""));
  EXPECT_FALSE(dv::models::is_valid_notice_text("   \t\n "));
  EXPECT_TRUE(dv::models::is_valid_notice_text("  padded  "));
  EXPECT_EQ(dv::models::trim_notice_text("  padded  "), "padded");

  EXPECT_TRUE(dv::models::is_valid_notice_text(std::string(dv::models::kMaxNoticeTextBytes, 'a')));
  EXPECT_FALSE(
      dv::models::is_valid_notice_text(std::string(dv::models::kMaxNoticeTextBytes + 1, 'a')));
  // The length is measured after the trim, because that is what gets stored.
  EXPECT_TRUE(dv::models::is_valid_notice_text(
      " " + std::string(dv::models::kMaxNoticeTextBytes, 'a') + " "));
}

}  // namespace
