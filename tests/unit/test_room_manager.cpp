#include <string>
#include <vector>

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

std::string create(RoomManager& manager, const std::string& name = "") {
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

TEST(RoomManager, AnUnnamedRoomIsCalledByItsIdentifier) {
  // The whole point of the fallback living here: whatever screen or client
  // asked for the room, the name it ends up with is never empty.
  RoomManager manager = make_manager();
  const std::string id = create(manager, "");
  ASSERT_NE(manager.find(id), nullptr);
  EXPECT_EQ(manager.find(id)->name, id);
}

TEST(RoomManager, ANameOfNothingButWhitespaceIsNoName) {
  RoomManager manager = make_manager();
  const std::string id = create(manager, "   \t  ");
  ASSERT_NE(manager.find(id), nullptr);
  EXPECT_EQ(manager.find(id)->name, id);
}

TEST(RoomManager, TrimsTheNameItIsGiven) {
  RoomManager manager = make_manager();
  const std::string id = create(manager, "  Retro de sexta  ");
  ASSERT_NE(manager.find(id), nullptr);
  EXPECT_EQ(manager.find(id)->name, "Retro de sexta");
}

TEST(RoomManager, RefusesANameAnotherRoomAlreadyHas) {
  RoomManager manager = make_manager();
  const std::string first = create(manager, "Daily");
  ASSERT_FALSE(first.empty());

  auto second = manager.create_room("Daily");
  ASSERT_FALSE(second.ok());
  EXPECT_EQ(second.error().code, "room_name_taken");
  // Refused, and nothing left behind: an identifier was drawn and has to go
  // back, or a failed creation costs a room.
  EXPECT_EQ(manager.room_count(), 1u);
}

TEST(RoomManager, TheDuplicateCheckIgnoresCaseAndWhitespace) {
  RoomManager manager = make_manager();
  (void)create(manager, "Daily");

  // A named vector rather than a braced list in the loop head. That list
  // deduces to initializer_list<const char*>, so binding each element to a
  // const std::string& builds a temporary per turn, which is what GCC's
  // -Wrange-loop-construct is for and what MSVC never mentions.
  const std::vector<std::string> variants = {"daily", "DAILY", "  Daily  ", "dAiLy"};
  for (const std::string& variant : variants) {
    auto refused = manager.create_room(variant);
    EXPECT_FALSE(refused.ok()) << variant << " was accepted";
    if (!refused.ok()) {
      EXPECT_EQ(refused.error().code, "room_name_taken") << variant;
    }
  }
  EXPECT_EQ(manager.room_count(), 1u);
}

TEST(RoomManager, ClosingARoomFreesItsName) {
  // The rule is about rooms that exist. A name held forever by a room somebody
  // closed would be a name nobody can use and nobody can find.
  RoomManager manager = make_manager();
  const std::string first = create(manager, "Daily");
  ASSERT_TRUE(manager.remove_room(first).ok());

  auto again = manager.create_room("Daily");
  ASSERT_TRUE(again.ok()) << again.error().message;
  EXPECT_NE(again.value(), first);
}

TEST(RoomManager, UnnamedRoomsDoNotCollideWithEachOther) {
  // Each is named after its own identifier, and identifiers are already
  // unique, so the rule costs an unnamed room nothing.
  RoomManager manager = make_manager();
  const std::string first = create(manager, "");
  const std::string second = create(manager, "");
  ASSERT_FALSE(first.empty());
  ASSERT_FALSE(second.empty());
  EXPECT_NE(first, second);
  EXPECT_EQ(manager.room_count(), 2u);
}

TEST(RoomManager, AnUnnamedRoomSkipsAnIdentifierSomebodyTookAsAName) {
  // Nothing stops a person from calling their room "A26DCB", and the generator
  // is free to draw that identifier afterwards. The room that would have been
  // named after it takes another identifier instead of failing: it asked for
  // no name in particular, so there is nothing to refuse.
  //
  // A probe manager on the same seed says which identifiers are coming.
  RoomManager probe = make_manager();
  const std::string id1 = create(probe, "");
  const std::string id2 = create(probe, "");
  const std::string id3 = create(probe, "");
  ASSERT_NE(id2, id3);

  RoomManager manager = make_manager();
  // Draws id1 for itself, and wears id2 as a name chosen by hand.
  const std::string named = create(manager, id2);
  EXPECT_EQ(named, id1);

  // Now draws id2, whose fallback name is taken, and moves on to id3.
  const std::string unnamed = create(manager, "");
  EXPECT_NE(unnamed, id2);
  EXPECT_EQ(unnamed, id3);
  ASSERT_NE(manager.find(unnamed), nullptr);
  EXPECT_EQ(manager.find(unnamed)->name, unnamed);
}

TEST(RoomManager, LoadingKeepsDuplicateNamesTheStoreAlreadyHas) {
  // A database written before names had to be unique holds whatever it holds.
  // Refusing those at startup would delete rooms people still use to enforce a
  // rule that did not exist when they were made.
  dv::server::store::MemoryRoomStore store;
  ASSERT_FALSE(store
                   .upsert(dv::server::store::RoomRecord{
                       .id = "AAAAAA", .name = "room", .owner_id = "u1", .persistent = true})
                   .has_value());
  ASSERT_FALSE(store
                   .upsert(dv::server::store::RoomRecord{
                       .id = "BBBBBB", .name = "room", .owner_id = "u2", .persistent = true})
                   .has_value());

  RoomManager manager(RoomManager::Options{.id_seed = 1234u, .store = &store});
  EXPECT_EQ(manager.load_rooms(), 2u);
  ASSERT_NE(manager.find("AAAAAA"), nullptr);
  ASSERT_NE(manager.find("BBBBBB"), nullptr);
  EXPECT_EQ(manager.find("AAAAAA")->name, "room");
  EXPECT_EQ(manager.find("BBBBBB")->name, "room");

  // The rule still applies to anything created from now on.
  auto refused = manager.create_room("room");
  ASSERT_FALSE(refused.ok());
  EXPECT_EQ(refused.error().code, "room_name_taken");
}

TEST(RoomManager, TheUnnamedFallbackReachesTheStore) {
  // Not only the live map. A room created without a name and then reloaded
  // after a restart has to come back carrying the same thing.
  dv::server::store::MemoryRoomStore store;
  RoomManager manager(RoomManager::Options{.id_seed = 1234u, .store = &store});
  auto created = manager.create_room("");
  ASSERT_TRUE(created.ok());
  const std::string id = created.value();

  const auto record = store.find(id);
  ASSERT_TRUE(record.has_value());
  EXPECT_EQ(record->name, id);

  RoomManager reloaded(RoomManager::Options{.id_seed = 1234u, .store = &store});
  EXPECT_EQ(reloaded.load_rooms(), 1u);
  ASSERT_NE(reloaded.find(id), nullptr);
  EXPECT_EQ(reloaded.find(id)->name, id);
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

// What a refused room operation says, and not only which code it carries.
//
// These messages are not written for a log file alone. `Hub` relays them to
// the client, which puts them in a dialog, so somebody who joined a room twice
// used to read "f31d4c2809d51d780fdcc5e49d78340f is already in 332368" - two
// identifiers and nothing a person recognises. Asserting the text is what
// keeps it from drifting back: every test here was checking `code`, which is
// exactly why the wording could rot unnoticed.
TEST(RoomManager, RefusalsNamePeopleAndRoomsRatherThanIdentifiers) {
  RoomManager manager = make_manager(2);
  const std::string id = create(manager, "The Room");
  ASSERT_FALSE(manager.join(id, user("u1")).has_value());
  ASSERT_FALSE(manager.join(id, user("u2")).has_value());

  const auto twice = manager.join(id, user("u1"));
  ASSERT_TRUE(twice.has_value());
  EXPECT_EQ(twice->message, "Name u1 is already in The Room");

  const auto full = manager.join(id, user("u3"));
  ASSERT_TRUE(full.has_value());
  EXPECT_EQ(full->message, "room The Room is full");

  ASSERT_FALSE(manager.start_screen_share(id, "u1").has_value());
  const auto busy = manager.start_screen_share(id, "u2");
  ASSERT_TRUE(busy.has_value());
  EXPECT_EQ(busy->message, "Name u1 is already sharing in The Room");

  // The room resolves even where the person cannot. Somebody who is not a
  // participant has no name this class can read, and inventing a lookup so an
  // error path could print a nicer word would be the wrong trade.
  const auto absent = manager.leave(id, "u9");
  ASSERT_TRUE(absent.has_value());
  EXPECT_EQ(absent->message, "u9 is not in The Room");
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
