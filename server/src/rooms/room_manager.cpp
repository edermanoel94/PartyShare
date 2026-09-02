#include "rooms/room_manager.hpp"

#include <algorithm>
#include <string_view>
#include <utility>

#include <dv/logging/logger.hpp>

namespace dv::server {
namespace {

Error error(std::string code, std::string message) {
  return Error{.code = std::move(code), .message = std::move(message)};
}

/// What `models::user_label` is given for the username half of a name here.
///
/// `models::User` does not carry one. The username is a property of the account
/// an authenticated connection is bound to, and `Hub` is what holds that; this
/// class knows a room's participants and nothing about how they signed in. So a
/// name from here is the display name alone, which is what `user_label` returns
/// for an empty username, falling back to the identifier for a user who has no
/// display name either.
constexpr const char* kNoUsername = "";

constexpr std::string_view kHexDigits = "0123456789ABCDEF";

/// Enough attempts that exhausting them means the identifier space is
/// genuinely close to full, rather than that we were unlucky.
constexpr int kMaxIdAttempts = 64;

}  // namespace

RoomManager::RoomManager() : RoomManager(Options{}) {}

RoomManager::RoomManager(Options options)
    : options_(options),
      random_(options.id_seed.has_value() ? *options.id_seed : std::random_device{}()) {}

std::string RoomManager::generate_room_id() {
  std::uniform_int_distribution<std::size_t> digit(0, kHexDigits.size() - 1);
  std::string id(models::kRoomIdLength, '0');
  for (char& character : id) {
    character = kHexDigits[digit(random_)];
  }
  return id;
}

void RoomManager::forget(const std::string& room_id) const {
  if (options_.chat == nullptr) {
    return;
  }
  // Logged and carried on with, rather than turned into a refusal. The room is
  // already gone from the live map by the time this runs, so there is nothing
  // left to undo, and the alternative is a `leave` that fails after having
  // succeeded. What stays behind is a history whose room no longer exists, and
  // the log is where somebody finds out about it.
  if (auto failure = options_.chat->clear(room_id)) {
    DV_LOG_ERROR("The conversation of room {} was not cleared: {}", room_id, failure->message);
  }
}

bool RoomManager::name_taken(const std::string& key) const {
  if (key.empty()) {
    return false;
  }
  return std::ranges::any_of(
      rooms_, [&](const auto& entry) { return models::room_name_key(entry.second.name) == key; });
}

int RoomManager::default_capacity() const noexcept {
  return std::min(models::kDefaultRoomCapacity, max_capacity());
}

int RoomManager::max_capacity() const noexcept {
  // Never below the smallest room, whatever the configuration says: a ceiling
  // under the floor would leave no size a room could be created with, and the
  // configuration reader already refuses a value that low.
  return std::clamp(options_.max_participants_per_room, models::kMinRoomCapacity,
                    models::kMaxRoomCapacity);
}

Result<std::string> RoomManager::create_room(const std::string& name, std::string owner_id,
                                             int capacity) {
  // Checked before anything else is looked at, name included, so a request
  // that asks for a size nobody could be given is told that rather than told
  // its name is taken.
  if (capacity == 0) {
    capacity = default_capacity();
  } else if (!models::is_valid_room_capacity(capacity) || capacity > max_capacity()) {
    return Result<std::string>::failure(
        "invalid_value", "a room holds between " + std::to_string(models::kMinRoomCapacity) +
                             " and " + std::to_string(max_capacity()) + " people");
  }

  // Trimmed once, before the loop, because it does not depend on which
  // identifier the loop settles on.
  const std::string wanted = models::trim_room_name(name);

  // Refused rather than made unique by appending something. Somebody who asked
  // for "Daily" and got "Daily (2)" has a room they did not name, and the list
  // they were trying to be found in now has two rows that read almost alike.
  //
  // Only for a name that was asked for. An empty one has no collision to have
  // yet: what it becomes is decided inside the loop, against an identifier
  // that is not chosen until then.
  if (name_taken(models::room_name_key(wanted))) {
    return Result<std::string>::failure("room_name_taken",
                                        "another room is already called " + wanted);
  }

  for (int attempt = 0; attempt < kMaxIdAttempts; ++attempt) {
    std::string id = generate_room_id();
    if (rooms_.contains(id)) {
      continue;
    }
    // The store is consulted too, not only the live map. A room that nobody is
    // in right now still owns its identifier, and handing the same one out
    // twice would put two sets of people in what they both believe is their
    // own room.
    if (options_.store != nullptr && options_.store->find(id).has_value()) {
      continue;
    }
    // The identifier is free, and the name it would produce may still not be:
    // nothing stops somebody from naming their room "A26DCB" by hand, and the
    // generator is free to hand that identifier out afterwards. Another
    // identifier costs one more turn of this loop, where refusing would fail a
    // creation that asked for nothing in particular.
    if (wanted.empty() && name_taken(models::room_name_key(id))) {
      continue;
    }

    models::Room room;
    room.id = id;
    // A room nobody named is called by its own identifier, and that is written
    // down here rather than left for each screen to work out. The name reaches
    // the database, the room list, the administrator's tab and the title above
    // the call, and a fallback repeated in four places is four chances for one
    // of them to show an empty column.
    room.name = wanted.empty() ? id : wanted;
    room.owner_id = std::move(owner_id);
    room.persistent = true;
    room.capacity = capacity;

    if (options_.store != nullptr) {
      // Written before the room is live, and written for every room now. A
      // room that exists in memory but not in the database is one that
      // disappears at the next restart, and there is no longer a kind of room
      // that is allowed to.
      //
      // A failure here fails the creation instead of carrying on. The caller
      // asked for a room that survives, and one that exists only in this
      // process is not it: better to say so now than at the next restart.
      if (auto failure = options_.store->upsert(store::RoomRecord{.id = room.id,
                                                                  .name = room.name,
                                                                  .owner_id = room.owner_id,
                                                                  .persistent = true,
                                                                  .capacity = room.capacity})) {
        return Result<std::string>::failure(*failure);
      }
    }

    rooms_.emplace(id, std::move(room));
    return id;
  }
  return Result<std::string>::failure("room_id_exhausted", "could not find a free room identifier");
}

std::size_t RoomManager::load_rooms() {
  if (options_.store == nullptr) {
    return 0;
  }

  std::size_t loaded = 0;
  for (const store::RoomRecord& record : options_.store->list()) {
    // Everything in the store is loaded. The filter that used to be here
    // skipped records that were not persistent, and no such record was ever
    // written: only a persistent room reached the store at all.
    //
    // Names are not checked for duplicates on the way in, and that is the
    // point. A database written before names had to be unique holds whatever
    // it holds - every room an older client made is called "room" - and a
    // startup that refused those would delete rooms people are still using to
    // enforce a rule that did not exist when they were made. The rule applies
    // to creating a room, which is the moment somebody can be told to pick
    // something else.
    if (rooms_.contains(record.id)) {
      continue;
    }
    models::Room room;
    room.id = record.id;
    room.name = record.name;
    room.owner_id = record.owner_id;
    room.persistent = true;
    // A record from before rooms had a size carries zero, and gets the number
    // every room held then. Not checked against this server's ceiling for the
    // same reason names are not: the record is what it is, and refusing it
    // would delete a room somebody is using.
    room.capacity = record.capacity > 0 ? record.capacity : models::kDefaultRoomCapacity;
    rooms_.emplace(record.id, std::move(room));
    ++loaded;
  }
  return loaded;
}

Result<std::vector<std::string>> RoomManager::remove_room(const std::string& room_id) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    // Still worth removing from the store: a persistent room with nobody in it
    // is not in the live map, and closing it has to work all the same.
    if (options_.store != nullptr && options_.store->find(room_id).has_value()) {
      if (auto failure = options_.store->remove(room_id)) {
        return Result<std::vector<std::string>>::failure(*failure);
      }
      forget(room_id);
      return std::vector<std::string>{};
    }
    return Result<std::vector<std::string>>::failure("room_not_found",
                                                     "no room with id " + room_id);
  }

  std::vector<std::string> removed;
  removed.reserve(it->second.participants.size());
  for (const models::Participant& participant : it->second.participants) {
    removed.push_back(participant.user.id);
    user_to_room_.erase(participant.user.id);
  }
  rooms_.erase(it);

  if (options_.store != nullptr) {
    // Ignored on purpose. Every room is written now, so this should find one,
    // but a store that cannot forget a room the live map has already dropped
    // is not a failure worth reporting to whoever closed it: the room is gone
    // either way, and what is left is a record the next startup will reload.
    (void)options_.store->remove(room_id);
  }
  forget(room_id);
  return removed;
}

