#include "store/memory_store.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace dv::server::store {
namespace {

Error not_found(std::string what) {
  return Error{.code = std::move(what) + "_not_found", .message = "no such record"};
}

}  // namespace

std::int64_t unix_seconds_now() {
  return std::chrono::duration_cast<std::chrono::seconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// --- users -------------------------------------------------------------------

std::optional<Error> MemoryUserStore::create(Account account) {
  const bool taken = std::ranges::any_of(
      accounts_, [&](const Account& other) { return other.username == account.username; });
  if (taken) {
    return Error{.code = "user_exists", .message = "username is already taken"};
  }
  if (account.created_at == 0) {
    account.created_at = unix_seconds_now();
  }
  accounts_.push_back(std::move(account));
  return std::nullopt;
}

std::optional<Account> MemoryUserStore::find_by_username(const std::string& username) const {
  const auto it = std::ranges::find_if(
      accounts_, [&](const Account& account) { return account.username == username; });
  return it == accounts_.end() ? std::nullopt : std::optional<Account>(*it);
}

std::optional<Account> MemoryUserStore::find_by_id(const std::string& user_id) const {
  const auto it = std::ranges::find_if(
      accounts_, [&](const Account& account) { return account.user.id == user_id; });
  return it == accounts_.end() ? std::nullopt : std::optional<Account>(*it);
}

std::vector<Account> MemoryUserStore::list() const {
  return accounts_;
}

std::optional<Error> MemoryUserStore::update(const Account& account) {
  const auto it = std::ranges::find_if(
      accounts_, [&](const Account& other) { return other.user.id == account.user.id; });
  if (it == accounts_.end()) {
    return not_found("user");
  }

  // The identifier and the creation time are the account's history and are not
  // the caller's to rewrite. Everything else is replaced.
  const std::int64_t created_at = it->created_at;
  *it = account;
  it->created_at = created_at;
  return std::nullopt;
}

std::optional<Error> MemoryUserStore::remove(const std::string& user_id) {
  const auto removed = std::ranges::remove_if(
      accounts_, [&](const Account& account) { return account.user.id == user_id; });
  if (removed.empty()) {
    return not_found("user");
  }
  accounts_.erase(removed.begin(), removed.end());
  return std::nullopt;
}

std::size_t MemoryUserStore::count_with_role(models::Role role) const {
  return static_cast<std::size_t>(std::ranges::count_if(
      accounts_, [&](const Account& account) { return account.user.role == role; }));
}

// --- rooms -------------------------------------------------------------------

std::optional<Error> MemoryRoomStore::upsert(RoomRecord record) {
  if (record.created_at == 0) {
    record.created_at = unix_seconds_now();
  }

  const auto it = std::ranges::find_if(
      rooms_, [&](const RoomRecord& other) { return other.id == record.id; });
  if (it == rooms_.end()) {
    rooms_.push_back(std::move(record));
    return std::nullopt;
  }

  // Replacing keeps the original creation time, for the same reason an
  // account's does: it records when the room came into existence, not when it
  // was last written.
  const std::int64_t created_at = it->created_at;
  *it = std::move(record);
  it->created_at = created_at;
  return std::nullopt;
}

std::optional<RoomRecord> MemoryRoomStore::find(const std::string& room_id) const {
  const auto it =
      std::ranges::find_if(rooms_, [&](const RoomRecord& room) { return room.id == room_id; });
  return it == rooms_.end() ? std::nullopt : std::optional<RoomRecord>(*it);
}

std::vector<RoomRecord> MemoryRoomStore::list() const {
  return rooms_;
}

std::optional<Error> MemoryRoomStore::remove(const std::string& room_id) {
  const auto removed =
      std::ranges::remove_if(rooms_, [&](const RoomRecord& room) { return room.id == room_id; });
  if (removed.empty()) {
    return not_found("room");
  }
  rooms_.erase(removed.begin(), removed.end());
  return std::nullopt;
}

// --- audit -------------------------------------------------------------------

std::optional<Error> MemoryAuditLog::append(models::AuditEntry entry) {
  entry.id = std::to_string(next_id_++);
  if (entry.timestamp_seconds == 0) {
    entry.timestamp_seconds = unix_seconds_now();
  }
  entries_.push_back(std::move(entry));

  if (entries_.size() > kCapacity) {
    entries_.erase(entries_.begin(),
                   entries_.begin() + static_cast<std::ptrdiff_t>(entries_.size() - kCapacity));
  }
  return std::nullopt;
}

std::vector<models::AuditEntry> MemoryAuditLog::list(int limit, const std::string& actor_id) const {
  const auto wanted = static_cast<std::size_t>(clamp_audit_limit(limit));

  std::vector<models::AuditEntry> result;
  result.reserve(std::min(wanted, entries_.size()));

  // Backwards, because the newest entries are the ones asked for and walking
  // from the end means the filter never touches the history it would discard.
  for (auto it = entries_.rbegin(); it != entries_.rend() && result.size() < wanted; ++it) {
    if (actor_id.empty() || it->actor_id == actor_id) {
      result.push_back(*it);
    }
  }
  return result;
}

}  // namespace dv::server::store
