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
constexpr std::array<TypeMapping, 18> kTypeMappings{{
    {MessageType::Authenticate, "authenticate"},
    {MessageType::Authenticated, "authenticated"},
    {MessageType::CreateRoom, "create_room"},
    {MessageType::RoomCreated, "room_created"},
    {MessageType::JoinRoom, "join_room"},
    {MessageType::LeaveRoom, "leave_room"},
    {MessageType::UserJoined, "user_joined"},
    {MessageType::UserLeft, "user_left"},
    {MessageType::Offer, "offer"},
    {MessageType::Answer, "answer"},
    {MessageType::IceCandidate, "ice_candidate"},
    {MessageType::ScreenShareStarted, "screen_share_started"},
    {MessageType::ScreenShareStopped, "screen_share_stopped"},
    {MessageType::Mute, "mute"},
    {MessageType::Unmute, "unmute"},
    {MessageType::Error, "error"},
    {MessageType::Ping, "ping"},
    {MessageType::Pong, "pong"},
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

  std::string optional_string(std::string_view key, std::string fallback = {}) {
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
    if (!nested.ok()) {
      fail(nested.error().code, key, nested.error().message);
    }
    return value;
  }

  [[nodiscard]] bool ok() const noexcept { return !error_.has_value(); }
  [[nodiscard]] const Error& error() const { return *error_; }

 private:
  const json* find(std::string_view key) const {
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
    error_ = Error{std::string(code), std::string(key) + " " + std::string(reason)};
  }

  const json& object_;
  std::optional<Error> error_;
};

json user_to_json(const models::User& user) {
  return json{{"id", user.id}, {"display_name", user.display_name}, {"avatar", user.avatar}};
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

MessageType type_of(const Message& message) noexcept {
  return std::visit(
      [](const auto& value) -> MessageType {
        using T = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<T, Authenticate>) return MessageType::Authenticate;
        if constexpr (std::is_same_v<T, Authenticated>) return MessageType::Authenticated;
        if constexpr (std::is_same_v<T, CreateRoom>) return MessageType::CreateRoom;
        if constexpr (std::is_same_v<T, RoomCreated>) return MessageType::RoomCreated;
        if constexpr (std::is_same_v<T, JoinRoom>) return MessageType::JoinRoom;
        if constexpr (std::is_same_v<T, LeaveRoom>) return MessageType::LeaveRoom;
        if constexpr (std::is_same_v<T, UserJoined>) return MessageType::UserJoined;
        if constexpr (std::is_same_v<T, UserLeft>) return MessageType::UserLeft;
        if constexpr (std::is_same_v<T, Offer>) return MessageType::Offer;
        if constexpr (std::is_same_v<T, Answer>) return MessageType::Answer;
        if constexpr (std::is_same_v<T, IceCandidate>) return MessageType::IceCandidate;
        if constexpr (std::is_same_v<T, ScreenShareStarted>) return MessageType::ScreenShareStarted;
        if constexpr (std::is_same_v<T, ScreenShareStopped>) return MessageType::ScreenShareStopped;
        if constexpr (std::is_same_v<T, Mute>) return MessageType::Mute;
        if constexpr (std::is_same_v<T, Unmute>) return MessageType::Unmute;
        if constexpr (std::is_same_v<T, ErrorMessage>) return MessageType::Error;
        if constexpr (std::is_same_v<T, Ping>) return MessageType::Ping;
        return MessageType::Pong;
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
        } else if constexpr (std::is_same_v<T, RoomCreated>) {
          root["room_id"] = value.room_id;
          root["room_name"] = value.room_name;
        } else if constexpr (std::is_same_v<T, JoinRoom>) {
          root["room_id"] = value.room_id;
          root["user_id"] = value.user_id;
          root["display_name"] = value.display_name;
        } else if constexpr (std::is_same_v<T, LeaveRoom>) {
          root["room_id"] = value.room_id;
          root["user_id"] = value.user_id;
        } else if constexpr (std::is_same_v<T, UserJoined>) {
          root["room_id"] = value.room_id;
          root["user"] = user_to_json(value.user);
        } else if constexpr (std::is_same_v<T, UserLeft>) {
          root["room_id"] = value.room_id;
          root["user_id"] = value.user_id;
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
        } else if constexpr (std::is_same_v<T, ScreenShareStarted> ||
                             std::is_same_v<T, ScreenShareStopped> || std::is_same_v<T, Mute> ||
                             std::is_same_v<T, Unmute>) {
          root["room_id"] = value.room_id;
          root["user_id"] = value.user_id;
        } else if constexpr (std::is_same_v<T, ErrorMessage>) {
          root["code"] = value.code;
          root["message"] = value.message;
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
      return finish(reader, value);
    }
    case MessageType::Unmute: {
      Unmute value;
      value.room_id = reader.string("room_id");
      value.user_id = reader.string("user_id");
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
  }

  return Result<Message>::failure("unknown_message_type", "unhandled message type");
}

}  // namespace dv::protocol
