#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include <dv/core/result.hpp>
#include <dv/models/room.hpp>
#include <dv/models/user.hpp>

namespace dv::server {

/// Owns the set of live rooms and enforces the rules from section 5.1 of
/// SPEC.md: a capacity limit, and at most one participant sharing their screen.
///
/// This class knows nothing about sockets or about the wire protocol. It is
/// pure state plus rules, which is what makes it directly testable.
///
/// It is not thread safe on its own. The Hub owns the only instance and
/// serializes access to it.
class RoomManager {
 public:
  struct Options {
    int max_participants_per_room = 5;
    /// Fixing the seed makes room identifiers reproducible in tests.
    std::optional<std::uint32_t> id_seed;
  };

  /// Defined out of line: the default member initializers of Options are
  /// only usable once the enclosing class is complete.
  RoomManager();
  explicit RoomManager(Options options);

  /// Creates a room with a fresh identifier. Fails only if no free identifier
  /// could be found, which needs the identifier space to be nearly exhausted.
  [[nodiscard]] Result<std::string> create_room(std::string name);

  /// Adds a participant. Fails with room_not_found, room_full or
  /// already_in_room.
  [[nodiscard]] std::optional<Error> join(const std::string& room_id, models::User user);

  /// Removes a participant, and the room itself once it becomes empty.
  [[nodiscard]] std::optional<Error> leave(const std::string& room_id, const std::string& user_id);

  /// Removes a participant from whatever room they are in. Used when a
  /// connection drops and the client never sent leave_room.
  /// Returns the room the user was removed from, if any.
  [[nodiscard]] std::optional<std::string> remove_from_any_room(const std::string& user_id);

  [[nodiscard]] std::optional<Error> set_muted(const std::string& room_id,
                                               const std::string& user_id, bool muted);

  /// Fails with screen_share_busy when someone else is already sharing.
  /// Starting a share the user already owns succeeds and changes nothing.
  [[nodiscard]] std::optional<Error> start_screen_share(const std::string& room_id,
                                                        const std::string& user_id);

  [[nodiscard]] std::optional<Error> stop_screen_share(const std::string& room_id,
                                                       const std::string& user_id);

  [[nodiscard]] const models::Room* find(const std::string& room_id) const;

  [[nodiscard]] std::optional<std::string> room_of(const std::string& user_id) const;

  [[nodiscard]] std::size_t room_count() const noexcept { return rooms_.size(); }

  [[nodiscard]] int max_participants_per_room() const noexcept {
    return options_.max_participants_per_room;
  }

 private:
  [[nodiscard]] std::string generate_room_id();

  Options options_;
  std::mt19937 random_;
  std::unordered_map<std::string, models::Room> rooms_;
  /// user id to room id, so a dropped connection does not have to scan every
  /// room to be cleaned up.
  std::unordered_map<std::string, std::string> user_to_room_;
};

}  // namespace dv::server
