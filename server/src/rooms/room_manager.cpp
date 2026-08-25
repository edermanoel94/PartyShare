#include "rooms/room_manager.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include <dv/logging/logger.hpp>

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

void RoomManager::forget(const std::string& room_id) const {
  if (options_.chat == nullptr) {
    return;
  }
  // Logged and carried on with, rather than turned into a refusal. The room is
  // already gone from the live map by the time this runs, so there is nothing
  // left to undo, and the alternative is a `leave` that fails after having
  // succeeded. What stays behind is a history whose room no longer exists, and
  // the log is where somebody finds out about it.
  if (auto failure = options_.chat->clear(room_id)) {
    DV_LOG_ERROR("The conversation of room {} was not cleared: {}", room_id, failure->message);
  }
}

Result<std::string> RoomManager::create_room(std::string name, std::string owner_id) {
  for (int attempt = 0; attempt < kMaxIdAttempts; ++attempt) {
    std::string id = generate_room_id();
    if (rooms_.contains(id)) {
      continue;
    }
    // The store is consulted too, not only the live map. A room that nobody is
    // in right now still owns its identifier, and handing the same one out
    // twice would put two sets of people in what they both believe is their
    // own room.
    if (options_.store != nullptr && options_.store->find(id).has_value()) {
      continue;
    }

    models::Room room;
    room.id = id;
    room.name = std::move(name);
    room.owner_id = std::move(owner_id);
    room.persistent = true;

    if (options_.store != nullptr) {
      // Written before the room is live, and written for every room now. A
      // room that exists in memory but not in the database is one that
      // disappears at the next restart, and there is no longer a kind of room
      // that is allowed to.
      //
      // A failure here fails the creation instead of carrying on. The caller
      // asked for a room that survives, and one that exists only in this
      // process is not it: better to say so now than at the next restart.
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

std::size_t RoomManager::load_rooms() {
  if (options_.store == nullptr) {
    return 0;
  }

  std::size_t loaded = 0;
  for (const store::RoomRecord& record : options_.store->list()) {
    // Everything in the store is loaded. The filter that used to be here
    // skipped records that were not persistent, and no such record was ever
    // written: only a persistent room reached the store at all.
    if (rooms_.contains(record.id)) {
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
      forget(room_id);
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
    // Ignored on purpose. Every room is written now, so this should find one,
    // but a store that cannot forget a room the live map has already dropped
    // is not a failure worth reporting to whoever closed it: the room is gone
    // either way, and what is left is a record the next startup will reload.
    (void)options_.store->remove(room_id);
  }
  forget(room_id);
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

  // The room stays, empty, and keeps what was said in it. It used to be erased
  // here the moment the last participant walked out, which meant stepping out
  // of your own room destroyed it: the identifier stopped working, and anybody
  // still holding a list watched a room they could see refuse to let them in.
  //
  // Ending a room is now something somebody does on purpose. See remove_room.
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
                                                     const std::string& user_id, bool with_audio) {
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
  participant->sharing_audio = with_audio;
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
  participant->sharing_audio = false;
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
