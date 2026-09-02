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

  const auto it =
      std::ranges::find_if(rooms_, [&](const RoomRecord& other) { return other.id == record.id; });
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

// --- chat --------------------------------------------------------------------

Result<models::ChatMessage> MemoryChatStore::append(models::ChatMessage message) {
  message.id = std::to_string(next_id_++);
  if (message.timestamp_seconds == 0) {
    message.timestamp_seconds = unix_seconds_now();
  }

  std::vector<models::ChatMessage>& room = rooms_[message.room_id];
  room.push_back(message);

  if (room.size() > kCapacityPerRoom) {
    room.erase(room.begin(),
               room.begin() + static_cast<std::ptrdiff_t>(room.size() - kCapacityPerRoom));
  }
  return message;
}

std::vector<models::ChatMessage> MemoryChatStore::list(const std::string& room_id,
                                                       int limit) const {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return {};
  }

  const std::vector<models::ChatMessage>& room = it->second;
  const auto wanted = static_cast<std::size_t>(clamp_chat_limit(limit));
  const std::size_t skipped = room.size() > wanted ? room.size() - wanted : 0;

  // A window from the end, still in order: the vector is oldest first, and so
  // is what the contract asks for, so the tail is the answer as it stands.
  return {room.begin() + static_cast<std::ptrdiff_t>(skipped), room.end()};
}

std::optional<Error> MemoryChatStore::clear(const std::string& room_id) {
  rooms_.erase(room_id);
  return std::nullopt;
}

// --- notices -----------------------------------------------------------------

Result<models::Notice> MemoryNoticeStore::append(models::Notice notice) {
  notice.id = std::to_string(next_id_++);
  if (notice.created_at == 0) {
    notice.created_at = unix_seconds_now();
  }
  notices_.push_back(notice);

  // Over capacity, the receipts go before the messages. Walked oldest first,
  // taking the acknowledged ones, and only then falling back to dropping
  // whatever is oldest: losing a notice somebody has read costs a record that
  // they read it, and losing one they have not costs the thing itself.
  std::size_t excess = notices_.size() > kCapacity ? notices_.size() - kCapacity : 0;
  for (auto it = notices_.begin(); excess > 0 && it != notices_.end();) {
    if (it->acknowledged()) {
      it = notices_.erase(it);
      --excess;
    } else {
      ++it;
    }
  }
  if (excess > 0) {
    notices_.erase(notices_.begin(), notices_.begin() + static_cast<std::ptrdiff_t>(excess));
  }
  return notice;
}

std::vector<models::Notice> MemoryNoticeStore::pending_for(const std::string& user_id) const {
  std::vector<models::Notice> pending;
  for (const models::Notice& notice : notices_) {
    if (notice.user_id != user_id || notice.acknowledged()) {
      continue;
    }
    pending.push_back(notice);
    if (pending.size() >= static_cast<std::size_t>(NoticeStore::kMaxPendingPerDelivery)) {
      break;
    }
  }
  return pending;
}

Result<models::Notice> MemoryNoticeStore::acknowledge(const std::string& notice_id,
                                                      const std::string& user_id) {
  for (models::Notice& notice : notices_) {
    if (notice.id != notice_id || notice.user_id != user_id) {
      continue;
    }
    // Already acknowledged is left where it was rather than restamped. The
    // first time somebody said they read it is the fact worth keeping.
    if (!notice.acknowledged()) {
      notice.acknowledged_at = unix_seconds_now();
    }
    return notice;
  }
  return Result<models::Notice>::failure(not_found("notice"));
}

std::optional<Error> MemoryNoticeStore::clear_for(const std::string& user_id) {
  const auto removed = std::ranges::remove_if(
      notices_, [&](const models::Notice& notice) { return notice.user_id == user_id; });
  notices_.erase(removed.begin(), removed.end());
  return std::nullopt;
}

// --- sessions ----------------------------------------------------------------

Result<SessionRecord> MemorySessionStore::open(SessionRecord record) {
  record.id = std::to_string(next_id_++);
  const std::int64_t now = unix_seconds_now();
  if (record.connected_at == 0) {
    record.connected_at = now;
  }
  if (record.last_seen_at == 0) {
    record.last_seen_at = record.connected_at;
  }
  sessions_.push_back(record);

  // Only the ended ones are ever dropped, oldest first. A store whose rows are
  // all open goes over its capacity and stays there, which is the right way to
  // be wrong: there is one open row per connection this process is holding, so
  // the number that can be there at once is bounded by the sockets and not by
  // how long the server has been up.
  std::size_t excess = sessions_.size() > kCapacity ? sessions_.size() - kCapacity : 0;
  for (auto it = sessions_.begin(); excess > 0 && it != sessions_.end();) {
    if (it->open()) {
      ++it;
    } else {
      it = sessions_.erase(it);
      --excess;
    }
  }
  return record;
}

std::optional<Error> MemorySessionStore::touch(const std::vector<std::string>& ids) {
  const std::int64_t now = unix_seconds_now();
  for (SessionRecord& session : sessions_) {
    if (!session.open()) {
      continue;
    }
    if (std::ranges::find(ids, session.id) != ids.end()) {
      session.last_seen_at = now;
    }
  }
  return std::nullopt;
}

std::optional<Error> MemorySessionStore::close(const std::string& id) {
  for (SessionRecord& session : sessions_) {
    if (session.id == id && session.open()) {
      session.ended_at = unix_seconds_now();
      break;
    }
  }
  return std::nullopt;
}

std::size_t MemorySessionStore::close_open() {
  // At startup, which is the only place the server calls this, the answer is
  // always zero: the store was created empty a moment ago by the process
  // asking, so a session left open by a previous run is not a state it can be
  // in. The Mongo implementation is where the recovery has anything to
  // recover. The loop is here anyway because the contract is about the rows,
  // not about when it happens to be called, and a method that only works
  // before anybody has used the store is a method with a footnote.
  std::size_t closed = 0;
  for (SessionRecord& session : sessions_) {
    if (session.open()) {
      session.ended_at = session.last_seen_at;
      ++closed;
    }
  }
  return closed;
}

std::vector<SessionRecord> MemorySessionStore::list_open() const {
  std::vector<SessionRecord> open;
  // Backwards, because the contract is newest first and the vector is oldest
  // first.
  for (auto it = sessions_.rbegin(); it != sessions_.rend(); ++it) {
    if (it->open()) {
      open.push_back(*it);
    }
  }
  return open;
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
