#include <gtest/gtest.h>

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

}  // namespace
