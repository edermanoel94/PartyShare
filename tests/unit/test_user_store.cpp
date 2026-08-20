#include <memory>
#include <string>

#include <gtest/gtest.h>

#include <dv/models/user.hpp>

#include "store/memory_store.hpp"

namespace {

using dv::models::Role;
using dv::server::store::Account;
using dv::server::store::MemoryAuditLog;
using dv::server::store::MemoryRoomStore;
using dv::server::store::MemoryUserStore;
using dv::server::store::RoomRecord;

Account account(const std::string& username, Role role = Role::User) {
  Account value;
  value.username = username;
  value.user.id = "id-" + username;
  value.user.display_name = username;
  value.user.role = role;
  value.salt_hex = "73616c74";
  value.password_hash_hex = "68617368";
  return value;
}

// --- accounts ----------------------------------------------------------------

TEST(UserStore, StoresAndFindsAnAccountBothWays) {
  MemoryUserStore store;
  ASSERT_FALSE(store.create(account("ana", Role::Admin)).has_value());

  const auto by_name = store.find_by_username("ana");
  ASSERT_TRUE(by_name.has_value());
  EXPECT_EQ(by_name->user.id, "id-ana");
  EXPECT_EQ(by_name->user.role, Role::Admin);

  const auto by_id = store.find_by_id("id-ana");
  ASSERT_TRUE(by_id.has_value());
  EXPECT_EQ(by_id->username, "ana");
}

TEST(UserStore, AnUnknownAccountIsNothingRatherThanAnError) {
  const MemoryUserStore store;
  EXPECT_FALSE(store.find_by_username("nobody").has_value());
  EXPECT_FALSE(store.find_by_id("id-nobody").has_value());
}

TEST(UserStore, RefusesADuplicateUsername) {
  MemoryUserStore store;
  ASSERT_FALSE(store.create(account("ana")).has_value());

  const auto failure = store.create(account("ana"));
  ASSERT_TRUE(failure.has_value());
  EXPECT_EQ(failure->code, "user_exists");
}

TEST(UserStore, StampsTheCreationTimeAndKeepsItThroughAnUpdate) {
  MemoryUserStore store;
  ASSERT_FALSE(store.create(account("ana")).has_value());

  const auto created = store.find_by_username("ana");
  ASSERT_TRUE(created.has_value());
  ASSERT_GT(created->created_at, 0);

  // Not the caller's to rewrite: it records when the account came into
  // existence, not when it was last written.
  auto changed = *created;
  changed.created_at = 1;
  changed.user.display_name = "Ana Maria";
  ASSERT_FALSE(store.update(changed).has_value());

  const auto after = store.find_by_username("ana");
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->created_at, created->created_at);
  EXPECT_EQ(after->user.display_name, "Ana Maria");
}

TEST(UserStore, UpdatingOrRemovingSomethingAbsentIsAnError) {
  MemoryUserStore store;
  EXPECT_EQ(store.update(account("ghost"))->code, "user_not_found");
  EXPECT_EQ(store.remove("id-ghost")->code, "user_not_found");
}

TEST(UserStore, RemovesAnAccount) {
  MemoryUserStore store;
  ASSERT_FALSE(store.create(account("ana")).has_value());
  ASSERT_FALSE(store.remove("id-ana").has_value());
  EXPECT_FALSE(store.find_by_username("ana").has_value());
  EXPECT_TRUE(store.list().empty());
}

TEST(UserStore, CountsTheAccountsHoldingARole) {
  MemoryUserStore store;
  ASSERT_FALSE(store.create(account("ana", Role::Admin)).has_value());
  ASSERT_FALSE(store.create(account("bruno", Role::User)).has_value());
  ASSERT_FALSE(store.create(account("carla", Role::Admin)).has_value());

  EXPECT_EQ(store.count_with_role(Role::Admin), 2U);
  EXPECT_EQ(store.count_with_role(Role::User), 1U);

  // This is what protects the last administrator, so it has to follow a
  // demotion and not only a deletion.
  auto carla = *store.find_by_username("carla");
  carla.user.role = Role::User;
  ASSERT_FALSE(store.update(carla).has_value());
  EXPECT_EQ(store.count_with_role(Role::Admin), 1U);
}

