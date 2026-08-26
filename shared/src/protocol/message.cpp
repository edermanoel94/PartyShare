#include <array>
#include <utility>

#include <nlohmann/json.hpp>

#include <dv/protocol/message.hpp>

namespace dv::protocol {
namespace {

using nlohmann::json;

struct TypeMapping {
  MessageType type;
  std::string_view name;
};

// The single source of truth for the wire names. docs/protocol.md must match.
constexpr std::array<TypeMapping, 38> kTypeMappings{{
    {.type = MessageType::Authenticate, .name = "authenticate"},
    {.type = MessageType::Authenticated, .name = "authenticated"},
    {.type = MessageType::CreateRoom, .name = "create_room"},
    {.type = MessageType::RoomCreated, .name = "room_created"},
    {.type = MessageType::JoinRoom, .name = "join_room"},
    {.type = MessageType::LeaveRoom, .name = "leave_room"},
    {.type = MessageType::UserJoined, .name = "user_joined"},
    {.type = MessageType::UserLeft, .name = "user_left"},
    {.type = MessageType::Offer, .name = "offer"},
    {.type = MessageType::Answer, .name = "answer"},
    {.type = MessageType::IceCandidate, .name = "ice_candidate"},
    {.type = MessageType::ScreenShareStarted, .name = "screen_share_started"},
    {.type = MessageType::ScreenShareStopped, .name = "screen_share_stopped"},
    {.type = MessageType::Mute, .name = "mute"},
    {.type = MessageType::Unmute, .name = "unmute"},
    {.type = MessageType::ChangePassword, .name = "change_password"},
    {.type = MessageType::PasswordChanged, .name = "password_changed"},
    {.type = MessageType::ChatMessage, .name = "chat_message"},
    {.type = MessageType::ListChat, .name = "list_chat"},
    {.type = MessageType::ChatHistory, .name = "chat_history"},
    {.type = MessageType::Error, .name = "error"},
    {.type = MessageType::Ping, .name = "ping"},
    {.type = MessageType::Pong, .name = "pong"},
    {.type = MessageType::KickUser, .name = "kick_user"},
    {.type = MessageType::UserKicked, .name = "user_kicked"},
    {.type = MessageType::ForceMute, .name = "force_mute"},
    {.type = MessageType::RestrictUser, .name = "restrict_user"},
    {.type = MessageType::UserRestricted, .name = "user_restricted"},
    {.type = MessageType::ListUsers, .name = "list_users"},
    {.type = MessageType::UserList, .name = "user_list"},
    {.type = MessageType::CreateUser, .name = "create_user"},
    {.type = MessageType::UpdateUser, .name = "update_user"},
    {.type = MessageType::DeleteUser, .name = "delete_user"},
    {.type = MessageType::ListRooms, .name = "list_rooms"},
    {.type = MessageType::RoomList, .name = "room_list"},
    {.type = MessageType::DeleteRoom, .name = "delete_room"},
    {.type = MessageType::ListAudit, .name = "list_audit"},
    {.type = MessageType::AuditList, .name = "audit_list"},
}};

/// Reads fields out of a JSON object, remembering the first failure instead of
/// throwing. A parser reads everything it needs and checks `ok()` once at the
/// end, which keeps the per message code free of error plumbing.
class FieldReader {
 public:
  explicit FieldReader(const json& object) : object_(object) {}

  std::string string(std::string_view key) {
    const json* field = require(key);
    if (field == nullptr) {
      return {};
    }
    if (!field->is_string()) {
      fail("invalid_type", key, "expected a string");
      return {};
    }
    return field->get<std::string>();
  }

  std::string optional_string(std::string_view key, const std::string& fallback = {}) {
    const json* field = find(key);
    if (field == nullptr || field->is_null()) {
      return fallback;
    }
    if (!field->is_string()) {
      fail("invalid_type", key, "expected a string");
      return fallback;
    }
    return field->get<std::string>();
  }

  /// Absent and present-but-null both answer nothing, which is what lets
  /// `update_user` tell "leave this alone" apart from "set it to empty".
  std::optional<std::string> maybe_string(std::string_view key) {
    const json* field = find(key);
    if (field == nullptr || field->is_null()) {
      return std::nullopt;
    }
    if (!field->is_string()) {
      fail("invalid_type", key, "expected a string");
      return std::nullopt;
    }
    return field->get<std::string>();
  }

