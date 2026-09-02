#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dv::models {

/// One thing an administrator told one account, and whether they have said
/// they read it.
///
/// Deliberately not a chat message, and the differences are the whole design.
/// A chat line belongs to a room, is shown to everybody in it, and needs no
/// answer; a notice belongs to an account, is shown to nobody else, and exists
/// precisely to be answered - the answer being one button that says the person
/// saw it. A room outlives nothing; a notice outlives the session, the room
/// and the process, because somebody who was not connected when it was sent is
/// exactly who it most needs to reach.
///
/// One type for the store and for the wire, like ChatMessage and AuditEntry,
/// and for the reason given there: a second representation between the
/// database and the screen is two chances to drop a field and nothing gained.
struct Notice {
  /// Assigned by the store. It is what an acknowledgement names, so a notice
  /// that was not written down cannot be acknowledged and is not sent.
  std::string id;
  /// The account it is for. Not a room and not a connection: the same notice
  /// is delivered to whichever session that account next opens.
  std::string user_id;
  /// The administrator who wrote it. Both halves kept, for the reason
  /// AuditEntry keeps both: the account may be deleted or renamed later, and a
  /// notice that then reads "from: unknown" has lost what it was for. The name
  /// is the one they held when they sent it, not the one they hold now.
  std::string from_user_id;
  std::string from_display_name;
  std::string text;
  /// Seconds since the Unix epoch, UTC. A wall clock rather than a steady one:
  /// this is read by a person, possibly long after the process that wrote it
  /// stopped.
  std::int64_t created_at = 0;
  /// When the person said they had read it, and zero while they have not.
  ///
  /// A timestamp rather than a boolean, because the two questions an
  /// administrator asks are "did they see it" and "when", and the second one
  /// is not answerable from a flag. Zero is the safe reading of a document
  /// written before this field existed: an unacknowledged notice is delivered
  /// again, which is a repetition, where a wrongly acknowledged one is a
  /// message nobody ever sees.
  std::int64_t acknowledged_at = 0;

  [[nodiscard]] bool acknowledged() const noexcept { return acknowledged_at != 0; }

  friend bool operator==(const Notice&, const Notice&) = default;
};

/// The longest a notice may be, in bytes.
///
/// A quarter of what a chat line may be, and the smaller limit is the point.
/// This is an instruction or a warning aimed at one person, delivered as a box
/// they have to dismiss before carrying on; anything that does not fit in a
/// few lines is a conversation, and a conversation wants the chat rather than a
/// modal window with one button. It is also written into an audit entry, and a
/// log whose rows can each be two kilobytes is a log nobody scrolls.
///
/// Anything longer is refused rather than truncated, for the reason chat gives:
/// a message that arrives cut in half is worse than one that never arrived.
inline constexpr std::size_t kMaxNoticeTextBytes = 500;

/// `text` without the whitespace around it. What the server stores, so that a
/// notice of nothing but spaces cannot become a box somebody has to dismiss.
[[nodiscard]] std::string trim_notice_text(const std::string& text);

/// Whether the server will accept `text`: not empty once trimmed, and within
/// kMaxNoticeTextBytes.
[[nodiscard]] bool is_valid_notice_text(const std::string& text);

}  // namespace dv::models
