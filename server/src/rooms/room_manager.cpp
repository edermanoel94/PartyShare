#include "rooms/room_manager.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

namespace dv::server {
namespace {

Error error(std::string code, std::string message) {
  return Error{.code = std::move(code), .message = std::move(message)};
}

constexpr std::string_view kHexDigits = "0123456789ABCDEF";

/// Enough attempts that exhausting them means the identifier space is
/// genuinely close to full, rather than that we were unlucky.
constexpr int kMaxIdAttempts = 64;

}  // namespace

RoomManager::RoomManager() : RoomManager(Options{}) {}

RoomManager::RoomManager(Options options)
    : options_(options),
      random_(options.id_seed.has_value() ? *options.id_seed : std::random_device{}()) {}

std::string RoomManager::generate_room_id() {
  std::uniform_int_distribution<std::size_t> digit(0, kHexDigits.size() - 1);
  std::string id(models::kRoomIdLength, '0');
  for (char& character : id) {
    character = kHexDigits[digit(random_)];
  }
  return id;
}

Result<std::string> RoomManager::create_room(std::string name, std::string owner_id,
                                             bool persistent) {
  for (int attempt = 0; attempt < kMaxIdAttempts; ++attempt) {
    std::string id = generate_room_id();
    if (rooms_.contains(id)) {
      continue;
    }
    // The store is consulted too, not only the live map. A persistent room
    // that nobody is in right now still owns its identifier, and handing the
    // same one out twice would put two sets of people in what they both
    // believe is their own room.
    if (options_.store != nullptr && options_.store->find(id).has_value()) {
      continue;
    }

    models::Room room;
    room.id = id;
    room.name = std::move(name);
    room.owner_id = std::move(owner_id);
    room.persistent = persistent;

    if (persistent && options_.store != nullptr) {
      // Written before the room is live. A room that exists in memory but not
      // in the database is one that disappears at the next restart, which is
      // the one promise a persistent room makes.
      if (auto failure = options_.store->upsert(store::RoomRecord{
              .id = room.id, .name = room.name, .owner_id = room.owner_id, .persistent = true})) {
        return Result<std::string>::failure(*failure);
      }
    }

    rooms_.emplace(id, std::move(room));
    return id;
  }
  return Result<std::string>::failure("room_id_exhausted", "could not find a free room identifier");
}

std::size_t RoomManager::load_persistent() {
  if (options_.store == nullptr) {
    return 0;
  }

  std::size_t loaded = 0;
  for (const store::RoomRecord& record : options_.store->list()) {
    if (!record.persistent || rooms_.contains(record.id)) {
      continue;
    }
    models::Room room;
    room.id = record.id;
    room.name = record.name;
    room.owner_id = record.owner_id;
    room.persistent = true;
    rooms_.emplace(record.id, std::move(room));
    ++loaded;
  }
  return loaded;
}

Result<std::vector<std::string>> RoomManager::remove_room(const std::string& room_id) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    // Still worth removing from the store: a persistent room with nobody in it
    // is not in the live map, and closing it has to work all the same.
    if (options_.store != nullptr && options_.store->find(room_id).has_value()) {
      if (auto failure = options_.store->remove(room_id)) {
        return Result<std::vector<std::string>>::failure(*failure);
      }
      return std::vector<std::string>{};
    }
    return Result<std::vector<std::string>>::failure("room_not_found",
                                                     "no room with id " + room_id);
  }

  std::vector<std::string> removed;
  removed.reserve(it->second.participants.size());
  for (const models::Participant& participant : it->second.participants) {
    removed.push_back(participant.user.id);
    user_to_room_.erase(participant.user.id);
  }
  rooms_.erase(it);

  if (options_.store != nullptr) {
    // Ignored on purpose when the room was never persistent: removing what was
    // not written is not a failure worth reporting to whoever closed the room.
    (void)options_.store->remove(room_id);
  }
  return removed;
}