  /// A boolean that has to be there. Used where absence has no safe reading:
  /// a force_mute without `muted` is a request whose direction the server
  /// would have to guess, and guessing "mute" is the wrong way to be wrong.
  bool require_boolean(std::string_view key) {
    const json* field = require(key);
    if (field == nullptr) {
      return false;
    }
    if (!field->is_boolean()) {
      fail("invalid_type", key, "expected a boolean");
      return false;
    }
    return field->get<bool>();
  }

  bool boolean(std::string_view key, bool fallback = false) {
    const json* field = find(key);
    if (field == nullptr || field->is_null()) {
      return fallback;
    }
    if (!field->is_boolean()) {
      fail("invalid_type", key, "expected a boolean");
      return fallback;
    }
    return field->get<bool>();
  }

  /// The boolean counterpart of maybe_string, and there for the same reason.
  /// `restrict_user` has to tell "lift this" apart from "leave this alone", and
  /// a flag that read as false when absent would make every request a claim
  /// about all four restrictions.
  std::optional<bool> maybe_boolean(std::string_view key) {
    const json* field = find(key);
    if (field == nullptr || field->is_null()) {
      return std::nullopt;
    }
    if (!field->is_boolean()) {
      fail("invalid_type", key, "expected a boolean");
      return std::nullopt;
    }
    return field->get<bool>();
  }

  std::int64_t optional_integer(std::string_view key, std::int64_t fallback = 0) {
    const json* field = find(key);
    if (field == nullptr || field->is_null()) {
      return fallback;
    }
    if (!field->is_number_integer()) {
      fail("invalid_type", key, "expected an integer");
      return fallback;
    }
    return field->get<std::int64_t>();
  }

  int integer(std::string_view key) {
    const json* field = require(key);
    if (field == nullptr) {
      return 0;
    }
    if (!field->is_number_integer()) {
      fail("invalid_type", key, "expected an integer");
      return 0;
    }
    return field->get<int>();
  }

  /// Absent, null, or a flag missing from inside it, all read as "nothing is
  /// taken away". A client or a database from before restrictions existed
  /// sends no such object, and must not acquire a ban by omission any more
  /// than it acquires the administrator role by omitting `role`.
  models::Restrictions restrictions(std::string_view key) {
    const json* field = find(key);
    if (field == nullptr || field->is_null()) {
      return {};
    }
    if (!field->is_object()) {
      fail("invalid_type", key, "expected an object");
      return {};
    }
    FieldReader nested(*field);
    models::Restrictions value;
    value.banned = nested.boolean("banned");
    value.muted = nested.boolean("muted");
    value.silenced = nested.boolean("silenced");
    value.screen_share_blocked = nested.boolean("screen_share_blocked");
    if (!nested.ok()) {
      fail(nested.error().code, key, nested.error().message);
    }
    return value;
  }

  models::User user(std::string_view key) {
    const json* field = require(key);
    if (field == nullptr) {
      return {};
    }
    if (!field->is_object()) {
      fail("invalid_type", key, "expected an object");
      return {};
    }
    FieldReader nested(*field);
    models::User value;
    value.id = nested.string("id");
    value.display_name = nested.string("display_name");
    value.avatar = nested.optional_string("avatar");
    // Optional, and anything unrecognised reads as the plain role. A client
    // from before roles existed sends no field, and must not become an
    // administrator by omission.
    value.role = models::role_from_string(nested.optional_string("role"));
    value.restrictions = nested.restrictions("restrictions");
    if (!nested.ok()) {
      fail(nested.error().code, key, nested.error().message);
    }
    return value;
  }

  /// Reads a nested object, building a `T` from it through `read`, which is
  /// handed a reader over that object.
  ///
  /// A failure inside is reported against the object's own key, so that a
  /// malformed chat message says "message ..." rather than naming a field the
  /// sender cannot place.
  template <typename T, typename Read>
  T object(std::string_view key, Read read) {
    const json* field = require(key);
    if (field == nullptr) {
      return {};
    }
    if (!field->is_object()) {
      fail("invalid_type", key, "expected an object");
      return {};
    }
    FieldReader nested(*field);
    T value = read(nested);
    if (!nested.ok()) {
      fail(nested.error().code, key, nested.error().message);
      return {};
    }
    return value;
  }

