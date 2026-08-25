#include <string>

#include <gtest/gtest.h>

#include "rooms/room_manager.hpp"
#include "store/memory_store.hpp"

namespace {

using dv::models::User;
using dv::server::RoomManager;

RoomManager make_manager(int capacity = 5) {
  // A fixed seed keeps the generated room identifiers reproducible.
  return RoomManager(RoomManager::Options{capacity, 1234u});
}

std::string create(RoomManager& manager, const std::string& name = "dev-room") {
  auto created = manager.create_room(name);
  EXPECT_TRUE(created.ok());
  return created.ok() ? created.value() : std::string{};
}

User user(const std::string& id) {
  return User{id, "Name " + id, ""};
}

TEST(RoomManager, CreatesRoomsWithValidIdentifiers) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_TRUE(dv::models::is_valid_room_id(id)) << id;
  EXPECT_EQ(manager.room_count(), 1u);
}

TEST(RoomManager, GeneratesDistinctIdentifiers) {
  RoomManager manager = make_manager();
  const std::string first = create(manager);
  const std::string second = create(manager);
  EXPECT_NE(first, second);
  EXPECT_EQ(manager.room_count(), 2u);
}

TEST(RoomManager, KeepsTheRoomName) {
  RoomManager manager = make_manager();
  const std::string id = create(manager, "dev-room");
  ASSERT_NE(manager.find(id), nullptr);
  EXPECT_EQ(manager.find(id)->name, "dev-room");
}

TEST(RoomManager, JoiningAddsAParticipant) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  ASSERT_NE(manager.find(id), nullptr);
  EXPECT_EQ(manager.find(id)->size(), 1u);
  EXPECT_TRUE(manager.find(id)->contains("u1"));
}

TEST(RoomManager, JoiningAnUnknownRoomFails) {
  RoomManager manager = make_manager();
  const auto failure = manager.join("ABCDEF", user("u1"));
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "room_not_found");
}

TEST(RoomManager, JoiningTwiceFails) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  const auto failure = manager.join(id, user("u1"));
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "already_in_room");
}

TEST(RoomManager, EnforcesTheCapacityLimit) {
  RoomManager manager = make_manager(5);
  const std::string id = create(manager);
  for (int i = 0; i < 5; ++i) {
    EXPECT_FALSE(manager.join(id, user("u" + std::to_string(i))).has_value());
  }
  const auto failure = manager.join(id, user("u5"));
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "room_full");
  EXPECT_EQ(manager.find(id)->size(), 5u);
}

TEST(RoomManager, JoiningASecondRoomLeavesTheFirst) {
  RoomManager manager = make_manager();
  const std::string first = create(manager, "a");
  const std::string second = create(manager, "b");
  EXPECT_FALSE(manager.join(first, user("u1")).has_value());
  EXPECT_FALSE(manager.join(second, user("u1")).has_value());

  // The first room is empty but still there, and still theirs to come back to.
  ASSERT_NE(manager.find(first), nullptr);
  EXPECT_TRUE(manager.find(first)->participants.empty());
  EXPECT_TRUE(manager.find(second)->contains("u1"));
  EXPECT_EQ(manager.room_of("u1"), second);
}

TEST(RoomManager, LeavingRemovesTheParticipant) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  EXPECT_FALSE(manager.join(id, user("u2")).has_value());
  EXPECT_FALSE(manager.leave(id, "u1").has_value());
  EXPECT_FALSE(manager.find(id)->contains("u1"));
  EXPECT_TRUE(manager.find(id)->contains("u2"));
}

TEST(RoomManager, TheRoomSurvivesOnceEmpty) {
  // It used to be erased here, which meant stepping out of your own room
  // destroyed it and the identifier stopped working.
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  EXPECT_FALSE(manager.leave(id, "u1").has_value());
  ASSERT_NE(manager.find(id), nullptr);
  EXPECT_TRUE(manager.find(id)->participants.empty());
  EXPECT_EQ(manager.room_count(), 1u);
  EXPECT_FALSE(manager.room_of("u1").has_value());

  // And it can be walked back into, which is the whole point.
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  EXPECT_EQ(manager.find(id)->participants.size(), 1u);
}

TEST(RoomManager, CreatingARoomWritesItToTheStore) {
  // Nothing used to reach the store unless an administrator had asked for a
  // persistent room, so an ordinary one existed only in this process and the
  // database stayed empty.
  dv::server::store::MemoryRoomStore store;
  RoomManager manager(
      RoomManager::Options{.max_participants_per_room = 5, .id_seed = 1234u, .store = &store});
  const std::string id = create(manager, "dev-room");

  const auto record = store.find(id);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->id, id);
  EXPECT_EQ(record->name, "dev-room");
  EXPECT_TRUE(record->persistent);
  EXPECT_EQ(store.list().size(), 1u);
}

