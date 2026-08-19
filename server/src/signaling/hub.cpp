#include "signaling/hub.hpp"

#include <utility>

#include <dv/logging/logger.hpp>

namespace dv::server {
namespace {

Error unauthorized(std::string message) {
  return Error{"unauthorized", std::move(message)};
}

}  // namespace

Hub::Hub() : Hub(Options{}) {}

Hub::Hub(Options options)
    : options_(options),
      rooms_(RoomManager::Options{options.max_participants_per_room, options.room_id_seed}) {}

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
      out.push_back(Outgoing{*target, message});
    }
  }
}

void Hub::reply_error(std::vector<Outgoing>& out, ConnectionId connection, const Error& error) {
  out.push_back(Outgoing{connection, protocol::ErrorMessage{error.code, error.message}});
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
          out.push_back(Outgoing{state->id, protocol::Pong{value.nonce}});

        } else if constexpr (std::is_same_v<T, protocol::Pong>) {
          // last_seen was already refreshed above, which is the whole point.

        } else {
          // Messages the server only ever sends. Receiving one is a client bug.
          reply_error(out, state->id,
                      Error{"unknown_message_type", "this message is server to client only"});
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
  out.push_back(Outgoing{
      connection.id, protocol::Authenticated{value.user, value.token, value.expires_in_seconds}});
}

void Hub::handle_create_room(std::vector<Outgoing>& out, Connection& connection,
                             const protocol::CreateRoom& message) {
  if (message.user_id != connection.user->id) {
    reply_error(out, connection.id, unauthorized("user_id does not match this session"));
    return;
  }

  auto created = rooms_.create_room(message.room_name);
  if (!created) {
    reply_error(out, connection.id, created.error());
    return;
  }
  const std::string room_id = std::move(created).take();

  DV_LOG_INFO("Room {} created by {}", room_id, connection.user->id);
  out.push_back(Outgoing{connection.id, protocol::RoomCreated{room_id, message.room_name}});
}

void Hub::handle_join_room(std::vector<Outgoing>& out, Connection& connection,
                           const protocol::JoinRoom& message) {
  if (message.user_id != connection.user->id) {
    reply_error(out, connection.id, unauthorized("user_id does not match this session"));
    return;
  }

  models::User user = *connection.user;
  if (!message.display_name.empty()) {
    user.display_name = message.display_name;
  }

  if (const auto failure = rooms_.join(message.room_id, user)) {
    reply_error(out, connection.id, *failure);
    return;
  }
  connection.room_id = message.room_id;
  connection.user->display_name = user.display_name;

  const models::Room* room = rooms_.find(message.room_id);

  // Everyone already in the room, then the joiner themselves. The client uses
  // its own user_joined as the signal that the initial state is complete, so it
  // has to arrive last. See section 7 of docs/protocol.md.
  for (const models::Participant& participant : room->participants) {
    if (participant.user.id == user.id) {
      continue;
    }
    out.push_back(Outgoing{connection.id, protocol::UserJoined{message.room_id, participant.user}});
  }
  out.push_back(Outgoing{connection.id, protocol::UserJoined{message.room_id, user}});

  // A screen share already in progress has to be announced to the newcomer.
  if (const models::Participant* sharer = room->screen_sharer()) {
    out.push_back(
        Outgoing{connection.id, protocol::ScreenShareStarted{message.room_id, sharer->user.id}});
  }

  broadcast(out, message.room_id, protocol::UserJoined{message.room_id, user}, user.id);
  DV_LOG_INFO("User {} joined room {} ({} participants)", user.id, message.room_id, room->size());

  // Last, so that the participant already knows who is in the room by the time
  // the SFU starts negotiating media with them.
  if (media_signals_ != nullptr) {
    media_signals_->on_participant_joined(message.room_id, user);
  }
}

void Hub::handle_leave_room(std::vector<Outgoing>& out, Connection& connection,
                            const protocol::LeaveRoom& message) {
  if (message.user_id != connection.user->id) {
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
    broadcast(out, message.room_id, protocol::ScreenShareStopped{message.room_id, message.user_id});
  }
  broadcast(out, message.room_id, protocol::UserLeft{message.room_id, message.user_id});
  DV_LOG_INFO("User {} left room {}", message.user_id, message.room_id);

  if (media_signals_ != nullptr) {
    media_signals_->on_participant_left(message.room_id, message.user_id);
  }
}

void Hub::handle_relay(std::vector<Outgoing>& out, Connection& connection,
                       const protocol::Message& message, const std::string& room_id,
                       const std::string& from_user_id, const std::string& to_user_id) {
  if (from_user_id != connection.user->id) {
    reply_error(out, connection.id, unauthorized("from_user_id does not match this session"));
    return;
  }

  const models::Room* room = rooms_.find(room_id);
  if (room == nullptr) {
    reply_error(out, connection.id, Error{"room_not_found", "no room with id " + room_id});
    return;
  }
  if (!room->contains(from_user_id)) {
    reply_error(out, connection.id, Error{"not_in_room", "sender is not in " + room_id});
    return;
  }

  // Media is routed through the SFU, so a frame addressed to it is negotiation
  // with the server itself rather than something to hand to another
  // participant. See protocol::kSfuUserId.
  if (to_user_id == protocol::kSfuUserId) {
    if (media_signals_ == nullptr) {
      reply_error(out, connection.id,
                  Error{"media_unavailable", "this server is not routing media"});
      return;
    }
    media_signals_->on_media_signal(room_id, from_user_id, message);
    return;
  }

  if (!room->contains(to_user_id)) {
    reply_error(out, connection.id, Error{"not_in_room", "recipient is not in " + room_id});
    return;
  }

  const auto target = connection_of_user(to_user_id);
  if (!target.has_value()) {
    reply_error(out, connection.id, Error{"not_in_room", "recipient is not connected"});
    return;
  }

  // The server forwards the frame untouched. It never inspects SDP or ICE.
  out.push_back(Outgoing{*target, message});
}

void Hub::handle_mute(std::vector<Outgoing>& out, Connection& connection,
                      const std::string& room_id, const std::string& user_id, bool muted) {
  if (user_id != connection.user->id) {
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
    broadcast(out, room_id, protocol::Mute{room_id, user_id});
  } else {
    broadcast(out, room_id, protocol::Unmute{room_id, user_id});
  }
}

void Hub::handle_screen_share(std::vector<Outgoing>& out, Connection& connection,
                              const std::string& room_id, const std::string& user_id,
                              bool sharing) {
  if (user_id != connection.user->id) {
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
    broadcast(out, room_id, protocol::ScreenShareStarted{room_id, user_id});
  } else {
    DV_LOG_INFO("User {} stopped sharing in room {}", user_id, room_id);
    broadcast(out, room_id, protocol::ScreenShareStopped{room_id, user_id});
  }
}

std::vector<Outgoing> Hub::on_disconnect(ConnectionId connection, Clock::time_point) {
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
    broadcast(out, *room_id, protocol::ScreenShareStopped{*room_id, user_id});
  }
  broadcast(out, *room_id, protocol::UserLeft{*room_id, user_id});

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
      out.push_back(Outgoing{id, protocol::Ping{}});
    }
  }

  authenticator_.expire_tokens(now);
  return out;
}

}  // namespace dv::server
