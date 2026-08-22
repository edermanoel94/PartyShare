#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <dv/models/chat.hpp>

#include "store/chat_store.hpp"
#include "store/memory_store.hpp"

namespace {

using dv::models::ChatMessage;
using dv::server::store::ChatStore;
using dv::server::store::clamp_chat_limit;
using dv::server::store::MemoryChatStore;

ChatMessage message(const std::string& room_id, const std::string& text) {
  ChatMessage value;
  value.room_id = room_id;
  value.user_id = "user1";
  value.display_name = "Ana";
  value.text = text;
  return value;
}

/// The text of everything `list` returned, which is what every ordering
/// assertion below is really about.
std::vector<std::string> texts(const std::vector<ChatMessage>& messages) {
  std::vector<std::string> result;
  result.reserve(messages.size());
  for (const ChatMessage& value : messages) {
    result.push_back(value.text);
  }
  return result;
}

TEST(MemoryChatStore, AppendStampsAnIdentifierAndATime) {
  MemoryChatStore store;

  const auto written = store.append(message("8F42A1", "hello"));
  ASSERT_TRUE(written.ok()) << written.error().message;
  EXPECT_FALSE(written.value().id.empty());
  EXPECT_GT(written.value().timestamp_seconds, 0);
  EXPECT_EQ(written.value().text, "hello");
}

TEST(MemoryChatStore, IdentifiersAreNotReused) {
  MemoryChatStore store;

  const auto first = store.append(message("8F42A1", "one"));
  const auto second = store.append(message("8F42A1", "two"));
  // Across rooms as well as within one: the identifier is what a client uses
  // to tell two messages apart, and two rooms can be open in one session.
  const auto third = store.append(message("B00B00", "three"));

  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());
  ASSERT_TRUE(third.ok());
  EXPECT_NE(first.value().id, second.value().id);
  EXPECT_NE(second.value().id, third.value().id);
  EXPECT_NE(first.value().id, third.value().id);
}

TEST(MemoryChatStore, AGivenTimeIsKept) {
  // The store stamps only what arrives without one. A message that already
  // carries a time is being replayed or imported, and rewriting it would move
  // it in the conversation.
  MemoryChatStore store;

  ChatMessage dated = message("8F42A1", "hello");
  dated.timestamp_seconds = 1755676800;
  const auto written = store.append(dated);

  ASSERT_TRUE(written.ok());
  EXPECT_EQ(written.value().timestamp_seconds, 1755676800);
}

TEST(MemoryChatStore, ListsOldestFirst) {
  MemoryChatStore store;
  for (const char* text : {"one", "two", "three"}) {
    ASSERT_TRUE(store.append(message("8F42A1", text)).ok());
  }

  EXPECT_EQ(texts(store.list("8F42A1", 0)), (std::vector<std::string>{"one", "two", "three"}));
}

TEST(MemoryChatStore, TheWindowIsTakenFromTheEnd) {
  MemoryChatStore store;
  for (const char* text : {"one", "two", "three", "four"}) {
    ASSERT_TRUE(store.append(message("8F42A1", text)).ok());
  }

  // The newest two, still in the order they were said. Asking for a window is
  // asking for the end of the conversation, not the beginning of it.
  EXPECT_EQ(texts(store.list("8F42A1", 2)), (std::vector<std::string>{"three", "four"}));
}

TEST(MemoryChatStore, RoomsDoNotSeeEachOther) {
  MemoryChatStore store;
  ASSERT_TRUE(store.append(message("8F42A1", "ours")).ok());
  ASSERT_TRUE(store.append(message("B00B00", "theirs")).ok());

  EXPECT_EQ(texts(store.list("8F42A1", 0)), (std::vector<std::string>{"ours"}));
  EXPECT_EQ(texts(store.list("B00B00", 0)), (std::vector<std::string>{"theirs"}));
}

TEST(MemoryChatStore, AnUnknownRoomIsEmptyRatherThanAFailure) {
  const MemoryChatStore store;
  EXPECT_TRUE(store.list("8F42A1", 0).empty());
}

TEST(MemoryChatStore, ClearForgetsOneRoomAndLeavesTheRest) {
  MemoryChatStore store;
  ASSERT_TRUE(store.append(message("8F42A1", "ours")).ok());
  ASSERT_TRUE(store.append(message("B00B00", "theirs")).ok());

  EXPECT_FALSE(store.clear("8F42A1").has_value());
  EXPECT_TRUE(store.list("8F42A1", 0).empty());
  EXPECT_EQ(texts(store.list("B00B00", 0)), (std::vector<std::string>{"theirs"}));
}

TEST(MemoryChatStore, ClearingARoomThatSaidNothingSucceeds) {
  // Called from RoomManager whenever a room stops existing, and most rooms end
  // without anybody having typed in them. A failure there would be noise in
  // the log on the ordinary path.
  MemoryChatStore store;
  EXPECT_FALSE(store.clear("8F42A1").has_value());
}

TEST(MemoryChatStore, TheOldestAreDroppedOnceTheRoomIsFull) {
  MemoryChatStore store;
  for (std::size_t i = 0; i < MemoryChatStore::kCapacityPerRoom + 10; ++i) {
    ASSERT_TRUE(store.append(message("8F42A1", std::to_string(i))).ok());
  }

  const auto kept = store.list("8F42A1", ChatStore::kMaxLimit);
  ASSERT_FALSE(kept.empty());
  // The end of the conversation survived and the beginning did not, which is
  // the right way round for a cap on something people are still adding to.
  EXPECT_EQ(kept.back().text, std::to_string(MemoryChatStore::kCapacityPerRoom + 9));
  EXPECT_NE(kept.front().text, "0");
}

TEST(ChatLimit, ZeroAndBelowAskForTheDefault) {
  EXPECT_EQ(clamp_chat_limit(0), ChatStore::kDefaultLimit);
  EXPECT_EQ(clamp_chat_limit(-1), ChatStore::kDefaultLimit);
}

TEST(ChatLimit, IsCappedAtTheMaximum) {
  EXPECT_EQ(clamp_chat_limit(ChatStore::kMaxLimit + 1), ChatStore::kMaxLimit);
  EXPECT_EQ(clamp_chat_limit(10), 10);
}

}  // namespace
