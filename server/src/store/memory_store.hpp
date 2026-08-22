#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "store/audit_log.hpp"
#include "store/chat_store.hpp"
#include "store/room_store.hpp"
#include "store/user_store.hpp"

namespace dv::server::store {

/// The stores a server without a database uses, and the ones every test that
/// is not about MongoDB uses.
///
/// They keep the behaviour the server had before persistence existed: accounts
/// registered at startup, rooms that live as long as the process, and an audit
/// log that is gone when it stops. That is why they are not a stub: with
/// `DV_ENABLE_MONGO=OFF` this is the product, not a stand-in.
///
/// A vector rather than a map, in all three: the collections are small by
/// construction, order is part of the contract, and a linear scan over a few
/// hundred accounts is not the thing that will ever be slow here.
class MemoryUserStore final : public UserStore {
 public:
  [[nodiscard]] std::optional<Error> create(Account account) override;
  [[nodiscard]] std::optional<Account> find_by_username(const std::string& username) const override;
  [[nodiscard]] std::optional<Account> find_by_id(const std::string& user_id) const override;
  [[nodiscard]] std::vector<Account> list() const override;
  [[nodiscard]] std::optional<Error> update(const Account& account) override;
  [[nodiscard]] std::optional<Error> remove(const std::string& user_id) override;
  [[nodiscard]] std::size_t count_with_role(models::Role role) const override;

 private:
  std::vector<Account> accounts_;
};

class MemoryRoomStore final : public RoomStore {
 public:
  [[nodiscard]] std::optional<Error> upsert(RoomRecord record) override;
  [[nodiscard]] std::optional<RoomRecord> find(const std::string& room_id) const override;
  [[nodiscard]] std::vector<RoomRecord> list() const override;
  [[nodiscard]] std::optional<Error> remove(const std::string& room_id) override;

 private:
  std::vector<RoomRecord> rooms_;
};

/// A map here, where the other three use a vector, because every operation is
/// scoped to one room: appending, reading the end, and forgetting a room whole.
/// A flat list would make each of those a scan over every other room's
/// conversation as well.
class MemoryChatStore final : public ChatStore {
 public:
  /// Per room, oldest dropped first. The reason the audit log has a cap
  /// applies twice over here: this is the product on a server without a
  /// database, and a persistent room that nobody ever closes is otherwise a
  /// process that grows for as long as people keep talking in it.
  ///
  /// Above kMaxLimit, so that asking for the largest window the protocol
  /// allows still returns messages rather than the whole of what is kept.
  static constexpr std::size_t kCapacityPerRoom = 1000;

  [[nodiscard]] Result<models::ChatMessage> append(models::ChatMessage message) override;
  [[nodiscard]] std::vector<models::ChatMessage> list(const std::string& room_id,
                                                      int limit) const override;
  [[nodiscard]] std::optional<Error> clear(const std::string& room_id) override;

 private:
  /// Oldest first within each room, so appending is a push_back and trimming
  /// is one erase at the front.
  std::unordered_map<std::string, std::vector<models::ChatMessage>> rooms_;
  std::uint64_t next_id_ = 1;
};

class MemoryAuditLog final : public AuditLog {
 public:
  /// Entries beyond this are dropped, oldest first. A log that only grows is
  /// how a long lived process runs out of memory, and this one is not the
  /// durable copy: that is the MongoDB implementation.
  static constexpr std::size_t kCapacity = 2000;

  [[nodiscard]] std::optional<Error> append(models::AuditEntry entry) override;
  [[nodiscard]] std::vector<models::AuditEntry> list(int limit,
                                                     const std::string& actor_id) const override;

 private:
  /// Oldest first, so appending is a push_back and trimming is a single erase
  /// at the front. `list` reverses what it returns.
  std::vector<models::AuditEntry> entries_;
  std::uint64_t next_id_ = 1;
};

/// Seconds since the Unix epoch, UTC, from the system clock.
[[nodiscard]] std::int64_t unix_seconds_now();

}  // namespace dv::server::store