  /// Reads an array of objects, building one `T` per element through `read`,
  /// which is handed a reader over that element.
  ///
  /// A failure inside an element is reported against the array's own key. The
  /// index is not in the message because the caller cannot act on it: a
  /// malformed list is refused whole either way.
  template <typename T, typename Read>
  std::vector<T> array(std::string_view key, Read read) {
    std::vector<T> values;
    const json* field = require(key);
    if (field == nullptr) {
      return values;
    }
    if (!field->is_array()) {
      fail("invalid_type", key, "expected an array");
      return values;
    }
    values.reserve(field->size());
    for (const json& element : *field) {
      if (!element.is_object()) {
        fail("invalid_type", key, "expected an array of objects");
        return values;
      }
      FieldReader nested(element);
      T value = read(nested);
      if (!nested.ok()) {
        fail(nested.error().code, key, nested.error().message);
        return values;
      }
      values.push_back(std::move(value));
    }
    return values;
  }

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  /// Only after ok() has said false.
  /// NOLINTNEXTLINE(bugprone-unchecked-optional-access)
  [[nodiscard]] const Error& error() const { return *error_; }

 private:
  [[nodiscard]] const json* find(std::string_view key) const {
    const auto it = object_.find(key);
    return it == object_.end() ? nullptr : &*it;
  }

  const json* require(std::string_view key) {
    const json* field = find(key);
    if (field == nullptr || field->is_null()) {
      fail("missing_field", key, "is required");
      return nullptr;
    }
    return field;
  }

  void fail(std::string_view code, std::string_view key, std::string_view reason) {
    if (error_.has_value()) {
      return;  // keep the first failure, it is the most informative
    }
    error_ =
        Error{.code = std::string(code), .message = std::string(key) + " " + std::string(reason)};
  }

