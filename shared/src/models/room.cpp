#include <algorithm>
#include <cctype>

#include <dv/models/room.hpp>

namespace dv::models {

const Participant* Room::find(const std::string& user_id) const noexcept {
  const auto it = std::find_if(participants.begin(), participants.end(),
                               [&](const Participant& p) { return p.user.id == user_id; });
  return it == participants.end() ? nullptr : &*it;
}

Participant* Room::find(const std::string& user_id) noexcept {
  const auto it = std::find_if(participants.begin(), participants.end(),
                               [&](const Participant& p) { return p.user.id == user_id; });
  return it == participants.end() ? nullptr : &*it;
}

bool Room::contains(const std::string& user_id) const noexcept {
  return find(user_id) != nullptr;
}

const Participant* Room::screen_sharer() const noexcept {
  const auto it = std::find_if(participants.begin(), participants.end(),
                               [](const Participant& p) { return p.sharing_screen; });
  return it == participants.end() ? nullptr : &*it;
}

bool is_valid_room_id(const std::string& id) noexcept {
  if (id.size() != kRoomIdLength) {
    return false;
  }
  return std::all_of(id.begin(), id.end(), [](unsigned char c) {
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F');
  });
}

}  // namespace dv::models
