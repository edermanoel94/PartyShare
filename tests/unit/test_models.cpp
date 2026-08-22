#include <string>

#include <gtest/gtest.h>

#include <dv/models/chat.hpp>
#include <dv/models/room.hpp>
#include <dv/models/user.hpp>

namespace {

using dv::models::Participant;
using dv::models::Room;
using dv::models::User;

Room make_room() {
  Room room;
  room.id = "8F42A1";
  room.name = "dev-room";
  // Named rather than positional. Both types gained a field in the middle
  // while roles were added, and positional initialisers quietly shifted their
  // meaning: what had been "sharing" became "muted by an administrator", and
  // nothing but a failing assertion said so.
  room.participants = {
      Participant{.user = User{.id = "user1", .display_name = "Ana"}},
      Participant{.user = User{.id = "user2", .display_name = "Bruno"}, .muted = true},
      Participant{.user = User{.id = "user3", .display_name = "Carla"}, .sharing_screen = true},
  };
  return room;
}

TEST(Room, FindsAParticipantById) {
  const Room room = make_room();
  const Participant* found = room.find("user2");
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->user.display_name, "Bruno");
  EXPECT_TRUE(found->muted);
}

TEST(Room, ReturnsNullForAnUnknownParticipant) {
  const Room room = make_room();
  EXPECT_EQ(room.find("user9"), nullptr);
  EXPECT_FALSE(room.contains("user9"));
  EXPECT_TRUE(room.contains("user1"));
}

TEST(Room, MutableFindAllowsUpdatingState) {
  Room room = make_room();
  Participant* found = room.find("user1");
  ASSERT_NE(found, nullptr);
  found->muted = true;
  EXPECT_TRUE(room.find("user1")->muted);
}

TEST(Room, ReportsTheSingleScreenSharer) {
  const Room room = make_room();
  const Participant* sharer = room.screen_sharer();
  ASSERT_NE(sharer, nullptr);
  EXPECT_EQ(sharer->user.id, "user3");
}

TEST(Room, ReportsNoSharerWhenNobodyIsSharing) {
  Room room = make_room();
  room.find("user3")->sharing_screen = false;
  EXPECT_EQ(room.screen_sharer(), nullptr);
}

TEST(Room, SizeCountsParticipants) {
  EXPECT_EQ(make_room().size(), 3u);
}

TEST(RoomId, AcceptsTheSpecExample) {
  EXPECT_TRUE(dv::models::is_valid_room_id("8F42A1"));
}

TEST(RoomId, RejectsTheWrongLength) {
  EXPECT_FALSE(dv::models::is_valid_room_id("8F42A"));
  EXPECT_FALSE(dv::models::is_valid_room_id("8F42A11"));
  EXPECT_FALSE(dv::models::is_valid_room_id(""));
}

TEST(RoomId, RejectsLowercaseAndNonHexCharacters) {
  EXPECT_FALSE(dv::models::is_valid_room_id("8f42a1"));
  EXPECT_FALSE(dv::models::is_valid_room_id("8F42AG"));
  EXPECT_FALSE(dv::models::is_valid_room_id("8F42A-"));
}

TEST(User, ComparesByValue) {
  EXPECT_EQ((User{"1", "Ana", ""}), (User{"1", "Ana", ""}));
  EXPECT_NE((User{"1", "Ana", ""}), (User{"2", "Ana", ""}));
}

TEST(ChatText, TrimsTheWhitespaceAround) {
  EXPECT_EQ(dv::models::trim_chat_text("  hello  "), "hello");
  EXPECT_EQ(dv::models::trim_chat_text("\n\thello\r\n"), "hello");
  // Only around. What somebody typed in the middle of a sentence is theirs.
  EXPECT_EQ(dv::models::trim_chat_text("  a  b  "), "a  b");
}

TEST(ChatText, RejectsWhatIsOnlyWhitespace) {
  EXPECT_FALSE(dv::models::is_valid_chat_text(""));
  EXPECT_FALSE(dv::models::is_valid_chat_text("   "));
  EXPECT_FALSE(dv::models::is_valid_chat_text("\n\t\r"));
  EXPECT_TRUE(dv::models::is_valid_chat_text(" x "));
}

TEST(ChatText, MeasuresTheLimitAfterTrimming) {
  const std::string at_the_limit(dv::models::kMaxChatTextBytes, 'x');
  EXPECT_TRUE(dv::models::is_valid_chat_text(at_the_limit));
  EXPECT_FALSE(dv::models::is_valid_chat_text(at_the_limit + "x"));

  // Padded past the limit with spaces that will be thrown away. Checking the
  // length before trimming would refuse a message that is going to fit.
  EXPECT_TRUE(dv::models::is_valid_chat_text("   " + at_the_limit + "   "));
}

TEST(ChatText, LeavesMultibyteCharactersAlone) {
  // Trimming walks bytes, so this is the case that would break if a
  // continuation byte were ever mistaken for whitespace.
  const std::string text = "  Ana Ferrão diz olá  ";
  EXPECT_EQ(dv::models::trim_chat_text(text), "Ana Ferrão diz olá");
}

TEST(ChatText, AcceptsEmojiAndDoesNotCutThemInHalf) {
  // Four bytes each, and the last one sits right where the trim starts
  // walking backwards. A trim that mistook a continuation byte for whitespace
  // would leave half a character behind, which is not a string any more.
  const std::string text = "  deploy feito 🚀🎉  ";
  EXPECT_TRUE(dv::models::is_valid_chat_text(text));
  EXPECT_EQ(dv::models::trim_chat_text(text), "deploy feito 🚀🎉");

  // A message that is nothing but an emoji is a message. Nobody has to write
  // words to say something.
  EXPECT_TRUE(dv::models::is_valid_chat_text("👍"));
  EXPECT_EQ(dv::models::trim_chat_text(" 👍 "), "👍");
}

TEST(ChatText, TheLimitIsBytesSoEmojiCostFourOfThem) {
  // Worth pinning rather than discovering: the limit is bytes, so the number
  // of characters that fit depends on what they are. 2000 emoji do not fit,
  // 500 do, and both of those are the rule working rather than a bug.
  const std::string one = "🚀";
  ASSERT_EQ(one.size(), 4U);

  std::string exactly_full;
  for (std::size_t i = 0; i < dv::models::kMaxChatTextBytes / one.size(); ++i) {
    exactly_full += one;
  }
  EXPECT_EQ(exactly_full.size(), dv::models::kMaxChatTextBytes);
  EXPECT_TRUE(dv::models::is_valid_chat_text(exactly_full));
  EXPECT_FALSE(dv::models::is_valid_chat_text(exactly_full + one));
}

}  // namespace
