#include "signaling/restriction_source.hpp"

namespace dv::server {

RestrictionPoll StoreRestrictionSource::poll(const std::vector<WatchedAccount>& watched) {
  RestrictionPoll found;
  for (const WatchedAccount& account : watched) {
    const auto stored = users_->find_by_id(account.user_id);
    if (!stored.has_value()) {
      // Deleted while somebody was signed in as it. The server's own
      // `delete_user` ends the session in the same breath as the removal; a
      // deletion written straight into the store cannot, so it is reported here
      // and the Hub does that half.
      found.gone.push_back(account.user_id);
      continue;
    }
    if (stored->user.restrictions == account.known) {
      continue;
    }
    found.changed.push_back(RestrictionChange{
        .user_id = account.user_id, .before = account.known, .after = stored->user.restrictions});
  }
  return found;
}

}  // namespace dv::server