  // A reference rather than a copy, and the object outlives this by
  // construction: the reader is a local built around one message and dies
  // with it. Copying a parsed message per field read would be the
  // alternative.
  // NOLINTNEXTLINE(cppcoreguidelines-avoid-const-or-ref-data-members)
  const json& object_;
  std::optional<Error> error_;
};

/// Always written, even when nothing is taken away, so that a reader never has
/// to tell "no restrictions" apart from "a sender that does not know about
/// them". Both mean the same thing here, and writing the object anyway is what
/// keeps a packet capture readable without that reasoning.
json restrictions_to_json(const models::Restrictions& restrictions) {
  return json{{"banned", restrictions.banned},
              {"muted", restrictions.muted},
              {"silenced", restrictions.silenced},
              {"screen_share_blocked", restrictions.screen_share_blocked}};
}

json user_to_json(const models::User& user) {
  return json{{"id", user.id},
              {"display_name", user.display_name},
              {"avatar", user.avatar},
              {"role", models::to_string(user.role)},
              {"restrictions", restrictions_to_json(user.restrictions)}};
}

json user_summary_to_json(const UserSummary& summary) {
  return json{{"user", user_to_json(summary.user)},
              {"username", summary.username},
              {"created_at", summary.created_at},
              {"online", summary.online}};
}

UserSummary user_summary_from(FieldReader& reader) {
  UserSummary value;
  value.user = reader.user("user");
  value.username = reader.string("username");
  value.created_at = reader.optional_integer("created_at");
  value.online = reader.boolean("online");
  return value;
}

json room_summary_to_json(const RoomSummary& summary) {
  return json{{"id", summary.id},
              {"name", summary.name},
              {"owner_id", summary.owner_id},
              {"persistent", summary.persistent},
              {"participant_count", summary.participant_count}};
}

RoomSummary room_summary_from(FieldReader& reader) {
  RoomSummary value;
  value.id = reader.string("id");
  value.name = reader.optional_string("name");
  value.owner_id = reader.optional_string("owner_id");
  value.persistent = reader.boolean("persistent");
  value.participant_count = static_cast<int>(reader.optional_integer("participant_count"));
  return value;
}

json chat_message_to_json(const models::ChatMessage& message) {
  return json{{"id", message.id},           {"room_id", message.room_id},
              {"user_id", message.user_id}, {"display_name", message.display_name},
              {"text", message.text},       {"timestamp_seconds", message.timestamp_seconds}};
}

models::ChatMessage chat_message_from(FieldReader& reader) {
  models::ChatMessage value;
  // Only the three fields a client has to provide are required. The rest are
  // the server's to fill in, so a message on its way up carries them empty and
  // is not refused for it.
  value.id = reader.optional_string("id");
  value.room_id = reader.string("room_id");
  value.user_id = reader.string("user_id");
  value.display_name = reader.optional_string("display_name");
  value.text = reader.string("text");
  value.timestamp_seconds = reader.optional_integer("timestamp_seconds");
  return value;
}

json audit_entry_to_json(const models::AuditEntry& entry) {
  return json{{"id", entry.id},
              {"actor_id", entry.actor_id},
              {"actor_username", entry.actor_username},
              {"action", entry.action},
              {"target_id", entry.target_id},
              {"room_id", entry.room_id},
              {"detail", entry.detail},
              {"timestamp_seconds", entry.timestamp_seconds}};
}

models::AuditEntry audit_entry_from(FieldReader& reader) {
  models::AuditEntry value;
  value.id = reader.optional_string("id");
  value.actor_id = reader.optional_string("actor_id");
  value.actor_username = reader.optional_string("actor_username");
  value.action = reader.string("action");
  value.target_id = reader.optional_string("target_id");
  value.room_id = reader.optional_string("room_id");
  value.detail = reader.optional_string("detail");
  value.timestamp_seconds = reader.optional_integer("timestamp_seconds");
  return value;
}

Result<Message> finish(FieldReader& reader, Message message) {
  if (!reader.ok()) {
    return Result<Message>::failure(reader.error());
  }
  return message;
}

}  // namespace

std::string_view type_name(MessageType type) noexcept {
  for (const auto& mapping : kTypeMappings) {
    if (mapping.type == type) {
      return mapping.name;
    }
  }
  return "unknown";
}

std::optional<MessageType> type_from_name(std::string_view name) noexcept {
  for (const auto& mapping : kTypeMappings) {
    if (mapping.name == name) {
      return mapping.type;
    }
  }
  return std::nullopt;
}

// std::visit throws only on a valueless variant, which happens when a move
// constructor throws during assignment. Every alternative here moves without
// throwing, so this cannot, and noexcept is a guarantee worth keeping.
// NOLINTNEXTLINE(bugprone-exception-escape)
MessageType type_of(const Message& message) noexcept {
  return std::visit(
      [](const auto& value) -> MessageType {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Authenticate>) {
          return MessageType::Authenticate;
        }
        if constexpr (std::is_same_v<T, Authenticated>) {
          return MessageType::Authenticated;
        }
        if constexpr (std::is_same_v<T, CreateRoom>) {
          return MessageType::CreateRoom;
        }
        if constexpr (std::is_same_v<T, RoomCreated>) {
          return MessageType::RoomCreated;
        }
        if constexpr (std::is_same_v<T, JoinRoom>) {
          return MessageType::JoinRoom;
        }
        if constexpr (std::is_same_v<T, LeaveRoom>) {
          return MessageType::LeaveRoom;
        }
        if constexpr (std::is_same_v<T, UserJoined>) {
          return MessageType::UserJoined;
        }
        if constexpr (std::is_same_v<T, UserLeft>) {
          return MessageType::UserLeft;
        }
        if constexpr (std::is_same_v<T, Offer>) {
          return MessageType::Offer;
        }
        if constexpr (std::is_same_v<T, Answer>) {
          return MessageType::Answer;
        }
        if constexpr (std::is_same_v<T, IceCandidate>) {
          return MessageType::IceCandidate;
        }
        if constexpr (std::is_same_v<T, ScreenShareStarted>) {
          return MessageType::ScreenShareStarted;
        }
        if constexpr (std::is_same_v<T, ScreenShareStopped>) {
          return MessageType::ScreenShareStopped;
        }
        if constexpr (std::is_same_v<T, Mute>) {
          return MessageType::Mute;
        }
        if constexpr (std::is_same_v<T, Unmute>) {
          return MessageType::Unmute;
        }
        if constexpr (std::is_same_v<T, ChangePassword>) {
          return MessageType::ChangePassword;
        }
        if constexpr (std::is_same_v<T, PasswordChanged>) {
          return MessageType::PasswordChanged;
        }
        if constexpr (std::is_same_v<T, ChatMessage>) {
          return MessageType::ChatMessage;
        }
        if constexpr (std::is_same_v<T, ListChat>) {
          return MessageType::ListChat;
        }
        if constexpr (std::is_same_v<T, ChatHistory>) {
          return MessageType::ChatHistory;
        }
        if constexpr (std::is_same_v<T, ErrorMessage>) {
          return MessageType::Error;
        }
        if constexpr (std::is_same_v<T, Ping>) {
          return MessageType::Ping;
        }
        if constexpr (std::is_same_v<T, Pong>) {
          return MessageType::Pong;
        }
        if constexpr (std::is_same_v<T, KickUser>) {
          return MessageType::KickUser;
        }
        if constexpr (std::is_same_v<T, UserKicked>) {
          return MessageType::UserKicked;
        }
        if constexpr (std::is_same_v<T, ForceMute>) {
          return MessageType::ForceMute;
        }
        if constexpr (std::is_same_v<T, RestrictUser>) {
          return MessageType::RestrictUser;
        }
        if constexpr (std::is_same_v<T, UserRestricted>) {
          return MessageType::UserRestricted;
        }
        if constexpr (std::is_same_v<T, ListUsers>) {
          return MessageType::ListUsers;
        }
        if constexpr (std::is_same_v<T, UserList>) {
          return MessageType::UserList;
        }
        if constexpr (std::is_same_v<T, CreateUser>) {
          return MessageType::CreateUser;
        }
        if constexpr (std::is_same_v<T, UpdateUser>) {
          return MessageType::UpdateUser;
        }
        if constexpr (std::is_same_v<T, DeleteUser>) {
          return MessageType::DeleteUser;
        }
        if constexpr (std::is_same_v<T, ListRooms>) {
          return MessageType::ListRooms;
        }
        if constexpr (std::is_same_v<T, RoomList>) {
          return MessageType::RoomList;
        }
        if constexpr (std::is_same_v<T, DeleteRoom>) {
          return MessageType::DeleteRoom;
        }
        if constexpr (std::is_same_v<T, ListAudit>) {
          return MessageType::ListAudit;
        }
        return MessageType::AuditList;
      },
      message);
}

