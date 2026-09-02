#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <dv/core/result.hpp>

namespace dv::server::store {

/// A room as the database knows it.
///
/// No participant list, on purpose. Who is in a room right now is state of the
/// running process: it changes several times a minute, it is meaningless once
/// the process dies, and writing it to a database would only produce a record
/// that is wrong the moment the server restarts. The RoomManager stays the
/// authority on that, and this is the part worth outliving a restart.
struct RoomRecord {
  std::string id;
  std::string name;
  std::string owner_id;
  bool persistent = false;
  /// Seconds since the Unix epoch, UTC. Stamped by the store when zero.
  std::int64_t created_at = 0;
  /// How many people the room holds. Zero for a record written before the
  /// field existed, which the RoomManager reads as
  /// `models::kDefaultRoomCapacity`: the number every room held back then.
  int capacity = 0;
};

/// Not thread safe. See UserStore.
class RoomStore {
 public:
  RoomStore() = default;
  virtual ~RoomStore() = default;

  RoomStore(const RoomStore&) = delete;
  RoomStore& operator=(const RoomStore&) = delete;
  RoomStore(RoomStore&&) = delete;
  RoomStore& operator=(RoomStore&&) = delete;

  /// Creates or replaces. Upsert rather than create, because the caller that
  /// matters is room creation, which already holds a fresh identifier and has
  /// nothing to gain from finding out it collided a second time.
  [[nodiscard]] virtual std::optional<Error> upsert(RoomRecord record) = 0;

  [[nodiscard]] virtual std::optional<RoomRecord> find(const std::string& room_id) const = 0;

  /// Every room, oldest first.
  [[nodiscard]] virtual std::vector<RoomRecord> list() const = 0;

  /// Fails with `room_not_found`.
  [[nodiscard]] virtual std::optional<Error> remove(const std::string& room_id) = 0;
};

}  // namespace dv::server::store