std::optional<Error> RoomManager::join(const std::string& room_id, models::User user) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }

  models::Room& room = it->second;
  if (room.contains(user.id)) {
    return error("already_in_room", models::user_label(user.id, user.display_name, kNoUsername) +
                                        " is already in " + models::room_label(room_id, room.name));
  }
  // The room's own size, not the server's ceiling: two rooms on one server can
  // hold different numbers of people, and the number that matters is the one
  // whoever made this room chose.
  if (room.is_full()) {
    return error("room_full", "room " + models::room_label(room_id, room.name) + " is full (" +
                                  std::to_string(room.capacity) + " people)");
  }

  // A user can only be in one room at a time. Joining a second one leaves the
  // first, so the two views can never disagree.
  if (const auto previous = room_of(user.id)) {
    (void)leave(*previous, user.id);
  }

  user_to_room_[user.id] = room_id;
  room.participants.push_back(
      models::Participant{.user = std::move(user), .muted = false, .sharing_screen = false});
  return std::nullopt;
}

// Every message below that names a person or a room does it through
// `models::user_label` and `models::room_label`, because these messages are not
// only logged: `Hub` relays them to the client, which puts them in a dialog. A
// person who joins a room twice used to be told
// "f31d4c2809d51d780fdcc5e49d78340f is already in 332368".
//
// `not_in_room` is the exception, and stays an identifier on the user's side.
// Somebody who is not a participant has no name to read here: this class knows
// the people in its rooms and nobody else, and inventing a lookup so an error
// path can print a nicer word would be the wrong trade. The room half of that
// same message does resolve, because the room is in hand.
std::optional<Error> RoomManager::leave(const std::string& room_id, const std::string& user_id) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }

  models::Room& room = it->second;
  const auto participant = std::ranges::find_if(
      room.participants,
      [&](const models::Participant& candidate) { return candidate.user.id == user_id; });
  if (participant == room.participants.end()) {
    return error("not_in_room",
                 user_id + " is not in " + models::room_label(room_id, it->second.name));
  }

  room.participants.erase(participant);
  user_to_room_.erase(user_id);

  // The room stays, empty, and keeps what was said in it. It used to be erased
  // here the moment the last participant walked out, which meant stepping out
  // of your own room destroyed it: the identifier stopped working, and anybody
  // still holding a list watched a room they could see refuse to let them in.
  //
  // Ending a room is now something somebody does on purpose. See remove_room.
  return std::nullopt;
}

