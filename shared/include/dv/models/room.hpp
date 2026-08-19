#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include <dv/models/user.hpp>

namespace dv::models {

/// Rooms are plain data here. Capacity limits, identifier generation and the
/// join and leave policy belong to the server side RoomManager (M2), because
/// they depend on server configuration.
struct Room {
  std::string id;
  std::string name;
  std::vector<Participant> participants;

  [[nodiscard]] std::size_t size() const noexcept { return participants.size(); }

  [[nodiscard]] const Participant* find(const std::string& user_id) const noexcept;
  [[nodiscard]] Participant* find(const std::string& user_id) noexcept;
  [[nodiscard]] bool contains(const std::string& user_id) const noexcept;

  /// The single participant currently sharing their screen, if any.
  /// Section 5.1 of SPEC.md allows only one at a time.
  [[nodiscard]] const Participant* screen_sharer() const noexcept;

  friend bool operator==(const Room&, const Room&) = default;
};

/// Room identifiers are 6 uppercase hexadecimal characters, as in the
/// "8F42A1" example from section 5.1 of SPEC.md.
inline constexpr std::size_t kRoomIdLength = 6;

[[nodiscard]] bool is_valid_room_id(const std::string& id) noexcept;

}  // namespace dv::models
