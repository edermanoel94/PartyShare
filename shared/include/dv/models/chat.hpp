#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dv::models {

/// One line of a room's conversation, as it is stored and as it crosses the
/// wire.
///
/// One type for both, for the same reason models::AuditEntry is one type: a
/// message exists to be read back by a person, and a second representation
/// between the database and the screen would be two chances to drop a field
/// and nothing gained.
struct ChatMessage {
  /// Assigned by the store.
  std::string id;
  std::string room_id;
  /// Who wrote it. Both the identifier and the name are kept, because the
  /// author may leave, rename themselves or have their account deleted, and a
  /// history that then reads "who: unknown" has lost what it was written for.
  /// The name is the one they held when they sent it, not the one they hold
  /// now: the history says what the room saw at the time.
  std::string user_id;
  std::string display_name;
  std::string text;
  /// Seconds since the Unix epoch, UTC. A wall clock rather than a steady one:
  /// this is read by a person, possibly long after the process that wrote it
  /// stopped.
  std::int64_t timestamp_seconds = 0;

  friend bool operator==(const ChatMessage&, const ChatMessage&) = default;
};

/// The longest a message may be, in bytes.
///
/// A limit, and not a generous one. This is a line somebody types during a
/// call; the server broadcasts it to the whole room and writes it to a
/// database, and an unbounded field only buys a way to make it do both with a
/// megabyte. Anything longer is refused rather than truncated, because a
/// message that arrives cut in half is worse than one that never arrived.
inline constexpr std::size_t kMaxChatTextBytes = 2000;

/// `text` without the whitespace around it. What the server stores, so that a
/// line of nothing but spaces cannot take up a row of everybody's history.
[[nodiscard]] std::string trim_chat_text(const std::string& text);

/// Whether the server will accept `text`: not empty once trimmed, and within
/// kMaxChatTextBytes.
[[nodiscard]] bool is_valid_chat_text(const std::string& text);

}  // namespace dv::models
