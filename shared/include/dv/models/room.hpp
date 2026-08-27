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
  /// Who created it. Empty for a room that predates the field.
  std::string owner_id;
  /// A persistent room outlives its last participant, which is what makes an
  /// identifier worth writing down and reusing. Only an administrator can
  /// create one. An ordinary room is still deleted the moment it empties, so
  /// nothing accumulates on its own.
  bool persistent = false;

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

/// The longest a room name may be, in bytes.
///
/// Bounded for the same reason a chat line is: this is text a participant
/// types, and the server writes it to the database and shows it in a column on
/// everybody else's home page. Anything longer is refused rather than
/// truncated, so that the name somebody chose and the name the room carries are
/// never two different strings.
inline constexpr std::size_t kMaxRoomNameBytes = 48;

/// `name` without the whitespace around it. What the server stores, so that a
/// name of nothing but spaces cannot masquerade as one somebody chose.
[[nodiscard]] std::string trim_room_name(const std::string& name);

/// Whether the server will accept `name`. An empty name is valid: it is how a
/// client asks for the room to be called by its own identifier, and the
/// RoomManager is what fills that in.
[[nodiscard]] bool is_valid_room_name(const std::string& name);

/// The form two room names are compared in when deciding whether one is
/// already taken: trimmed, with the ASCII letters folded to lower case.
///
/// Only the ASCII letters. Full case folding needs a Unicode table, which is a
/// dependency this project does not carry for one comparison, and the partial
/// job is worth more than nothing here: names differ in case mostly at the
/// first letter, so "Reunião" and "reunião" do collide. "REUNIÃO" does not,
/// because Ã and ã are different bytes and nothing here knows they are a pair.
///
/// Folding only 'A' to 'Z' is safe on text that is not ASCII: every byte of a
/// multibyte UTF-8 character is at least 0x80, so none of them can be mistaken
/// for a letter this touches.
[[nodiscard]] std::string room_name_key(const std::string& name);

/// How a room is named in a log line: what people call it, falling back to the
/// identifier when nobody named it.
///
/// The fallback is very nearly dead code, because `RoomManager::create_room`
/// already writes the identifier into the name of a room nobody named, once,
/// so that no screen has to. It is kept for the two callers that can still
/// reach an empty name: a room stored before the field existed, and an
/// identifier that no room answers to at all, which is what a log line about a
/// room that has just been closed is holding.
///
/// Unlike `user_label` this prints one thing and not two. A room name is
/// unique, because `create_room` refuses one that is taken, so the name alone
/// already says which room. See docs/05-rooms.md.
[[nodiscard]] std::string room_label(const std::string& id, const std::string& name);

}  // namespace dv::models
