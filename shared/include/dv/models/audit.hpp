#pragma once

#include <cstdint>
#include <string>

namespace dv::models {

/// One administrative action, as it is stored and as it crosses the wire.
///
/// The same type serves both on purpose. An audit entry exists to be read back
/// by a person, so a second representation between the database and the screen
/// would be two chances to drop a field and no gain.
///
/// What it deliberately does not carry: anything that would make the log worth
/// stealing. No password, no token, no session identifier.
struct AuditEntry {
  /// Assigned by the store.
  std::string id;
  /// Who acted. Both the identifier and the name are kept, because the
  /// account may be deleted later and a log that then reads "who: unknown" has
  /// lost the thing it was written for.
  std::string actor_id;
  std::string actor_username;
  /// What was done: "kick", "force_mute", "force_unmute", "restrict_user",
  /// "create_user", "update_user", "delete_user", "delete_room",
  /// "create_room".
  std::string action;
  /// Who or what it was done to. A user id, or a room id for room actions.
  std::string target_id;
  /// Where it happened, when the action belongs to a room.
  std::string room_id;
  /// Free text for whatever the action needs to be understandable later, for
  /// example the reason given for a kick or the role a user was moved to.
  std::string detail;
  /// Seconds since the Unix epoch, UTC. A wall clock rather than a steady one:
  /// this is read by a person, long after the process that wrote it died.
  std::int64_t timestamp_seconds = 0;

  friend bool operator==(const AuditEntry&, const AuditEntry&) = default;
};

}  // namespace dv::models
