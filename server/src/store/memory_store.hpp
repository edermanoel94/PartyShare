#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "store/audit_log.hpp"
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