std::string serialize(const Message& message) {
  json root;
  root["type"] = type_name(type_of(message));

  std::visit(
      [&root](const auto& value) {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Authenticate>) {
          root["username"] = value.username;
          root["password"] = value.password;
        } else if constexpr (std::is_same_v<T, Authenticated>) {
          root["user"] = user_to_json(value.user);
          root["token"] = value.token;
          root["expires_in_seconds"] = value.expires_in_seconds;
        } else if constexpr (std::is_same_v<T, CreateRoom>) {
          root["user_id"] = value.user_id;
          root["room_name"] = value.room_name;
          root["persistent"] = value.persistent;
        } else if constexpr (std::is_same_v<T, RoomCreated>) {
          root["room_id"] = value.room_id;
          root["room_name"] = value.room_name;
        } else if constexpr (std::is_same_v<T, JoinRoom>) {
          root["room_id"] = value.room_id;
          root["user_id"] = value.user_id;
          root["display_name"] = value.display_name;
        } else if constexpr (std::is_same_v<T, UserJoined>) {
          root["room_id"] = value.room_id;
          root["user"] = user_to_json(value.user);
        } else if constexpr (std::is_same_v<T, Offer> || std::is_same_v<T, Answer>) {
          root["room_id"] = value.room_id;
          root["from_user_id"] = value.from_user_id;
          root["to_user_id"] = value.to_user_id;
          root["sdp"] = value.sdp;
        } else if constexpr (std::is_same_v<T, IceCandidate>) {
          root["room_id"] = value.room_id;
          root["from_user_id"] = value.from_user_id;
          root["to_user_id"] = value.to_user_id;
          root["candidate"] = value.candidate;
          root["sdp_mid"] = value.sdp_mid;
          root["sdp_mline_index"] = value.sdp_mline_index;
        } else if constexpr (std::is_same_v<T, ScreenShareStarted>) {
          // Was one of the room-and-user messages below until the screen
          // learned to carry sound.
          root["room_id"] = value.room_id;
          root["user_id"] = value.user_id;
          root["has_audio"] = value.has_audio;
        } else if constexpr (std::is_same_v<T, LeaveRoom> || std::is_same_v<T, UserLeft> ||
                             std::is_same_v<T, ScreenShareStopped>) {
          // Everything whose payload is exactly a room and a user.
          root["room_id"] = value.room_id;
          root["user_id"] = value.user_id;
        } else if constexpr (std::is_same_v<T, Mute> || std::is_same_v<T, Unmute>) {
          root["room_id"] = value.room_id;
          root["user_id"] = value.user_id;
          root["by_user_id"] = value.by_user_id;
        } else if constexpr (std::is_same_v<T, ChangePassword>) {
          root["current_password"] = value.current_password;
          root["new_password"] = value.new_password;
        } else if constexpr (std::is_same_v<T, PasswordChanged>) {
          // No payload at all. The type is the whole message.
        } else if constexpr (std::is_same_v<T, ChatMessage>) {
          root["message"] = chat_message_to_json(value.message);
        } else if constexpr (std::is_same_v<T, ListChat>) {
          root["room_id"] = value.room_id;
          root["limit"] = value.limit;
        } else if constexpr (std::is_same_v<T, ChatHistory>) {
          root["room_id"] = value.room_id;
          json messages = json::array();
          // `line` and not `message`, which is the name of the parameter this
          // lambda sits inside. GCC calls that a shadow and the build treats
          // it as an error; Clang says nothing, because the lambda does not
          // capture the parameter, so this only ever fails on the Linux job.
          for (const models::ChatMessage& line : value.messages) {
            messages.push_back(chat_message_to_json(line));
          }
          root["messages"] = std::move(messages);
        } else if constexpr (std::is_same_v<T, ErrorMessage>) {
          root["code"] = value.code;
          root["message"] = value.message;
        } else if constexpr (std::is_same_v<T, KickUser> || std::is_same_v<T, UserKicked>) {
          root["room_id"] = value.room_id;
          root["user_id"] = value.user_id;
          root["reason"] = value.reason;
        } else if constexpr (std::is_same_v<T, ForceMute>) {
          root["room_id"] = value.room_id;
          root["user_id"] = value.user_id;
          root["muted"] = value.muted;
        } else if constexpr (std::is_same_v<T, RestrictUser>) {
          root["user_id"] = value.user_id;
          root["reason"] = value.reason;
          // Only the flags that were actually asked about. Writing the absent
          // ones as null would be the same message on the wire, and writing
          // them as false would turn "leave this alone" into "lift this".
          const auto write = [&root](const char* key, const std::optional<bool>& flag) {
            if (flag.has_value()) {
              root[key] = *flag;
            }
          };
          write("banned", value.banned);
          write("muted", value.muted);
          write("silenced", value.silenced);
          write("screen_share_blocked", value.screen_share_blocked);
        } else if constexpr (std::is_same_v<T, UserRestricted>) {
          root["user_id"] = value.user_id;
          root["restrictions"] = restrictions_to_json(value.restrictions);
          root["by_user_id"] = value.by_user_id;
          root["reason"] = value.reason;
          root["room_id"] = value.room_id;
        } else if constexpr (std::is_same_v<T, ListUsers> || std::is_same_v<T, ListRooms>) {
          // No payload at all. The type is the whole message.
        } else if constexpr (std::is_same_v<T, UserList>) {
          json users = json::array();
          for (const UserSummary& summary : value.users) {
            users.push_back(user_summary_to_json(summary));
          }
          root["users"] = std::move(users);
        } else if constexpr (std::is_same_v<T, CreateUser>) {
          root["username"] = value.username;
          root["password"] = value.password;
          root["display_name"] = value.display_name;
          root["role"] = models::to_string(value.role);
        } else if constexpr (std::is_same_v<T, UpdateUser>) {
          root["user_id"] = value.user_id;
          // An absent field means "leave this alone", so an unset optional has
          // to be left out rather than written as null or as an empty string.
          if (value.role.has_value()) {
            root["role"] = models::to_string(*value.role);
          }
          if (value.display_name.has_value()) {
            root["display_name"] = *value.display_name;
          }
          if (value.password.has_value()) {
            root["password"] = *value.password;
          }
        } else if constexpr (std::is_same_v<T, DeleteUser>) {
          root["user_id"] = value.user_id;
        } else if constexpr (std::is_same_v<T, RoomList>) {
          json rooms = json::array();
          for (const RoomSummary& summary : value.rooms) {
            rooms.push_back(room_summary_to_json(summary));
          }
          root["rooms"] = std::move(rooms);
        } else if constexpr (std::is_same_v<T, DeleteRoom>) {
          root["room_id"] = value.room_id;
        } else if constexpr (std::is_same_v<T, ListAudit>) {
          root["limit"] = value.limit;
          root["actor_id"] = value.actor_id;
        } else if constexpr (std::is_same_v<T, AuditList>) {
          json entries = json::array();
          for (const models::AuditEntry& entry : value.entries) {
            entries.push_back(audit_entry_to_json(entry));
          }
          root["entries"] = std::move(entries);
        } else {
          root["nonce"] = value.nonce;
        }
      },
      message);

  return root.dump();
}