std::optional<std::string> RoomManager::remove_from_any_room(const std::string& user_id) {
  auto room_id = room_of(user_id);
  if (!room_id.has_value()) {
    return std::nullopt;
  }
  (void)leave(*room_id, user_id);
  return room_id;
}

std::optional<Error> RoomManager::set_muted(const std::string& room_id, const std::string& user_id,
                                            bool muted, bool by_admin) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }
  models::Participant* participant = it->second.find(user_id);
  if (participant == nullptr) {
    return error("not_in_room",
                 user_id + " is not in " + models::room_label(room_id, it->second.name));
  }

  // A participant cannot take off a mute an administrator put on. Muting
  // themselves further is still allowed: it takes nothing away from anyone.
  if (!by_admin && !muted && participant->muted_by_admin) {
    return error("forbidden", "an administrator muted " +
                                  models::user_label(participant->user.id,
                                                     participant->user.display_name, kNoUsername));
  }

  participant->muted = muted;
  if (by_admin) {
    // Set by a forced mute and cleared by a forced unmute, which is what makes
    // the administrator the only one who can release it.
    participant->muted_by_admin = muted;
  }
  return std::nullopt;
}

std::optional<Error> RoomManager::start_screen_share(const std::string& room_id,
                                                     const std::string& user_id, bool with_audio) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }
  models::Room& room = it->second;
  models::Participant* participant = room.find(user_id);
  if (participant == nullptr) {
    return error("not_in_room",
                 user_id + " is not in " + models::room_label(room_id, it->second.name));
  }

  if (const models::Participant* current = room.screen_sharer();
      current != nullptr && current->user.id != user_id) {
    return error("screen_share_busy",
                 models::user_label(current->user.id, current->user.display_name, kNoUsername) +
                     " is already sharing in " + models::room_label(room_id, room.name));
  }

  participant->sharing_screen = true;
  participant->sharing_audio = with_audio;
  return std::nullopt;
}

std::optional<Error> RoomManager::stop_screen_share(const std::string& room_id,
                                                    const std::string& user_id) {
  const auto it = rooms_.find(room_id);
  if (it == rooms_.end()) {
    return error("room_not_found", "no room with id " + room_id);
  }
  models::Participant* participant = it->second.find(user_id);
  if (participant == nullptr) {
    return error("not_in_room",
                 user_id + " is not in " + models::room_label(room_id, it->second.name));
  }
  participant->sharing_screen = false;
  participant->sharing_audio = false;
  return std::nullopt;
}

const models::Room* RoomManager::find(const std::string& room_id) const {
  const auto it = rooms_.find(room_id);
  return it == rooms_.end() ? nullptr : &it->second;
}

std::vector<models::Room> RoomManager::list() const {
  std::vector<models::Room> result;
  result.reserve(rooms_.size());
  for (const auto& [id, room] : rooms_) {
    result.push_back(room);
  }
  return result;
}

std::optional<std::string> RoomManager::room_owned_by(const std::string& user_id) const {
  if (user_id.empty()) {
    return std::nullopt;
  }
  for (const auto& [id, room] : rooms_) {
    if (room.owner_id == user_id) {
      return id;
    }
  }
  return std::nullopt;
}

std::optional<std::string> RoomManager::room_of(const std::string& user_id) const {
  const auto it = user_to_room_.find(user_id);
  if (it == user_to_room_.end()) {
    return std::nullopt;
  }
  return it->second;
}

}  // namespace dv::server
