#pragma once

#include <optional>
#include <string>
#include <vector>

#include <dv/core/result.hpp>
#include <dv/models/audit.hpp>

namespace dv::server::store {

/// The record of what administrators did.
///
/// `append` returns a failure rather than swallowing one, and the caller is
/// expected to log it loudly and carry on with the action anyway. The other
/// choice, refusing to remove a disruptive participant because the database is
/// unreachable, protects the log at the expense of the thing the log is about.
/// The trade is deliberate and is stated in docs/13-security.md.
///
/// Not thread safe. See UserStore.
class AuditLog {
 public:
  /// How many entries `list` returns when the caller asks for no particular
  /// number, and the ceiling it clamps any request to. A panel shows a page,
  /// not a history, and an unbounded query is a way to make the server read a
  /// collection that only ever grows.
  static constexpr int kDefaultLimit = 100;
  static constexpr int kMaxLimit = 500;

  AuditLog() = default;
  virtual ~AuditLog() = default;

  AuditLog(const AuditLog&) = delete;
  AuditLog& operator=(const AuditLog&) = delete;
  AuditLog(AuditLog&&) = delete;
  AuditLog& operator=(AuditLog&&) = delete;

  /// The store assigns `id`, and stamps `timestamp_seconds` when it is zero.
  [[nodiscard]] virtual std::optional<Error> append(models::AuditEntry entry) = 0;

  /// Newest first. An empty `actor_id` means every actor, and `limit` is
  /// clamped to the range above.
  [[nodiscard]] virtual std::vector<models::AuditEntry> list(int limit,
                                                             const std::string& actor_id) const = 0;
};

/// Clamps a requested limit into the range the log allows.
[[nodiscard]] constexpr int clamp_audit_limit(int requested) noexcept {
  if (requested <= 0) {
    return AuditLog::kDefaultLimit;
  }
  return requested > AuditLog::kMaxLimit ? AuditLog::kMaxLimit : requested;
}

}  // namespace dv::server::store