Result<Message> parse(std::string_view json_text) {
  const json root = json::parse(json_text, nullptr, false);
  if (root.is_discarded()) {
    return Result<Message>::failure("invalid_json", "payload is not valid JSON");
  }
  if (!root.is_object()) {
    return Result<Message>::failure("invalid_json", "payload must be a JSON object");
  }

  const auto type_field = root.find("type");
  if (type_field == root.end()) {
    return Result<Message>::failure("missing_field", "type is required");
  }
  if (!type_field->is_string()) {
    return Result<Message>::failure("invalid_type", "type expected a string");
  }

  const auto type = type_from_name(type_field->get<std::string>());
  if (!type.has_value()) {
    return Result<Message>::failure("unknown_message_type",
                                    "unknown type: " + type_field->get<std::string>());
  }

  FieldReader reader(root);

  switch (*type) {
    case MessageType::Authenticate: {
      Authenticate value;
      value.username = reader.string("username");
      value.password = reader.string("password");
      return finish(reader, value);
    }
    case MessageType::Authenticated: {
      Authenticated value;
      value.user = reader.user("user");
      value.token = reader.string("token");
      value.expires_in_seconds = reader.integer("expires_in_seconds");
      return finish(reader, value);
    }
    case MessageType::CreateRoom: {
      CreateRoom value;
      value.user_id = reader.string("user_id");
      value.room_name = reader.optional_string("room_name");
      value.persistent = reader.boolean("persistent");
      return finish(reader, value);
    }
    case MessageType::RoomCreated: {
      RoomCreated value;
      value.room_id = reader.string("room_id");
      value.room_name = reader.optional_string("room_name");
      return finish(reader, value);
    }
    case MessageType::JoinRoom: {
      JoinRoom value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
      value.display_name = reader.optional_string("display_name");
      return finish(reader, value);
    }
    case MessageType::LeaveRoom: {
      LeaveRoom value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
      return finish(reader, value);
    }
    case MessageType::UserJoined: {
      UserJoined value;
      value.room_id = reader.string("room_id");
      value.user = reader.user("user");
      return finish(reader, value);
    }
    case MessageType::UserLeft: {
      UserLeft value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
      return finish(reader, value);
    }
    case MessageType::Offer: {
      Offer value;
      value.room_id = reader.string("room_id");
      value.from_user_id = reader.string("from_user_id");
      value.to_user_id = reader.string("to_user_id");
      value.sdp = reader.string("sdp");
      return finish(reader, value);
    }
    case MessageType::Answer: {
      Answer value;
      value.room_id = reader.string("room_id");
      value.from_user_id = reader.string("from_user_id");
      value.to_user_id = reader.string("to_user_id");
      value.sdp = reader.string("sdp");
      return finish(reader, value);
    }
    case MessageType::IceCandidate: {
      IceCandidate value;
      value.room_id = reader.string("room_id");
      value.from_user_id = reader.string("from_user_id");
      value.to_user_id = reader.string("to_user_id");
      value.candidate = reader.string("candidate");
      value.sdp_mid = reader.string("sdp_mid");
      value.sdp_mline_index = reader.integer("sdp_mline_index");
      return finish(reader, value);
    }
    case MessageType::ScreenShareStarted: {
      ScreenShareStarted value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
      // With a fallback rather than required: a peer built before the screen
      // could carry sound does not send the field, and false is what it means.
      value.has_audio = reader.boolean("has_audio", false);
      return finish(reader, value);
    }
    case MessageType::ScreenShareStopped: {
      ScreenShareStopped value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
      return finish(reader, value);
    }
    case MessageType::Mute: {
      Mute value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
      value.by_user_id = reader.optional_string("by_user_id");
      return finish(reader, value);
    }
    case MessageType::ChangePassword: {
      ChangePassword value;
      // Both required. An absent new_password read as an empty string would be
      // refused by the authenticator anyway, but "you did not send the field"
      // and "you asked for an empty password" are different mistakes and the
      // sender is told which one they made.
      value.current_password = reader.string("current_password");
      value.new_password = reader.string("new_password");
      return finish(reader, value);
    }
    case MessageType::PasswordChanged:
      return finish(reader, PasswordChanged{});
    case MessageType::Unmute: {
      Unmute value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
      value.by_user_id = reader.optional_string("by_user_id");
      return finish(reader, value);
    }
    case MessageType::ChatMessage: {
      ChatMessage value;
      value.message = reader.object<models::ChatMessage>("message", chat_message_from);
      return finish(reader, value);
    }
    case MessageType::ListChat: {
      ListChat value;
      value.room_id = reader.string("room_id");
      value.limit = static_cast<int>(reader.optional_integer("limit"));
      return finish(reader, value);
    }
    case MessageType::ChatHistory: {
      ChatHistory value;
      value.room_id = reader.string("room_id");
      value.messages = reader.array<models::ChatMessage>("messages", chat_message_from);
      return finish(reader, value);
    }
    case MessageType::Error: {
      ErrorMessage value;
      value.code = reader.string("code");
      value.message = reader.optional_string("message");
      return finish(reader, value);
    }
    case MessageType::Ping: {
      Ping value;
      value.nonce = reader.optional_string("nonce");
      return finish(reader, value);
    }
    case MessageType::Pong: {
      Pong value;
      value.nonce = reader.optional_string("nonce");
      return finish(reader, value);
    }
    case MessageType::KickUser: {
      KickUser value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
      value.reason = reader.optional_string("reason");
      return finish(reader, value);
    }
    case MessageType::UserKicked: {
      UserKicked value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
      value.reason = reader.optional_string("reason");
      return finish(reader, value);
    }
    case MessageType::ForceMute: {
      ForceMute value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
      value.muted = reader.require_boolean("muted");
      return finish(reader, value);
    }
    case MessageType::RestrictUser: {
      RestrictUser value;
      value.user_id = reader.string("user_id");
      value.banned = reader.maybe_boolean("banned");
      value.muted = reader.maybe_boolean("muted");
      value.silenced = reader.maybe_boolean("silenced");
      value.screen_share_blocked = reader.maybe_boolean("screen_share_blocked");
      value.reason = reader.optional_string("reason");
      return finish(reader, value);
    }
    case MessageType::UserRestricted: {
      UserRestricted value;
      value.user_id = reader.string("user_id");
      value.restrictions = reader.restrictions("restrictions");
      value.by_user_id = reader.optional_string("by_user_id");
      value.reason = reader.optional_string("reason");
      value.room_id = reader.optional_string("room_id");
      return finish(reader, value);
    }
    case MessageType::ListUsers:
      return finish(reader, ListUsers{});
    case MessageType::UserList: {
      UserList value;
      value.users = reader.array<UserSummary>("users", user_summary_from);
      return finish(reader, value);
    }
    case MessageType::CreateUser: {
      CreateUser value;
      value.username = reader.string("username");
      value.password = reader.string("password");
      value.display_name = reader.optional_string("display_name");
      value.role = models::role_from_string(reader.optional_string("role"));
      return finish(reader, value);
    }
    case MessageType::UpdateUser: {
      UpdateUser value;
      value.user_id = reader.string("user_id");
      if (const auto role = reader.maybe_string("role")) {
        value.role = models::role_from_string(*role);
      }
      value.display_name = reader.maybe_string("display_name");
      value.password = reader.maybe_string("password");
      return finish(reader, value);
    }
    case MessageType::DeleteUser: {
      DeleteUser value;
      value.user_id = reader.string("user_id");
      return finish(reader, value);
    }
    case MessageType::ListRooms:
      return finish(reader, ListRooms{});
    case MessageType::RoomList: {
      RoomList value;
      value.rooms = reader.array<RoomSummary>("rooms", room_summary_from);
      return finish(reader, value);
    }
    case MessageType::DeleteRoom: {
      DeleteRoom value;
      value.room_id = reader.string("room_id");
      return finish(reader, value);
    }
    case MessageType::ListAudit: {
      ListAudit value;
      value.limit = static_cast<int>(reader.optional_integer("limit"));
      value.actor_id = reader.optional_string("actor_id");
      return finish(reader, value);
    }
    case MessageType::AuditList: {
      AuditList value;
      value.entries = reader.array<models::AuditEntry>("entries", audit_entry_from);
      return finish(reader, value);
    }
  }

  return Result<Message>::failure("unknown_message_type", "unhandled message type");
}

}  // namespace dv::protocol
