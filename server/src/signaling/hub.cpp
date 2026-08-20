#include "signaling/hub.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include <dv/logging/logger.hpp>

#include "signaling/permissions.hpp"
#include "store/memory_store.hpp"

namespace dv::server {
namespace {

Error unauthorized(std::string message) {
  return Error{.code = "unauthorized", .message = std::move(message)};
}

/// Told to everyone still in a room that an administrator is closing.
constexpr std::string_view kRoomClosed = "the room was closed by an administrator";

}  // namespace

Hub::Hub() : Hub(Options{}) {}

Hub::Hub(Options options)
    : options_(options),
      // A store the caller did not provide is created here and owned here, so
      // that a Hub built with no options at all is the server this project had
      // before there was a database: accounts and rooms in memory, and an
      // audit log that goes away with the process.
      owned_users_(options.users == nullptr ? std::make_unique<store::MemoryUserStore>() : nullptr),
      owned_rooms_(options.rooms == nullptr ? std::make_unique<store::MemoryRoomStore>() : nullptr),
      owned_audit_(options.audit == nullptr ? std::make_unique<store::MemoryAuditLog>() : nullptr),
      users_(options.users != nullptr ? options.users : owned_users_.get()),
      audit_(options.audit != nullptr ? options.audit : owned_audit_.get()),
      rooms_(RoomManager::Options{
          .max_participants_per_room = options.max_participants_per_room,
          .id_seed = options.room_id_seed,
          .store = options.rooms != nullptr ? options.rooms : owned_rooms_.get()}),
      authenticator_(Authenticator::Options{}, *users_) {
  // Rooms somebody wrote down have to still exist after a restart.
  if (const std::size_t loaded = rooms_.load_persistent(); loaded > 0) {
    DV_LOG_INFO("Loaded {} persistent room(s)", loaded);
  }
}

models::Role Hub::current_role(const std::string& user_id) const {
  const auto account = users_->find_by_id(user_id);
  return account.has_value() ? account->user.role : models::Role::User;
}

void Hub::record(const models::User& actor, std::string action, std::string target_id,
                 std::string room_id, std::string detail) {
  models::AuditEntry entry;
  entry.actor_id = actor.id;
  // The username, which is unique, rather than the display name, which two
  // accounts may share. A log that cannot say which of two people calling
  // themselves "Ana" did something has lost what it was written for. The
  // display name is the fallback for an account that has just been deleted and
  // has nothing left to look up.
  const auto account = users_->find_by_id(actor.id);
  entry.actor_username = account.has_value() ? account->username : actor.display_name;
  entry.action = std::move(action);
  entry.target_id = std::move(target_id);
  entry.room_id = std::move(room_id);
  entry.detail = std::move(detail);

  // Copied before the move, because the failure message below needs it and the
  // entry is gone by then.
  const std::string action_name = entry.action;
  if (auto failure = audit_->append(std::move(entry))) {
    // Error level and not warning: an administrative action that happened and
    // was not recorded is the exact hole an audit log exists to close, and
    // whoever reads the logs has to see it.
    DV_LOG_ERROR("Audit entry for '{}' by {} was not written: {}", action_name, actor.id,
                 failure->message);
  }
}

void Hub::on_connect(ConnectionId connection, Clock::time_point now) {
  Connection state;
  state.id = connection;
  state.last_seen = now;
  state.last_ping = now;
  connections_.insert_or_assign(connection, std::move(state));
  DV_LOG_DEBUG("Connection {} opened", connection);
}

Hub::Connection* Hub::find_connection(ConnectionId connection) {
  const auto it = connections_.find(connection);
  return it == connections_.end() ? nullptr : &it->second;
}

std::optional<ConnectionId> Hub::connection_for_user(const std::string& user_id) const {
  return connection_of_user(user_id);
}

std::optional<ConnectionId> Hub::connection_of_user(const std::string& user_id) const {
  const auto it = user_to_connection_.find(user_id);
  if (it == user_to_connection_.end()) {
    return std::nullopt;
  }
  return it->second;
}

void Hub::broadcast(std::vector<Outgoing>& out, const std::string& room_id,
                    const protocol::Message& message,
                    const std::optional<std::string>& except_user_id) const {
  const models::Room* room = rooms_.find(room_id);
  if (room == nullptr) {
    return;
  }
  for (const models::Participant& participant : room->participants) {
    if (except_user_id.has_value() && participant.user.id == *except_user_id) {
      continue;
    }
    if (const auto target = connection_of_user(participant.user.id)) {
      out.push_back(Outgoing{.connection = *target, .message = message});
    }
  }
}

void Hub::reply_error(std::vector<Outgoing>& out, ConnectionId connection, const Error& error) {
  out.push_back(
      Outgoing{.connection = connection,
               .message = protocol::ErrorMessage{.code = error.code, .message = error.message}});
}

std::vector<Outgoing> Hub::on_message(ConnectionId connection, std::string_view payload,
                                      Clock::time_point now) {
  std::vector<Outgoing> out;

  Connection* state = find_connection(connection);
  if (state == nullptr) {
    // A frame from a connection we never saw open. Nothing to answer to.
    DV_LOG_WARN("Message from unknown connection {}", connection);
    return out;
  }
  state->last_seen = now;

  auto parsed = protocol::parse(payload);
  if (!parsed) {
    reply_error(out, connection, parsed.error());
    return out;
  }
  const protocol::Message message = std::move(parsed).take();

  // Authentication is the only thing an unauthenticated connection may do.
  if (!state->user.has_value() && !std::holds_alternative<protocol::Authenticate>(message)) {
    reply_error(out, connection, unauthorized("authenticate before sending anything else"));
    return out;
  }

  // One gate for the whole protocol, from the table in permissions.hpp. Doing
  // it here rather than in each handler is what makes a handler added later
  // without a check impossible to write: the table covers the enum, and a type
  // nobody classified does not compile.
  const protocol::MessageType type = protocol::type_of(message);
  switch (access_for(type)) {
    case Access::ServerToClient:
      // Messages the server only ever sends. Receiving one is a client bug.
      reply_error(out, connection,
                  Error{.code = "unknown_message_type",
                        .message = "this message is server to client only"});
      return out;

    case Access::AdminOnly: {
      // Unreachable without an identity: the only message an unauthenticated
      // connection may send is `authenticate`, which is not administrative.
      // Checked all the same, because that reasoning lives in a table in
      // another file and what it costs to be wrong here is an empty optional
      // dereferenced.
      if (!state->user.has_value()) {
        reply_error(out, connection, unauthorized("authenticate before sending anything else"));
        return out;
      }

      // The authority on the role is the store, not what this connection was
      // when it logged in. The cached identity is brought back into step at
      // the same time, so the handlers and the audit entry agree with it.
      const std::string user_id = state->user->id;
      const models::Role role = current_role(user_id);
      state->user->role = role;
      if (!is_allowed(role, type)) {
        DV_LOG_WARN("Connection {} ({}) attempted '{}' without administrator rights", connection,
                    user_id, protocol::type_name(type));
        reply_error(out, connection,
                    Error{.code = "forbidden", .message = "this action requires an administrator"});
        return out;
      }
      break;
    }

    case Access::Authenticated:
      break;
  }

  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;

        if constexpr (std::is_same_v<T, protocol::Authenticate>) {
          handle_authenticate(out, *state, value, now);

        } else if constexpr (std::is_same_v<T, protocol::CreateRoom>) {
          handle_create_room(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::JoinRoom>) {
          handle_join_room(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::LeaveRoom>) {
          handle_leave_room(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::Offer> ||
                             std::is_same_v<T, protocol::Answer> ||
                             std::is_same_v<T, protocol::IceCandidate>) {
          handle_relay(out, *state, message, value.room_id, value.from_user_id, value.to_user_id);

        } else if constexpr (std::is_same_v<T, protocol::Mute>) {
          handle_mute(out, *state, value.room_id, value.user_id, true);

        } else if constexpr (std::is_same_v<T, protocol::Unmute>) {
          handle_mute(out, *state, value.room_id, value.user_id, false);

        } else if constexpr (std::is_same_v<T, protocol::ScreenShareStarted>) {
          handle_screen_share(out, *state, value.room_id, value.user_id, true);

        } else if constexpr (std::is_same_v<T, protocol::ScreenShareStopped>) {
          handle_screen_share(out, *state, value.room_id, value.user_id, false);

        } else if constexpr (std::is_same_v<T, protocol::Ping>) {
          out.push_back(Outgoing{.connection = state->id, .message = protocol::Pong{value.nonce}});

        } else if constexpr (std::is_same_v<T, protocol::Pong>) {
          // last_seen was already refreshed above, which is the whole point.

        } else if constexpr (std::is_same_v<T, protocol::KickUser>) {
          handle_kick(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::ForceMute>) {
          handle_force_mute(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::ListUsers>) {
          handle_list_users(out, *state);

        } else if constexpr (std::is_same_v<T, protocol::CreateUser>) {
          handle_create_user(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::UpdateUser>) {
          handle_update_user(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::DeleteUser>) {
          handle_delete_user(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::ListRooms>) {
          handle_list_rooms(out, *state);

        } else if constexpr (std::is_same_v<T, protocol::DeleteRoom>) {
          handle_delete_room(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::ListAudit>) {
          handle_list_audit(out, *state, value);

        } else {
          // Unreachable: the gate above already answered every server to
          // client type. Kept so that adding an alternative to the variant and
          // forgetting to dispatch it is a refusal rather than silence.
          reply_error(out, state->id,
                      Error{.code = "unknown_message_type",
                            .message = "this message is server to client only"});
        }
      },
      message);

  return out;
}

void Hub::handle_authenticate(std::vector<Outgoing>& out, Connection& connection,
                              const protocol::Authenticate& message, Clock::time_point now) {
  auto session = authenticator_.authenticate(message.username, message.password, now);
  if (!session) {
    // The username is logged, never the password.
    DV_LOG_WARN("Failed authentication for '{}' on connection {}", message.username, connection.id);
    reply_error(out, connection.id, session.error());
    return;
  }

  const Authenticator::Session value = std::move(session).take();

  // One identity, one connection. A second login kicks the first, so a stale
  // socket cannot keep receiving another person's media negotiation.
  if (const auto previous = connection_of_user(value.user.id);
      previous.has_value() && *previous != connection.id) {
    if (Connection* stale = find_connection(*previous)) {
      stale->user.reset();
      stale->room_id.reset();
    }
  }

  connection.user = value.user;
  user_to_connection_[value.user.id] = connection.id;

  DV_LOG_INFO("User {} authenticated on connection {}", value.user.id, connection.id);
  out.push_back(
      Outgoing{.connection = connection.id,
               .message = protocol::Authenticated{.user = value.user,
                                                  .token = value.token,
                                                  .expires_in_seconds = value.expires_in_seconds}});
}

models::User* Hub::authenticated(std::vector<Outgoing>& out, Connection& connection) {
  // The dispatcher refuses everything but `authenticate` before a user exists,
  // so this never fails in practice. Checking anyway is what keeps the
  // invariant local: a handler should not have to know what the dispatcher
  // does in order to be correct, and the day one is called from somewhere else
  // this is a refusal rather than a crash on a null optional.
  if (!connection.user.has_value()) {
    reply_error(out, connection.id, unauthorized("authenticate before sending anything else"));
    return nullptr;
  }
  return &*connection.user;
}

void Hub::handle_create_room(std::vector<Outgoing>& out, Connection& connection,
                             const protocol::CreateRoom& message) {
  const models::User* user = authenticated(out, connection);
  if (user == nullptr) {
    return;
  }
  if (message.user_id != user->id) {
    reply_error(out, connection.id, unauthorized("user_id does not match this session"));
    return;
  }

  // Creating a room is open to everybody; creating one that outlives its last
  // participant is not, because a room nobody can be bothered to close is a
  // room that accumulates. Refused rather than downgraded to an ordinary room:
  // a caller that asked for permanence and silently got the opposite would
  // find out only when the identifier stopped working.
  if (message.persistent && current_role(user->id) != models::Role::Admin) {
    reply_error(out, connection.id,
                Error{.code = "forbidden",
                      .message = "only an administrator can create a persistent room"});
    return;
  }

  auto created = rooms_.create_room(message.room_name, user->id, message.persistent);
  if (!created) {
    reply_error(out, connection.id, created.error());
    return;
  }
  const std::string room_id = std::move(created).take();

  if (message.persistent) {
    record(*user, "create_room", room_id, room_id, "persistent");
  }
  DV_LOG_INFO("Room {} created by {}{}", room_id, user->id,
              message.persistent ? " (persistent)" : "");
  out.push_back(Outgoing{
      .connection = connection.id,
      .message = protocol::RoomCreated{.room_id = room_id, .room_name = message.room_name}});
}

void Hub::handle_join_room(std::vector<Outgoing>& out, Connection& connection,
                           const protocol::JoinRoom& message) {
  models::User* authenticated_user = authenticated(out, connection);
  if (authenticated_user == nullptr) {
    return;
  }
  if (message.user_id != authenticated_user->id) {
    reply_error(out, connection.id, unauthorized("user_id does not match this session"));
    return;
  }

  models::User user = *authenticated_user;
  if (!message.display_name.empty()) {
    user.display_name = message.display_name;
  }

  if (const auto failure = rooms_.join(message.room_id, user)) {
    reply_error(out, connection.id, *failure);
    return;
  }
  connection.room_id = message.room_id;
  authenticated_user->display_name = user.display_name;

  const models::Room* room = rooms_.find(message.room_id);

  // Everyone already in the room, then the joiner themselves. The client uses
  // its own user_joined as the signal that the initial state is complete, so it
  // has to arrive last. See section 7 of docs/protocol.md.
  for (const models::Participant& participant : room->participants) {
    if (participant.user.id == user.id) {
      continue;
    }
    out.push_back(Outgoing{
        .connection = connection.id,
        .message = protocol::UserJoined{.room_id = message.room_id, .user = participant.user}});
  }
  out.push_back(
      Outgoing{.connection = connection.id,
               .message = protocol::UserJoined{.room_id = message.room_id, .user = user}});

  // A screen share already in progress has to be announced to the newcomer.
  if (const models::Participant* sharer = room->screen_sharer()) {
    out.push_back(Outgoing{.connection = connection.id,
                           .message = protocol::ScreenShareStarted{.room_id = message.room_id,
                                                                   .user_id = sharer->user.id}});
  }

  broadcast(out, message.room_id, protocol::UserJoined{.room_id = message.room_id, .user = user},
            user.id);
  DV_LOG_INFO("User {} joined room {} ({} participants)", user.id, message.room_id, room->size());

  // Last, so that the participant already knows who is in the room by the time
  // the SFU starts negotiating media with them.
  if (media_signals_ != nullptr) {
    media_signals_->on_participant_joined(message.room_id, user);
  }
}

void Hub::handle_leave_room(std::vector<Outgoing>& out, Connection& connection,
                            const protocol::LeaveRoom& message) {
  const models::User* user = authenticated(out, connection);
  if (user == nullptr) {
    return;
  }
  if (message.user_id != user->id) {
    reply_error(out, connection.id, unauthorized("user_id does not match this session"));
    return;
  }

  const models::Room* room = rooms_.find(message.room_id);
  const bool was_sharing = room != nullptr && room->find(message.user_id) != nullptr &&
                           room->find(message.user_id)->sharing_screen;

  if (const auto failure = rooms_.leave(message.room_id, message.user_id)) {
    reply_error(out, connection.id, *failure);
    return;
  }
  connection.room_id.reset();

  if (was_sharing) {
    broadcast(out, message.room_id,
              protocol::ScreenShareStopped{.room_id = message.room_id, .user_id = message.user_id});
  }
  broadcast(out, message.room_id,
            protocol::UserLeft{.room_id = message.room_id, .user_id = message.user_id});
  DV_LOG_INFO("User {} left room {}", message.user_id, message.room_id);

  if (media_signals_ != nullptr) {
    media_signals_->on_participant_left(message.room_id, message.user_id);
  }
}

void Hub::handle_relay(std::vector<Outgoing>& out, Connection& connection,
                       const protocol::Message& message, const std::string& room_id,
                       const std::string& from_user_id, const std::string& to_user_id) {
  const models::User* user = authenticated(out, connection);
  if (user == nullptr) {
    return;
  }
  if (from_user_id != user->id) {
    reply_error(out, connection.id, unauthorized("from_user_id does not match this session"));
    return;
  }

  const models::Room* room = rooms_.find(room_id);
  if (room == nullptr) {
    reply_error(out, connection.id,
                Error{.code = "room_not_found", .message = "no room with id " + room_id});
    return;
  }
  if (!room->contains(from_user_id)) {
    reply_error(out, connection.id,
                Error{.code = "not_in_room", .message = "sender is not in " + room_id});
    return;
  }

  // Media is routed through the SFU, so a frame addressed to it is negotiation
  // with the server itself rather than something to hand to another
  // participant. See protocol::kSfuUserId.
  if (to_user_id == protocol::kSfuUserId) {
    if (media_signals_ == nullptr) {
      reply_error(
          out, connection.id,
          Error{.code = "media_unavailable", .message = "this server is not routing media"});
      return;
    }
    media_signals_->on_media_signal(room_id, from_user_id, message);
    return;
  }

  if (!room->contains(to_user_id)) {
    reply_error(out, connection.id,
                Error{.code = "not_in_room", .message = "recipient is not in " + room_id});
    return;
  }

  const auto target = connection_of_user(to_user_id);
  if (!target.has_value()) {
    reply_error(out, connection.id,
                Error{.code = "not_in_room", .message = "recipient is not connected"});
    return;
  }

  // The server forwards the frame untouched. It never inspects SDP or ICE.
  out.push_back(Outgoing{.connection = *target, .message = message});
}

void Hub::handle_mute(std::vector<Outgoing>& out, Connection& connection,
                      const std::string& room_id, const std::string& user_id, bool muted) {
  const models::User* user = authenticated(out, connection);
  if (user == nullptr) {
    return;
  }
  if (user_id != user->id) {
    reply_error(out, connection.id, unauthorized("user_id does not match this session"));
    return;
  }
  if (const auto failure = rooms_.set_muted(room_id, user_id, muted)) {
    reply_error(out, connection.id, *failure);
    return;
  }

  // Confirmed to everyone, the sender included: clients only update their UI
  // once the server has agreed.
  if (muted) {
    broadcast(out, room_id, protocol::Mute{.room_id = room_id, .user_id = user_id});
  } else {
    broadcast(out, room_id, protocol::Unmute{.room_id = room_id, .user_id = user_id});
  }
}

void Hub::handle_screen_share(std::vector<Outgoing>& out, Connection& connection,
                              const std::string& room_id, const std::string& user_id,
                              bool sharing) {
  const models::User* user = authenticated(out, connection);
  if (user == nullptr) {
    return;
  }
  if (user_id != user->id) {
    reply_error(out, connection.id, unauthorized("user_id does not match this session"));
    return;
  }

  const auto failure = sharing ? rooms_.start_screen_share(room_id, user_id)
                               : rooms_.stop_screen_share(room_id, user_id);
  if (failure) {
    reply_error(out, connection.id, *failure);
    return;
  }

  if (sharing) {
    DV_LOG_INFO("User {} started sharing in room {}", user_id, room_id);
    broadcast(out, room_id, protocol::ScreenShareStarted{.room_id = room_id, .user_id = user_id});
  } else {
    DV_LOG_INFO("User {} stopped sharing in room {}", user_id, room_id);
    broadcast(out, room_id, protocol::ScreenShareStopped{.room_id = room_id, .user_id = user_id});
  }
}

std::vector<Outgoing> Hub::on_disconnect(ConnectionId connection, Clock::time_point /*now*/) {
  std::vector<Outgoing> out;

  const auto it = connections_.find(connection);
  if (it == connections_.end()) {
    return out;
  }
  const Connection state = it->second;
  connections_.erase(it);

  if (!state.user.has_value()) {
    return out;
  }

  // Only drop the user to connection mapping when it still points here. A newer
  // login for the same identity must not be unregistered by an old socket
  // closing afterwards.
  if (const auto current = connection_of_user(state.user->id);
      current.has_value() && *current == connection) {
    user_to_connection_.erase(state.user->id);
  }

  const std::string user_id = state.user->id;
  const auto room_id = rooms_.room_of(user_id);
  if (!room_id.has_value()) {
    return out;
  }

  const models::Room* room = rooms_.find(*room_id);
  const bool was_sharing =
      room != nullptr && room->find(user_id) != nullptr && room->find(user_id)->sharing_screen;

  (void)rooms_.remove_from_any_room(user_id);

  if (was_sharing) {
    broadcast(out, *room_id, protocol::ScreenShareStopped{.room_id = *room_id, .user_id = user_id});
  }
  broadcast(out, *room_id, protocol::UserLeft{.room_id = *room_id, .user_id = user_id});

  if (media_signals_ != nullptr) {
    media_signals_->on_participant_left(*room_id, user_id);
  }

  DV_LOG_INFO("Connection {} closed, user {} removed from room {}", connection, user_id, *room_id);
  return out;
}

std::vector<Outgoing> Hub::tick(Clock::time_point now, std::vector<ConnectionId>& timed_out) {
  std::vector<Outgoing> out;

  for (auto& [id, state] : connections_) {
    if (now - state.last_seen >= options_.heartbeat_timeout) {
      timed_out.push_back(id);
      continue;
    }
    if (now - state.last_ping >= options_.heartbeat_interval) {
      state.last_ping = now;
      out.push_back(Outgoing{.connection = id, .message = protocol::Ping{}});
    }
  }

  authenticator_.expire_tokens(now);
  return out;
}

// --- administration ----------------------------------------------------------

void Hub::evict(std::vector<Outgoing>& out, const std::string& room_id, const std::string& user_id,
                const std::string& reason) {
  // Read before the participant is gone, and announced after, exactly as the
  // two other ways out of a room do it. Without this every remaining client
  // keeps showing a share that ended, and refuses to start its own because it
  // still believes the floor is taken.
  const models::Room* room = rooms_.find(room_id);
  const bool was_sharing =
      room != nullptr && room->find(user_id) != nullptr && room->find(user_id)->sharing_screen;

  // Announced before the room forgets them, because broadcast walks the
  // participant list: afterwards the person being removed is no longer on it,
  // and would be the one participant never told.
  broadcast(out, room_id,
            protocol::UserKicked{.room_id = room_id, .user_id = user_id, .reason = reason});

  (void)rooms_.leave(room_id, user_id);

  if (was_sharing) {
    broadcast(out, room_id, protocol::ScreenShareStopped{.room_id = room_id, .user_id = user_id});
  }

  // The same two things a voluntary leave does, so a kicked participant and one
  // who left are indistinguishable to everybody else's client and to the SFU.
  broadcast(out, room_id, protocol::UserLeft{.room_id = room_id, .user_id = user_id});
  if (media_signals_ != nullptr) {
    media_signals_->on_participant_left(room_id, user_id);
  }

  if (const auto target = connection_of_user(user_id)) {
    if (Connection* state = find_connection(*target)) {
      state->room_id.reset();
    }
  }
}

void Hub::handle_kick(std::vector<Outgoing>& out, Connection& connection,
                      const protocol::KickUser& message) {
  const models::User* actor = authenticated(out, connection);
  if (actor == nullptr) {
    return;
  }

  const models::Room* room = rooms_.find(message.room_id);
  if (room == nullptr) {
    reply_error(out, connection.id,
                Error{.code = "room_not_found", .message = "no room with id " + message.room_id});
    return;
  }
  if (!room->contains(message.user_id)) {
    reply_error(
        out, connection.id,
        Error{.code = "not_in_room", .message = message.user_id + " is not in " + message.room_id});
    return;
  }
  // An administrator removing themselves is almost certainly a misclick on the
  // wrong row, and leaving is what the ordinary button is for.
  if (message.user_id == actor->id) {
    reply_error(out, connection.id,
                Error{.code = "invalid_target",
                      .message = "use leave_room to remove yourself from a room"});
    return;
  }

  DV_LOG_INFO("{} kicked {} from room {}", actor->id, message.user_id, message.room_id);
  record(*actor, "kick", message.user_id, message.room_id, message.reason);
  evict(out, message.room_id, message.user_id, message.reason);
}

void Hub::handle_force_mute(std::vector<Outgoing>& out, Connection& connection,
                            const protocol::ForceMute& message) {
  const models::User* actor = authenticated(out, connection);
  if (actor == nullptr) {
    return;
  }
  if (const auto failure =
          rooms_.set_muted(message.room_id, message.user_id, message.muted, true)) {
    reply_error(out, connection.id, *failure);
    return;
  }

  record(*actor, message.muted ? "force_mute" : "force_unmute", message.user_id, message.room_id,
         {});

  // Announced as an ordinary mute carrying who did it. A client that knows
  // nothing about administration still renders the microphone correctly, and
  // one that does can say whose doing it was.
  if (message.muted) {
    broadcast(out, message.room_id,
              protocol::Mute{
                  .room_id = message.room_id, .user_id = message.user_id, .by_user_id = actor->id});
  } else {
    broadcast(out, message.room_id,
              protocol::Unmute{
                  .room_id = message.room_id, .user_id = message.user_id, .by_user_id = actor->id});
  }
}

protocol::UserList Hub::user_list() const {
  protocol::UserList list;
  for (const store::Account& account : users_->list()) {
    list.users.push_back(
        protocol::UserSummary{.user = account.user,
                              .username = account.username,
                              .created_at = account.created_at,
                              .online = user_to_connection_.contains(account.user.id)});
  }
  return list;
}

protocol::RoomList Hub::room_list() const {
  protocol::RoomList list;
  for (const models::Room& room : rooms_.list()) {
    list.rooms.push_back(protocol::RoomSummary{.id = room.id,
                                               .name = room.name,
                                               .owner_id = room.owner_id,
                                               .persistent = room.persistent,
                                               .participant_count = static_cast<int>(room.size())});
  }
  // The RoomManager holds rooms in a hash map, so its order is whatever the
  // hashing produced. Sorting here means a panel that refreshes does not
  // reshuffle its rows under the cursor.
  std::ranges::sort(list.rooms, {}, &protocol::RoomSummary::id);
  return list;
}

void Hub::handle_list_users(std::vector<Outgoing>& out, Connection& connection) {
  out.push_back(Outgoing{.connection = connection.id, .message = user_list()});
}

void Hub::handle_list_rooms(std::vector<Outgoing>& out, Connection& connection) {
  out.push_back(Outgoing{.connection = connection.id, .message = room_list()});
}

void Hub::handle_create_user(std::vector<Outgoing>& out, Connection& connection,
                             const protocol::CreateUser& message) {
  const models::User* actor = authenticated(out, connection);
  if (actor == nullptr) {
    return;
  }

  auto created = authenticator_.add_user(message.username, message.password, message.display_name,
                                         message.role);
  if (!created) {
    reply_error(out, connection.id, created.error());
    return;
  }

  const models::User& user = created.value();
  DV_LOG_INFO("{} created account {} with role {}", actor->id, message.username,
              models::to_string(message.role));
  record(*actor, "create_user", user.id, {},
         "username=" + message.username + " role=" + std::string(models::to_string(message.role)));
  out.push_back(Outgoing{.connection = connection.id, .message = user_list()});
}

void Hub::handle_update_user(std::vector<Outgoing>& out, Connection& connection,
                             const protocol::UpdateUser& message) {
  const models::User* actor = authenticated(out, connection);
  if (actor == nullptr) {
    return;
  }

  auto account = users_->find_by_id(message.user_id);
  if (!account.has_value()) {
    reply_error(out, connection.id,
                Error{.code = "user_not_found", .message = "no account with that identifier"});
    return;
  }

  std::string detail;
  if (message.role.has_value() && *message.role != account->user.role) {
    // Two ways to end up with a system nobody can administer, and both are
    // refused here rather than explained afterwards.
    if (account->user.id == actor->id) {
      reply_error(out, connection.id,
                  Error{.code = "invalid_target",
                        .message = "an administrator cannot change their own role"});
      return;
    }
    if (account->user.role == models::Role::Admin &&
        users_->count_with_role(models::Role::Admin) <= 1) {
      reply_error(out, connection.id,
                  Error{.code = "last_administrator",
                        .message = "the last administrator cannot be demoted"});
      return;
    }
    account->user.role = *message.role;
    detail = "role=" + std::string(models::to_string(*message.role));
  }
  if (message.display_name.has_value()) {
    account->user.display_name = *message.display_name;
  }

  // Derived before anything is written, and applied to the same copy, so that
  // the whole change is one store write. Two writes could half succeed and
  // leave an account whose role moved and whose password did not, with the
  // administrator looking at an error and a table that says otherwise.
  if (message.password.has_value()) {
    auto credentials = authenticator_.derive(*message.password);
    if (!credentials) {
      reply_error(out, connection.id, credentials.error());
      return;
    }
    const auto value = std::move(credentials).take();
    account->salt_hex = value.salt_hex;
    account->password_hash_hex = value.password_hash_hex;
    detail += detail.empty() ? "password" : " password";
  }

  if (auto failure = users_->update(*account)) {
    reply_error(out, connection.id, *failure);
    return;
  }

  record(*actor, "update_user", message.user_id, {}, detail);
  out.push_back(Outgoing{.connection = connection.id, .message = user_list()});
}

void Hub::handle_delete_user(std::vector<Outgoing>& out, Connection& connection,
                             const protocol::DeleteUser& message) {
  const models::User* actor = authenticated(out, connection);
  if (actor == nullptr) {
    return;
  }

  const auto account = users_->find_by_id(message.user_id);
  if (!account.has_value()) {
    reply_error(out, connection.id,
                Error{.code = "user_not_found", .message = "no account with that identifier"});
    return;
  }
  if (account->user.id == actor->id) {
    reply_error(out, connection.id,
                Error{.code = "invalid_target",
                      .message = "an administrator cannot delete their own account"});
    return;
  }
  if (account->user.role == models::Role::Admin &&
      users_->count_with_role(models::Role::Admin) <= 1) {
    reply_error(
        out, connection.id,
        Error{.code = "last_administrator", .message = "the last administrator cannot be deleted"});
    return;
  }

  // Out of the room first, so nobody is left talking to an account that no
  // longer exists.
  if (const auto room_id = rooms_.room_of(message.user_id)) {
    evict(out, *room_id, message.user_id, "the account was removed");
  }

  // Tokens go with the account. Without this, a session opened a minute ago
  // keeps working until it expires on its own, which for an account somebody
  // just decided to remove is exactly the wrong answer.
  authenticator_.revoke_tokens_of(message.user_id);
  if (const auto target = connection_of_user(message.user_id)) {
    if (Connection* state = find_connection(*target)) {
      state->user.reset();
      state->room_id.reset();
    }
    user_to_connection_.erase(message.user_id);
  }

  if (auto failure = users_->remove(message.user_id)) {
    reply_error(out, connection.id, *failure);
    return;
  }

  DV_LOG_INFO("{} deleted account {}", actor->id, account->username);
  record(*actor, "delete_user", message.user_id, {}, "username=" + account->username);
  out.push_back(Outgoing{.connection = connection.id, .message = user_list()});
}

void Hub::handle_delete_room(std::vector<Outgoing>& out, Connection& connection,
                             const protocol::DeleteRoom& message) {
  const models::User* actor = authenticated(out, connection);
  if (actor == nullptr) {
    return;
  }

  // Everyone is removed one at a time, through the same path a kick takes, so
  // that each of them gets told and the SFU tears each connection down. Read
  // the identifiers out first: evict mutates the list being walked.
  std::vector<std::string> occupants;
  if (const models::Room* room = rooms_.find(message.room_id); room != nullptr) {
    occupants.reserve(room->participants.size());
    for (const models::Participant& participant : room->participants) {
      occupants.push_back(participant.user.id);
    }
  }
  for (const std::string& user_id : occupants) {
    evict(out, message.room_id, user_id, std::string(kRoomClosed));
  }

  auto removed = rooms_.remove_room(message.room_id);
  if (!removed) {
    // An ordinary room is already gone: emptying it deleted it, which is the
    // outcome that was asked for. Only a room that never existed is an error.
    if (occupants.empty()) {
      reply_error(out, connection.id, removed.error());
      return;
    }
  }

  DV_LOG_INFO("{} closed room {}", actor->id, message.room_id);
  record(*actor, "delete_room", message.room_id, message.room_id,
         "participants=" + std::to_string(occupants.size()));
  out.push_back(Outgoing{.connection = connection.id, .message = room_list()});
}

void Hub::handle_list_audit(std::vector<Outgoing>& out, Connection& connection,
                            const protocol::ListAudit& message) {
  out.push_back(Outgoing{
      .connection = connection.id,
      .message = protocol::AuditList{.entries = audit_->list(message.limit, message.actor_id)}});
}

}  // namespace dv::server
