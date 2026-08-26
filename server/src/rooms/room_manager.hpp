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

#include "store/chat_store.hpp"
#include "store/room_store.hpp"

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
    /// Where persistent rooms are written. Null means rooms live and die with
    /// the process, which is what the server did before there was a database
    /// and what it still does without one.
    store::RoomStore* store = nullptr;
    /// Where what was said in each room is written.
    ///
    /// Here rather than only in the Hub, which is the one that writes the
    /// messages, because this class is what decides that a room has stopped
    /// existing. A history outlives its room only until somebody is handed
    /// that six character identifier again, and then it is a stranger's
    /// conversation on their screen. Keeping the two lifetimes in one place is
    /// what makes forgetting impossible to forget.
    store::ChatStore* chat = nullptr;
  };

  /// Defined out of line: the default member initializers of Options are
  /// only usable once the enclosing class is complete.
  RoomManager();
  explicit RoomManager(Options options);

  /// Creates a room with a fresh identifier. Fails only if no free identifier
  /// could be found, which needs the identifier space to be nearly exhausted.
  ///
  /// Every room is written to the store and outlives its last participant.
  /// There used to be two kinds, and the ordinary one evaporated the moment it
  /// emptied: somebody who stepped out of their own room came back to an
  /// identifier that no longer existed, and a list that still showed it. A
  /// room now ends only when somebody closes it, which is `remove_room`.
  ///
  /// `name` is trimmed, and an empty one becomes the identifier the room was
  /// just given. Naming a room is optional and always was; what changed is
  /// that the fallback is decided here, once, instead of by whichever screen
  /// happens to be showing the room. Callers may pass whatever a person typed.
  ///
  /// Fails with `room_name_taken` when another room already answers to that
  /// name, compared by `models::room_name_key`. A name is what somebody reads
  /// in a list and then clicks, so two rooms wearing one name is a list that
  /// cannot be acted on.
  [[nodiscard]] Result<std::string> create_room(const std::string& name, std::string owner_id = {});

  /// Reads the rooms back from the store, so that an identifier somebody wrote
  /// down still works after a restart. Rooms come back empty: who was inside
  /// did not survive the process and pretending otherwise would be a
  /// participant list nobody is connected to.
  ///
  /// Returns how many were loaded. Does nothing without a store.
  std::size_t load_rooms();

  /// Deletes a room whatever its state, persistent or not, and reports who was
  /// in it so the caller can tell them. This is the administrative close, and
  /// the one path that removes a persistent room.
  [[nodiscard]] Result<std::vector<std::string>> remove_room(const std::string& room_id);

  /// Adds a participant. Fails with room_not_found, room_full or
  /// already_in_room.
  [[nodiscard]] std::optional<Error> join(const std::string& room_id, models::User user);

  /// Removes a participant, and the room itself once it becomes empty.
  [[nodiscard]] std::optional<Error> leave(const std::string& room_id, const std::string& user_id);

  /// Removes a participant from whatever room they are in. Used when a
  /// connection drops and the client never sent leave_room.
  /// Returns the room the user was removed from, if any.
  [[nodiscard]] std::optional<std::string> remove_from_any_room(const std::string& user_id);

  /// Mutes or unmutes a participant.
  ///
  /// `by_admin` records who did it, and is what makes a forced mute hold: a
  /// participant unmuting themselves is refused while it is set, and only a
  /// forced unmute clears it. Fails with `forbidden` in that case.
  [[nodiscard]] std::optional<Error> set_muted(const std::string& room_id,
                                               const std::string& user_id, bool muted,
                                               bool by_admin = false);

  /// Fails with screen_share_busy when someone else is already sharing.
  /// Starting a share the user already owns succeeds and changes nothing.
  ///
  /// `with_audio` is remembered rather than acted on: what the sharer's machine
  /// is playing travels in their own audio track and never reaches the server
  /// as a separate thing. It is kept so that somebody joining mid-share can be
  /// told what they are about to hear.
  [[nodiscard]] std::optional<Error> start_screen_share(const std::string& room_id,
                                                        const std::string& user_id,
                                                        bool with_audio = false);

  [[nodiscard]] std::optional<Error> stop_screen_share(const std::string& room_id,
                                                       const std::string& user_id);

  [[nodiscard]] const models::Room* find(const std::string& room_id) const;

  /// Every live room. Unordered, because the underlying map is; the caller
  /// sorts if it cares, and the only caller is an administrative listing.
  [[nodiscard]] std::vector<models::Room> list() const;

  [[nodiscard]] std::optional<std::string> room_of(const std::string& user_id) const;

  /// A room this user created, if they have one. Which one, when they have
  /// several, is unspecified: the caller that matters asks whether there is
  /// any, and names it so the answer can say which.
  ///
  /// Ownership outlives being inside: rooms no longer end when they empty, so
  /// "the room you made" is a thing somebody keeps having until it is closed.
  /// That is what makes a limit on it mean anything.
  [[nodiscard]] std::optional<std::string> room_owned_by(const std::string& user_id) const;

  [[nodiscard]] std::size_t room_count() const noexcept { return rooms_.size(); }

  [[nodiscard]] int max_participants_per_room() const noexcept {
    return options_.max_participants_per_room;
  }

 private:
  [[nodiscard]] std::string generate_room_id();

  /// Whether a room already answers to this name. `key` is what
  /// `models::room_name_key` returns, not the name as typed.
  ///
  /// A scan, and not an index kept beside the map. The map is the one place a
  /// room exists, so a scan cannot disagree with it, and an index would have
  /// to be corrected in create, remove and load without ever being the thing
  /// anybody reads. `room_owned_by` scans for the same reason.
  [[nodiscard]] bool name_taken(const std::string& key) const;

  /// Called from the two places a room stops existing, and from nowhere else.
  ///
  /// Const because it changes nothing here: the room is already gone from the
  /// map by the time it runs, and what is left to do is in the store.
  void forget(const std::string& room_id) const;

  Options options_;
  std::mt19937 random_;
  std::unordered_map<std::string, models::Room> rooms_;
  /// user id to room id, so a dropped connection does not have to scan every
  /// room to be cleaned up.
  std::unordered_map<std::string, std::string> user_to_room_;
};

}  // namespace dv::server