TEST(RoomManager, RoomsComeBackFromTheStoreOnStartup) {
  dv::server::store::MemoryRoomStore store;
  std::string id;
  {
    RoomManager before(
        RoomManager::Options{.max_participants_per_room = 5, .id_seed = 1234u, .store = &store});
    id = create(before, "dev-room");
    EXPECT_FALSE(before.join(id, user("u1")).has_value());
    EXPECT_FALSE(before.leave(id, "u1").has_value());
  }

  // A new manager over the same store is what a restart looks like.
  RoomManager after(
      RoomManager::Options{.max_participants_per_room = 5, .id_seed = 4321u, .store = &store});
  EXPECT_EQ(after.load_rooms(), 1u);
  ASSERT_NE(after.find(id), nullptr);
  EXPECT_EQ(after.find(id)->name, "dev-room");
  // Empty: who was inside did not survive the process.
  EXPECT_TRUE(after.find(id)->participants.empty());
}

TEST(RoomManager, ClosingARoomTakesItOutOfTheStore) {
  // The one path that ends a room now, so it is the one that has to clean up
  // after itself: a record left behind would be reloaded at the next restart.
  dv::server::store::MemoryRoomStore store;
  RoomManager manager(
      RoomManager::Options{.max_participants_per_room = 5, .id_seed = 1234u, .store = &store});
  const std::string id = create(manager);
  ASSERT_TRUE(manager.remove_room(id).ok());
  EXPECT_FALSE(store.find(id).has_value());
  EXPECT_EQ(manager.find(id), nullptr);
}

TEST(RoomManager, LeavingARoomTheUserIsNotInFails) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  const auto failure = manager.leave(id, "u2");
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "not_in_room");
}

TEST(RoomManager, RemoveFromAnyRoomReportsWhereTheUserWas) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  EXPECT_FALSE(manager.join(id, user("u2")).has_value());
  EXPECT_EQ(manager.remove_from_any_room("u1"), id);
  EXPECT_FALSE(manager.remove_from_any_room("u1").has_value());
}

TEST(RoomManager, MuteChangesParticipantState) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  EXPECT_FALSE(manager.set_muted(id, "u1", true).has_value());
  EXPECT_TRUE(manager.find(id)->find("u1")->muted);
  EXPECT_FALSE(manager.set_muted(id, "u1", false).has_value());
  EXPECT_FALSE(manager.find(id)->find("u1")->muted);
}

TEST(RoomManager, OnlyOneParticipantCanShareAtATime) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  EXPECT_FALSE(manager.join(id, user("u2")).has_value());

  EXPECT_FALSE(manager.start_screen_share(id, "u1").has_value());
  const auto failure = manager.start_screen_share(id, "u2");
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "screen_share_busy");
  EXPECT_EQ(manager.find(id)->screen_sharer()->user.id, "u1");
}

TEST(RoomManager, RestartingYourOwnShareIsNotAConflict) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  EXPECT_FALSE(manager.start_screen_share(id, "u1").has_value());
  EXPECT_FALSE(manager.start_screen_share(id, "u1").has_value());
}

TEST(RoomManager, StoppingAShareFreesItForOthers) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  EXPECT_FALSE(manager.join(id, user("u2")).has_value());
  EXPECT_FALSE(manager.start_screen_share(id, "u1").has_value());
  EXPECT_FALSE(manager.stop_screen_share(id, "u1").has_value());
  EXPECT_FALSE(manager.start_screen_share(id, "u2").has_value());
  EXPECT_EQ(manager.find(id)->screen_sharer()->user.id, "u2");
}

TEST(RoomManager, LeavingReleasesTheScreenShare) {
  RoomManager manager = make_manager();
  const std::string id = create(manager);
  EXPECT_FALSE(manager.join(id, user("u1")).has_value());
  EXPECT_FALSE(manager.join(id, user("u2")).has_value());
  EXPECT_FALSE(manager.start_screen_share(id, "u1").has_value());
  EXPECT_FALSE(manager.leave(id, "u1").has_value());
  EXPECT_EQ(manager.find(id)->screen_sharer(), nullptr);
  EXPECT_FALSE(manager.start_screen_share(id, "u2").has_value());
}

TEST(RoomManager, OperationsOnAnUnknownRoomAllFailTheSameWay) {
  RoomManager manager = make_manager();
  EXPECT_EQ(manager.set_muted("ABCDEF", "u1", true)->code, "room_not_found");
  EXPECT_EQ(manager.start_screen_share("ABCDEF", "u1")->code, "room_not_found");
  EXPECT_EQ(manager.stop_screen_share("ABCDEF", "u1")->code, "room_not_found");
  EXPECT_EQ(manager.leave("ABCDEF", "u1")->code, "room_not_found");
}

}  // namespace
