#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>

#include <dv/models/room.hpp>

namespace dv::models {
namespace {

/// Whitespace, checked without <cctype> so that a byte above 127 cannot be
/// misread as one. A room name may be text in any language, and none of the
/// bytes below is ever a UTF-8 continuation byte, which is what makes trimming
/// byte by byte safe on a name that is not ASCII.
///
/// The same four lines live in chat.cpp, deliberately: one shared helper would
/// have to sit in a header both models include, and a spelling of "whitespace"
/// is not worth a new dependency between them.
[[nodiscard]] bool is_space(char character) noexcept {
  return character == ' ' || character == '\t' || character == '\n' || character == '\r' ||
         character == '\v' || character == '\f';
}

}  // namespace

const Participant* Room::find(const std::string& user_id) const noexcept {
  const auto it = std::ranges::find_if(participants,
                                       [&](const Participant& p) { return p.user.id == user_id; });
  return it == participants.end() ? nullptr : &*it;
}

Participant* Room::find(const std::string& user_id) noexcept {
  const auto it = std::ranges::find_if(participants,
                                       [&](const Participant& p) { return p.user.id == user_id; });
  return it == participants.end() ? nullptr : &*it;
}

bool Room::contains(const std::string& user_id) const noexcept {
  return find(user_id) != nullptr;
}

const Participant* Room::screen_sharer() const noexcept {
  const auto it =
      std::ranges::find_if(participants, [](const Participant& p) { return p.sharing_screen; });
  return it == participants.end() ? nullptr : &*it;
}

bool is_valid_room_id(const std::string& id) noexcept {
  if (id.size() != kRoomIdLength) {
    return false;
  }
  return std::ranges::all_of(
      id, [](unsigned char c) { return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'); });
}

std::string trim_room_name(const std::string& name) {
  std::size_t begin = 0;
  while (begin < name.size() && is_space(name[begin])) {
    ++begin;
  }
  std::size_t end = name.size();
  while (end > begin && is_space(name[end - 1])) {
    --end;
  }
  return name.substr(begin, end - begin);
}

bool is_valid_room_name(const std::string& name) {
  // Measured against the trimmed name, because that is what gets stored. A
  // name accepted at its full length and then stored trimmed would be one
  // whose limit depends on how much whitespace it was padded with.
  const std::string trimmed = trim_room_name(name);
  if (trimmed.size() > kMaxRoomNameBytes) {
    return false;
  }
  // No control characters, and this is not tidiness. The client flattens each
  // room into one tab separated row before it reaches the table, so a name
  // carrying a tab or a newline would arrive as extra columns in everybody
  // else's list. Refused at the door rather than escaped at every screen that
  // shows a room.
  return std::ranges::none_of(
      trimmed, [](unsigned char character) { return character < 0x20 || character == 0x7F; });
}

std::string room_name_key(const std::string& name) {
  std::string key = trim_room_name(name);
  for (char& character : key) {
    if (character >= 'A' && character <= 'Z') {
      character = static_cast<char>(character - 'A' + 'a');
    }
  }
  return key;
}

}  // namespace dv::models