std::optional<Error> RoomManager::join(const std::string& room_id, models::User user) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }

  models::Room& room = it->second;
  if (room.contains(user.id)) {
    return error("already_in_room", user.id + " is already in " + room_id);
  }
  if (std::cmp_greater_equal(room.size(), options_.max_participants_per_room)) {
    return error("room_full", "room " + room_id + " is full");
  }

  // A user can only be in one room at a time. Joining a second one leaves the
  // first, so the two views can never disagree.
  if (const auto previous = room_of(user.id)) {
    (void)leave(*previous, user.id);
  }

  user_to_room_[user.id] = room_id;
  room.participants.push_back(
      models::Participant{.user = std::move(user), .muted = false, .sharing_screen = false});
  return std::nullopt;
}

std::optional<Error> RoomManager::leave(const std::string& room_id, const std::string& user_id) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }

  models::Room& room = it->second;
  const auto participant = std::ranges::find_if(
      room.participants,
      [&](const models::Participant& candidate) { return candidate.user.id == user_id; });
  if (participant == room.participants.end()) {
    return error("not_in_room", user_id + " is not in " + room_id);
  }

  room.participants.erase(participant);
  user_to_room_.erase(user_id);

  // A persistent room stays, empty. That is the whole difference between the
  // two kinds, and the reason an identifier is worth writing down.
  if (room.participants.empty() && !room.persistent) {
    rooms_.erase(it);
  }
  return std::nullopt;
}

std::optional<std::string> RoomManager::remove_from_any_room(const std::string& user_id) {
  auto room_id = room_of(user_id);
  if (!room_id.has_value()) {
    return std::nullopt;
  }
  (void)leave(*room_id, user_id);
  return room_id;
}

std::optional<Error> RoomManager::set_muted(const std::string& room_id, const std::string& user_id,
                                            bool muted, bool by_admin) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }
  models::Participant* participant = it->second.find(user_id);
  if (participant == nullptr) {
    return error("not_in_room", user_id + " is not in " + room_id);
  }

  // A participant cannot take off a mute an administrator put on. Muting
  // themselves further is still allowed: it takes nothing away from anyone.
  if (!by_admin && !muted && participant->muted_by_admin) {
    return error("forbidden", "an administrator muted " + user_id);
  }

  participant->muted = muted;
  if (by_admin) {
    // Set by a forced mute and cleared by a forced unmute, which is what makes
    // the administrator the only one who can release it.
    participant->muted_by_admin = muted;
  }
  return std::nullopt;
}

std::optional<Error> RoomManager::start_screen_share(const std::string& room_id,
                                                     const std::string& user_id) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }
  models::Room& room = it->second;
  models::Participant* participant = room.find(user_id);
  if (participant == nullptr) {
    return error("not_in_room", user_id + " is not in " + room_id);
  }

  if (const models::Participant* current = room.screen_sharer();
      current != nullptr && current->user.id != user_id) {
    return error("screen_share_busy", current->user.id + " is already sharing in " + room_id);
  }

  participant->sharing_screen = true;
  return std::nullopt;
}

std::optional<Error> RoomManager::stop_screen_share(const std::string& room_id,
                                                    const std::string& user_id) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }
  models::Participant* participant = it->second.find(user_id);
  if (participant == nullptr) {
    return error("not_in_room", user_id + " is not in " + room_id);
  }
  participant->sharing_screen = false;
  return std::nullopt;
}

const models::Room* RoomManager::find(const std::string& room_id) const {
  const auto it = rooms_.find(room_id);
  return it == rooms_.end() ? nullptr : &it->second;
}

std::vector<models::Room> RoomManager::list() const {
  std::vector<models::Room> result;
  result.reserve(rooms_.size());
  for (const auto& [id, room] : rooms_) {
    result.push_back(room);
  }
  return result;
}

std::optional<std::string> RoomManager::room_of(const std::string& user_id) const {
  const auto it = user_to_room_.find(user_id);
  if (it == user_to_room_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace dv::server