TEST(UserStore, ListsInTheOrderAccountsWereCreated) {
  MemoryUserStore store;
  ASSERT_FALSE(store.create(account("ana")).has_value());
  ASSERT_FALSE(store.create(account("bruno")).has_value());

  const auto all = store.list();
  ASSERT_EQ(all.size(), 2U);
  EXPECT_EQ(all.front().username, "ana");
  EXPECT_EQ(all.back().username, "bruno");
}

// --- rooms -------------------------------------------------------------------

TEST(RoomStore, UpsertCreatesThenReplaces) {
  MemoryRoomStore store;
  ASSERT_FALSE(
      store.upsert(RoomRecord{.id = "8F42A1", .name = "standup", .persistent = true}).has_value());

  const auto created = store.find("8F42A1");
  ASSERT_TRUE(created.has_value());
  EXPECT_EQ(created->name, "standup");
  ASSERT_GT(created->created_at, 0);

  ASSERT_FALSE(
      store.upsert(RoomRecord{.id = "8F42A1", .name = "retro", .persistent = true}).has_value());

  const auto replaced = store.find("8F42A1");
  ASSERT_TRUE(replaced.has_value());
  EXPECT_EQ(replaced->name, "retro");
  EXPECT_EQ(replaced->created_at, created->created_at);
  EXPECT_EQ(store.list().size(), 1U);
}

TEST(RoomStore, RemovingSomethingAbsentIsAnError) {
  MemoryRoomStore store;
  EXPECT_EQ(store.remove("8F42A1")->code, "room_not_found");
}

// --- the audit log -----------------------------------------------------------

dv::models::AuditEntry entry(const std::string& actor, const std::string& action) {
  dv::models::AuditEntry value;
  value.actor_id = actor;
  value.action = action;
  return value;
}

TEST(AuditLog, ReturnsTheNewestFirst) {
  MemoryAuditLog log;
  ASSERT_FALSE(log.append(entry("ana", "kick")).has_value());
  ASSERT_FALSE(log.append(entry("ana", "force_mute")).has_value());

  const auto entries = log.list(0, {});
  ASSERT_EQ(entries.size(), 2U);
  EXPECT_EQ(entries.front().action, "force_mute");
  EXPECT_EQ(entries.back().action, "kick");
}

TEST(AuditLog, StampsAnIdentifierAndATime) {
  MemoryAuditLog log;
  ASSERT_FALSE(log.append(entry("ana", "kick")).has_value());

  const auto entries = log.list(0, {});
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_FALSE(entries.front().id.empty());
  EXPECT_GT(entries.front().timestamp_seconds, 0);
}

TEST(AuditLog, FiltersByActor) {
  MemoryAuditLog log;
  ASSERT_FALSE(log.append(entry("ana", "kick")).has_value());
  ASSERT_FALSE(log.append(entry("carla", "kick")).has_value());

  const auto entries = log.list(0, "carla");
  ASSERT_EQ(entries.size(), 1U);
  EXPECT_EQ(entries.front().actor_id, "carla");
}

TEST(AuditLog, ClampsTheRequestedLimit) {
  MemoryAuditLog log;
  for (int i = 0; i < 10; ++i) {
    ASSERT_FALSE(log.append(entry("ana", "kick")).has_value());
  }

  EXPECT_EQ(log.list(3, {}).size(), 3U);
  // Zero asks for the default, and anything above the ceiling is capped rather
  // than refused: an unbounded query over a collection that only grows is how
  // a panel refresh becomes a full table scan.
  EXPECT_EQ(log.list(0, {}).size(), 10U);
  EXPECT_EQ(dv::server::store::clamp_audit_limit(0), MemoryAuditLog::kDefaultLimit);
  EXPECT_EQ(dv::server::store::clamp_audit_limit(10000), MemoryAuditLog::kMaxLimit);
}

TEST(AuditLog, DropsTheOldestOnceItIsFull) {
  MemoryAuditLog log;
  // The in-memory log is not the durable copy, and a process that runs for
  // months must not grow one entry at a time forever.
  for (std::size_t i = 0; i < MemoryAuditLog::kCapacity + 10; ++i) {
    ASSERT_FALSE(log.append(entry("ana", std::to_string(i))).has_value());
  }

  const auto entries = log.list(MemoryAuditLog::kMaxLimit, {});
  ASSERT_FALSE(entries.empty());
  EXPECT_EQ(entries.front().action, std::to_string(MemoryAuditLog::kCapacity + 9));
}

}  // namespace
