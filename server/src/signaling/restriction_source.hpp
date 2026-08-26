#pragma once

#include <string>
#include <vector>

#include <dv/models/user.hpp>

#include "store/user_store.hpp"

namespace dv::server {

/// An account this server is currently enforcing restrictions for, together
/// with the set it believes is in force.
///
/// The belief is not something a source keeps. It is what the connection
/// authenticated with, which `Hub::enforce` already rewrites every time it
/// acts, and passing it in rather than caching it here is what makes a source
/// stateless: there is nothing to seed at login, nothing to evict at
/// disconnect, and no second copy that can drift from the one every handler
/// actually consults.
struct WatchedAccount {
  std::string user_id;
  models::Restrictions known;
};

/// One account's restrictions having moved without a message saying so.
struct RestrictionChange {
  std::string user_id;
  models::Restrictions before;
  models::Restrictions after;
};

/// What one pass found.
///
/// Two lists rather than one with a flag on it, because they are answers to
/// different questions and lead to different work: a restriction that moved is
/// enforced, and an account that is gone has its session ended. Folding the
/// second into the first would mean a `RestrictionChange` whose `after` names
/// the restrictions of an account that no longer has any, which reads as
/// "nothing is taken away" -- the exact opposite of what happened.
struct RestrictionPoll {
  /// Accounts whose restrictions are not what the caller believed.
  std::vector<RestrictionChange> changed;
  /// Accounts the store no longer holds at all.
  ///
  /// Only ever accounts somebody is connected as, because those are the only
  /// ones asked about. That is what makes this actionable rather than a
  /// curiosity: an account deleted while nobody was signed in as it needs
  /// nothing done about it, and one deleted mid-session is a person still in a
  /// room, still holding a token, still transmitting.
  std::vector<std::string> gone;
};

/// Where a restriction written outside this process is noticed.
///
/// Two programs write the accounts collection. This server does, through
/// `restrict_user`, and it enforces in the same breath because it is the one
/// being told. tools/dbadmin does too, editing the documents directly, on
/// purpose: the whole reason it exists is to reach the data when there is no
/// server running to ask. It cannot tell anybody, and this is how the Hub finds
/// out anyway.
///
/// An interface for the same reason `store::UserStore` is one. The
/// implementation below asks the store, which works on every deployment and on
/// the in-memory store the tests use. A MongoDB change stream would be a
/// second one, and deliberately is not this one: change streams require a
/// replica set or a sharded cluster, and every connection string in README.md,
/// INSTALL.md and tools/dbadmin/README.md names a standalone `mongod`, where
/// `watch` does not run at all. Adding that later is a new class here and
/// nothing else.
class RestrictionSource {
 public:
  RestrictionSource() = default;
  virtual ~RestrictionSource() = default;

  RestrictionSource(const RestrictionSource&) = delete;
  RestrictionSource& operator=(const RestrictionSource&) = delete;
  RestrictionSource(RestrictionSource&&) = delete;
  RestrictionSource& operator=(RestrictionSource&&) = delete;

  /// What has moved for the accounts in `watched` since the caller last looked.
  ///
  /// Only those accounts, and that is the whole of the cost argument. An
  /// account nobody is connected as has nothing to enforce: there is no
  /// microphone to take and no share to stop, and the restriction takes hold on
  /// its own the next time somebody logs in as it. Reading the whole collection
  /// to discover that would be the difference between a pass costing one lookup
  /// per participant and one costing the table.
  [[nodiscard]] virtual RestrictionPoll poll(const std::vector<WatchedAccount>& watched) = 0;
};

/// Notices a change by reading the accounts back and comparing.
///
/// The default, and the only one for now. It costs one `find_by_id` per
/// connected participant per pass, and the Hub runs a pass once per heartbeat
/// rather than once per hundred milliseconds, so on a five-person room that is
/// five lookups every five seconds. That is the same order as the lookup
/// `handle_chat` already does for every message anybody types.
class StoreRestrictionSource final : public RestrictionSource {
 public:
  /// The store is not owned and has to outlive this.
  explicit StoreRestrictionSource(store::UserStore& users) noexcept : users_(&users) {}

  [[nodiscard]] RestrictionPoll poll(const std::vector<WatchedAccount>& watched) override;

 private:
  store::UserStore* users_;
};

}  // namespace dv::server
