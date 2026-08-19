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

Result<std::string> RoomManager::create_room(std::string name) {
  for (int attempt = 0; attempt < kMaxIdAttempts; ++attempt) {
    std::string id = generate_room_id();
    if (rooms_.contains(id)) {
      continue;
    }
    models::Room room;
    room.id = id;
    room.name = std::move(name);
    rooms_.emplace(id, std::move(room));
    return id;
  }
  return Result<std::string>::failure("room_id_exhausted", "could not find a free room identifier");
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

  if (room.participants.empty()) {
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
                                            bool muted) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }
  models::Participant* participant = it->second.find(user_id);
  if (participant == nullptr) {
    return error("not_in_room", user_id + " is not in " + room_id);
  }
  participant->muted = muted;
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

std::optional<std::string> RoomManager::room_of(const std::string& user_id) const {
  const auto it = user_to_room_.find(user_id);
  if (it == user_to_room_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace dv::server
