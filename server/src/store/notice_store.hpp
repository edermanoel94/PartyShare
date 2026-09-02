#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <dv/core/result.hpp>
#include <dv/models/notice.hpp>

namespace dv::server::store {

/// What administrators have told individual accounts, and which of those the
/// account has said it read.
///
/// A failure to write has to stop the notice, the way it stops a chat message
/// and unlike the way it is survived by the audit log. The reasoning is the
/// one ChatStore gives and it is stronger here: the store *is* the notice. An
/// unwritten one has no identifier, so nobody can acknowledge it, so an
/// administrator would be looking at a message that can never be marked as
/// read - and if the recipient was offline, at a message that was never sent
/// at all while the panel said it was.
///
/// Not thread safe. See UserStore.
class NoticeStore {
 public:
  /// How many outstanding notices one sign-in hands over.
  ///
  /// Nothing is dropped: the rest are still pending and arrive at the next
  /// sign-in, oldest first, as these are acknowledged. What the cap buys is
  /// that somebody who was away for a month, or who was the target of an
  /// administrator holding down a key, signs in to a screen they can get
  /// through rather than to a stack of boxes.
  static constexpr int kMaxPendingPerDelivery = 20;

  NoticeStore() = default;
  virtual ~NoticeStore() = default;

  NoticeStore(const NoticeStore&) = delete;
  NoticeStore& operator=(const NoticeStore&) = delete;
  NoticeStore(NoticeStore&&) = delete;
  NoticeStore& operator=(NoticeStore&&) = delete;

  /// Writes one and hands back what was written.
  ///
  /// The store assigns `id` and stamps `created_at` when it is zero, and the
  /// copy that comes back carries both. It returns the notice and not only a
  /// failure for the same reason ChatStore::append does: what is sent to the
  /// recipient has to be the row that exists, or the identifier they
  /// acknowledge is one nothing answers to.
  [[nodiscard]] virtual Result<models::Notice> append(models::Notice notice) = 0;

  /// The notices one account has not acknowledged, oldest first, at most
  /// `kMaxPendingPerDelivery` of them.
  ///
  /// Oldest first because that is the order they were said in, and somebody
  /// reading two messages from the same administrator out of order is reading
  /// a different pair of messages.
  [[nodiscard]] virtual std::vector<models::Notice> pending_for(
      const std::string& user_id) const = 0;

  /// Marks one as read, and hands back what it now is.
  ///
  /// `user_id` is not redundant with `notice_id`: it is the check. A notice is
  /// acknowledged by the account it was addressed to and by nobody else, and
  /// making that a lookup key rather than a comparison in a handler means the
  /// rule cannot be forgotten by the next caller. An identifier belonging to
  /// somebody else fails exactly as an identifier belonging to nobody does,
  /// with `notice_not_found`, so a client cannot learn that a notice exists by
  /// being refused differently.
  ///
  /// Acknowledging one that is already acknowledged is not a failure. A client
  /// that reconnected and was handed the same notice twice would otherwise be
  /// answered with an error for doing the only correct thing.
  [[nodiscard]] virtual Result<models::Notice> acknowledge(const std::string& notice_id,
                                                           const std::string& user_id) = 0;

  /// Forgets everything ever sent to one account, and reports nothing when
  /// there was nothing.
  ///
  /// Called when the account is deleted, and not optional housekeeping: these
  /// are messages written to a named person, and a record of what an
  /// administrator told somebody who no longer exists is a record with a
  /// subject and no owner. RoomManager clears a room's chat for the same
  /// reason.
  [[nodiscard]] virtual std::optional<Error> clear_for(const std::string& user_id) = 0;
};

}  // namespace dv::server::store
