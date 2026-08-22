#pragma once

#include <optional>
#include <string>
#include <vector>

#include <dv/core/result.hpp>
#include <dv/models/chat.hpp>

namespace dv::server::store {

/// What was said in each room.
///
/// A failure to write has to stop the message, which is the opposite of what
/// the audit log does, and the difference is worth stating. An audit entry
/// records something that happened elsewhere, so refusing to remove a
/// disruptive participant because the database is unreachable would protect
/// the record at the expense of the thing being recorded. A chat message has
/// no elsewhere: the store is the message. Broadcasting one that was not
/// written produces a conversation everybody present saw and nobody who
/// reconnects can find, and an error the sender can act on is better than a
/// hole they will never be told about.
///
/// Not thread safe. See UserStore.
class ChatStore {
 public:
  /// How many messages `list` returns when the caller asks for no particular
  /// number, and the ceiling it clamps any request to. A panel shows the end
  /// of a conversation rather than all of it, and an unbounded query is a way
  /// to make the server read a collection that only grows.
  static constexpr int kDefaultLimit = 100;
  static constexpr int kMaxLimit = 500;

  ChatStore() = default;
  virtual ~ChatStore() = default;

  ChatStore(const ChatStore&) = delete;
  ChatStore& operator=(const ChatStore&) = delete;
  ChatStore(ChatStore&&) = delete;
  ChatStore& operator=(ChatStore&&) = delete;

  /// Writes one message and hands back what was written.
  ///
  /// The store assigns `id` and stamps `timestamp_seconds` when it is zero,
  /// and the copy that comes back carries both. That is why this returns the
  /// message rather than only a failure, unlike every other write in these
  /// stores: what the room is shown has to be the row that exists, so that a
  /// participant reading the conversation later sees the same identifiers and
  /// the same order as the one who was there.
  [[nodiscard]] virtual Result<models::ChatMessage> append(models::ChatMessage message) = 0;

  /// The newest `limit` messages of one room, oldest first.
  ///
  /// Both halves of that matter. The window is taken from the end, because the
  /// end is the part anybody wants to see; it is handed back in the order it
  /// is read in, so that no client has to reverse it before displaying it.
  /// `limit` is clamped to the range above.
  [[nodiscard]] virtual std::vector<models::ChatMessage> list(const std::string& room_id,
                                                              int limit) const = 0;

  /// Forgets everything said in a room, and reports nothing when there was
  /// nothing: a room where nobody spoke is not a failure to clear.
  ///
  /// Not optional housekeeping. Room identifiers are six characters and are
  /// handed out again once a room is gone, so a history that outlived its room
  /// would one day be shown to strangers who happen to be given the same
  /// identifier. See RoomManager, which is what calls this.
  [[nodiscard]] virtual std::optional<Error> clear(const std::string& room_id) = 0;
};

/// Clamps a requested limit into the range the store allows.
[[nodiscard]] constexpr int clamp_chat_limit(int requested) noexcept {
  if (requested <= 0) {
    return ChatStore::kDefaultLimit;
  }
  return requested > ChatStore::kMaxLimit ? ChatStore::kMaxLimit : requested;
}

}  // namespace dv::server::store
