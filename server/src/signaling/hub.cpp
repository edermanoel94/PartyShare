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
      owned_chat_(options.chat == nullptr ? std::make_unique<store::MemoryChatStore>() : nullptr),
      owned_notices_(options.notices == nullptr ? std::make_unique<store::MemoryNoticeStore>()
                                                : nullptr),
      owned_sessions_(options.sessions == nullptr ? std::make_unique<store::MemorySessionStore>()
                                                  : nullptr),
      owned_audit_(options.audit == nullptr ? std::make_unique<store::MemoryAuditLog>() : nullptr),
      users_(options.users != nullptr ? options.users : owned_users_.get()),
      chat_(options.chat != nullptr ? options.chat : owned_chat_.get()),
      notices_(options.notices != nullptr ? options.notices : owned_notices_.get()),
      sessions_(options.sessions != nullptr ? options.sessions : owned_sessions_.get()),
      audit_(options.audit != nullptr ? options.audit : owned_audit_.get()),
      owned_restrictions_(options.restrictions == nullptr
                              ? std::make_unique<StoreRestrictionSource>(*users_)
                              : nullptr),
      restrictions_(options.restrictions != nullptr ? options.restrictions
                                                    : owned_restrictions_.get()),
      rooms_(RoomManager::Options{
          .max_participants_per_room = options.max_participants_per_room,
          .id_seed = options.room_id_seed,
          .store = options.rooms != nullptr ? options.rooms : owned_rooms_.get(),
          .chat = chat_}),
      authenticator_(Authenticator::Options{}, *users_) {
  // Rooms somebody wrote down have to still exist after a restart.
  if (const std::size_t loaded = rooms_.load_rooms(); loaded > 0) {
    DV_LOG_INFO("Loaded {} room(s) from the store", loaded);
  }

  // Sessions from a run that was killed rather than stopped. Nobody else will
  // ever close them, and left open they are people this server would report as
  // connected for as long as the database exists. See SessionStore::close_open,
  // which stamps each with when it was last heard from rather than with now.
  if (const std::size_t recovered = sessions_->close_open(); recovered > 0) {
    DV_LOG_WARN("Closed {} session(s) left open by a previous run", recovered);
  }
}

models::Role Hub::current_role(const std::string& user_id) const {
  const auto account = users_->find_by_id(user_id);
  return account.has_value() ? account->user.role : models::Role::User;
}

models::Restrictions Hub::restrictions_of(const std::string& user_id) const {
  const auto account = users_->find_by_id(user_id);
  return account.has_value() ? account->user.restrictions : models::Restrictions{};
}

std::string Hub::user_label(const std::string& user_id) const {
  // The open connections first. It costs a hash lookup and covers every line
  // written about somebody who is here, which is nearly all of them.
  if (const auto mapped = user_to_connection_.find(user_id); mapped != user_to_connection_.end()) {
    if (const auto connection = connections_.find(mapped->second);
        connection != connections_.end()) {
      // The identity is bound to a name and then tested through that name,
      // rather than reached through the iterator twice. The two spellings mean
      // the same thing, but clang-tidy's unchecked-optional-access cannot see
      // that two dereferences of one iterator are one object, so it reads the
      // second as unguarded and the build treats that as an error.
      const std::optional<models::User>& identity = connection->second.user;
      if (identity.has_value()) {
        return models::user_label(user_id, identity->display_name, connection->second.username);
      }
    }
  }
  // Then the store, for somebody who is not. Not a defensive branch: the line
  // about an account deleted from under a session is written once the identity
  // on the connection has already been taken away.
  if (const auto account = users_->find_by_id(user_id)) {
    return models::user_label(user_id, account->user.display_name, account->username);
  }
  // Nothing left to ask. The identifier is not a name, but it is what the
  // caller meant, and a line naming nobody is worse than one carrying a code.
  return user_id;
}

std::string Hub::room_label(const std::string& room_id) const {
  const models::Room* room = rooms_.find(room_id);
  return room != nullptr ? models::room_label(room->id, room->name) : room_id;
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
  // Built from the username the entry already resolved, so this costs no
  // second lookup, and it falls back the same way: an actor whose account has
  // just been deleted reads as the display name alone.
  const std::string actor_label =
      models::user_label(actor.id, actor.display_name, entry.actor_username);
  if (auto failure = audit_->append(std::move(entry))) {
    // Error level and not warning: an administrative action that happened and
    // was not recorded is the exact hole an audit log exists to close, and
    // whoever reads the logs has to see it.
    DV_LOG_ERROR("Audit entry for '{}' by {} was not written: {}", action_name, actor_label,
                 failure->message);
  }
}

void Hub::on_connect(ConnectionId connection, std::string remote_address, Clock::time_point now) {
  Connection state;
  state.id = connection;
  state.remote_address = std::move(remote_address);
  state.last_seen = now;
  state.last_ping = now;
  connections_.insert_or_assign(connection, std::move(state));
  DV_LOG_DEBUG("Connection {} opened", connection);
}

void Hub::open_session(Connection& connection) {
  if (!connection.user.has_value()) {
    return;
  }

  auto opened = sessions_->open(
      store::SessionRecord{.user_id = connection.user->id, .ip = connection.remote_address});
  if (!opened) {
    // Warning and not error, which is the difference between this and the
    // audit log. An administrative action that was not recorded is the hole an
    // audit log exists to close; a session that was not recorded is a row
    // missing from a presence report, and the person is in the room either
    // way. See store::SessionStore.
    DV_LOG_WARN(
        "Session for {} on connection {} was not recorded: {}",
        models::user_label(connection.user->id, connection.user->display_name, connection.username),
        connection.id, opened.error().message);
    return;
  }
  connection.session_id = std::move(opened).take().id;
}

void Hub::close_session(Connection& connection) {
  if (connection.session_id.empty()) {
    return;
  }
  // Cleared before the write, and unconditionally: whether or not the store
  // could be reached, this connection is done with that row, and a second
  // attempt from another of the three ways a session ends would be a second
  // failure to log about the same thing.
  const std::string session_id = std::exchange(connection.session_id, std::string{});
  if (auto failure = sessions_->close(session_id)) {
    DV_LOG_WARN("Session {} on connection {} was not closed: {}", session_id, connection.id,
                failure->message);
  }
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

  // Authentication is the only thing an unauthenticated connection may do, and
  // the heartbeat is the exception, because it is not something the connection
  // is doing. Section 4.6 of docs/06-protocol.md is titled "Transport level" for
  // that reason: the server pings every connection it holds, authenticated or
  // not, and a pong is the socket saying it is still there.
  //
  // Refusing it produced a loop nobody could leave. A client that mistyped a
  // password kept the socket, answered every ping, and was told `unauthorized`
  // for it once per heartbeat interval, for as long as the window stayed open.
  const bool is_heartbeat = std::holds_alternative<protocol::Ping>(message) ||
                            std::holds_alternative<protocol::Pong>(message);
  if (!state->user.has_value() && !is_heartbeat &&
      !std::holds_alternative<protocol::Authenticate>(message)) {
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
                    models::user_label(user_id, state->user->display_name, state->username),
                    protocol::type_name(type));
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

        } else if constexpr (std::is_same_v<T, protocol::ChangePassword>) {
          handle_change_password(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::Mute>) {
          handle_mute(out, *state, value.room_id, value.user_id, true);

        } else if constexpr (std::is_same_v<T, protocol::Unmute>) {
          handle_mute(out, *state, value.room_id, value.user_id, false);

        } else if constexpr (std::is_same_v<T, protocol::ScreenShareStarted>) {
          handle_screen_share(out, *state, value.room_id, value.user_id, true, value.has_audio);

        } else if constexpr (std::is_same_v<T, protocol::ScreenShareStopped>) {
          handle_screen_share(out, *state, value.room_id, value.user_id, false, false);

        } else if constexpr (std::is_same_v<T, protocol::ChatMessage>) {
          handle_chat(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::ListChat>) {
          handle_list_chat(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::SendNotice>) {
          handle_send_notice(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::AcknowledgeNotice>) {
          handle_acknowledge_notice(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::Ping>) {
          out.push_back(Outgoing{.connection = state->id, .message = protocol::Pong{value.nonce}});

        } else if constexpr (std::is_same_v<T, protocol::Pong>) {
          // last_seen was already refreshed above, which is the whole point.

        } else if constexpr (std::is_same_v<T, protocol::KickUser>) {
          handle_kick(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::ForceMute>) {
          handle_force_mute(out, *state, value);

        } else if constexpr (std::is_same_v<T, protocol::RestrictUser>) {
          handle_restrict_user(out, *state, value);

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
      // Before the identity goes, because closing the row needs neither - it
      // needs the identifier this connection is holding - but the order is
      // what makes the presence collection agree with `user_to_connection_`:
      // one account, one open session, at the same instant both stop being
      // true of the old socket.
      close_session(*stale);
      stale->user.reset();
      stale->room_id.reset();
      stale->username.clear();
      stale->delivered_notice_ids.clear();
    }
  }

  connection.user = value.user;
  // What was typed, which is what the account is called: both stores match a
  // username exactly, so a login that got this far got it letter for letter.
  connection.username = message.username;
  // A new identity is owed everything its account has pending, whatever an
  // earlier identity on this socket was handed.
  connection.delivered_notice_ids.clear();
  user_to_connection_[value.user.id] = connection.id;

  DV_LOG_INFO("User {} authenticated on connection {}",
              models::user_label(value.user.id, value.user.display_name, message.username),
              connection.id);
  out.push_back(
      Outgoing{.connection = connection.id,
               .message = protocol::Authenticated{.user = value.user,
                                                  .token = value.token,
                                                  .expires_in_seconds = value.expires_in_seconds}});

  open_session(connection);
  // After `authenticated`, and that order is the contract: a client is not
  // expected to make sense of a message about its account before it has been
  // told which account it is.
  deliver_pending_notices(out, connection);

  // A request to end this account's session that was written before this
  // login was about a session that has already ended on its own. Left
  // standing, the next heartbeat would end this one, which nobody asked for.
  (void)discard_session_end_request(value.user.id,
                                    "it was written before this login and is about a session that "
                                    "had already ended");
}

void Hub::deliver_pending_notices(std::vector<Outgoing>& out, Connection& connection) {
  if (!connection.user.has_value()) {
    return;
  }

  const std::vector<models::Notice> pending = notices_->pending_for(connection.user->id);
  std::size_t handed_over = 0;
  for (const models::Notice& notice : pending) {
    if (hand_over_notice(out, connection, notice)) {
      ++handed_over;
    }
  }
  if (handed_over > 0) {
    DV_LOG_INFO("Delivered {} outstanding notice(s) to {}", handed_over,
                models::user_label(connection.user->id, connection.user->display_name,
                                   connection.username));
  }
}

void Hub::deliver_notices_written_elsewhere(std::vector<Outgoing>& out) {
  for (auto& [id, state] : connections_) {
    if (!state.user.has_value()) {
      continue;
    }
    // The same query the login runs, and the same cap. Somebody handed twenty
    // notices at once on a heartbeat is in the situation the cap exists for,
    // and the rest arrive as these are acknowledged, exactly as at sign-in.
    const std::vector<models::Notice> pending = notices_->pending_for(state.user->id);
    std::size_t handed_over = 0;
    for (const models::Notice& notice : pending) {
      if (hand_over_notice(out, state, notice)) {
        ++handed_over;
      }
    }
    if (handed_over > 0) {
      // No actor and no audit entry, as with a restriction written elsewhere:
      // whoever wrote the notice recorded it, in the server's own vocabulary,
      // and the moment this server noticed is not a fact anybody is after.
      DV_LOG_INFO("Delivered {} notice(s) written outside this server to {}", handed_over,
                  models::user_label(state.user->id, state.user->display_name, state.username));
    }
  }
}

bool Hub::hand_over_notice(std::vector<Outgoing>& out, Connection& connection,
                           const models::Notice& notice) {
  if (!connection.delivered_notice_ids.insert(notice.id).second) {
    return false;
  }
  out.push_back(Outgoing{.connection = connection.id, .message = protocol::Notice{notice}});
  return true;
}

bool Hub::discard_session_end_request(const std::string& user_id, std::string_view why) {
  auto account = users_->find_by_id(user_id);
  if (!account.has_value() || account->session_end_requested_at == 0) {
    return false;
  }
  account->session_end_requested_at = 0;
  if (const auto failure = users_->update(*account)) {
    // Warning and not error: the session the request was about is over
    // either way. What is at stake is the next one, and the login that opens
    // it runs this again.
    DV_LOG_WARN("The request to end the session of {} could not be cleared: {}",
                user_label(user_id), failure->message);
    return true;
  }
  DV_LOG_INFO("Discarded the request to end the session of {}: {}", user_label(user_id), why);
  return true;
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

  // Checked before the ownership limit below, so that a name nobody could have
  // meant is answered as such rather than as "you already have a room". An
  // empty name is not a failure here: it is the request to be called by the
  // identifier, and the RoomManager is what fills that in.
  if (!models::is_valid_room_name(message.room_name)) {
    reply_error(
        out, connection.id,
        Error{.code = "invalid_value",
              .message = "a room name is at most " + std::to_string(models::kMaxRoomNameBytes) +
                         " bytes once trimmed, and carries no control characters"});
    return;
  }

  // One room each, for anybody who is not an administrator.
  //
  // The limit is what a room outliving its last participant costs. Before
  // that, a room evaporated when the last person walked out and nobody could
  // accumulate anything; now a room stays until somebody closes it, and only
  // an administrator can close one. Without a limit, every ordinary user is
  // free to leave rooms behind that only somebody else can clear away.
  //
  // Refused rather than silently reusing the room they already have: they may
  // have meant to make a second one, and finding out is better than being
  // handed an identifier that is not new. The message names the room so the
  // client can say which one is in the way.
  if (current_role(user->id) != models::Role::Admin) {
    if (const auto existing = rooms_.room_owned_by(user->id)) {
      reply_error(out, connection.id,
                  Error{.code = "room_limit_reached",
                        .message = "you already have room " + *existing +
                                   "; an administrator has to close it before you can make "
                                   "another"});
      return;
    }
  }

  // `message.persistent` is read no more. Every room outlives its last
  // participant now, so asking for one is asking for what you were going to
  // get, and the administrator gate that used to guard the request guarded
  // nothing worth guarding. The field stays on the wire so that a client built
  // before this still connects.
  //
  // `message.capacity` is passed through as it came, zero included: the
  // RoomManager is what knows the default and the ceiling, and it answers a
  // size it cannot give with `invalid_value` naming the range.
  auto created = rooms_.create_room(message.room_name, user->id, message.capacity);
  if (!created) {
    reply_error(out, connection.id, created.error());
    return;
  }
  const std::string room_id = std::move(created).take();

  // The name the room ended up with, which is not always the one that was
  // asked for: an empty request becomes the identifier. Echoing the request
  // back would tell whoever created the room that it has no name, while every
  // other client's list shows it carrying a code.
  const models::Room* created_room = rooms_.find(room_id);
  const std::string room_name = created_room != nullptr ? created_room->name : room_id;
  // Echoed for the same reason the name is: a request for nothing in
  // particular came back with a number, and the creator is the one person
  // who has to know it to tell others whether they fit.
  const int capacity = created_room != nullptr ? created_room->capacity : message.capacity;

  // Not recorded in the audit log. It used to be, for the one room an
  // administrator had to ask for; creating a room is now something every
  // participant does, and the log is for administrative actions only. See
  // NothingAParticipantDoesReachesTheLog.
  // Both the name and the identifier, and this is the one line that keeps the
  // identifier on purpose: it is what the creator has to pass to whoever they
  // want in the room, so it is the fact an operator is here to read.
  DV_LOG_INFO("Room {} (\"{}\", {} people) created by {}", room_id, room_name, capacity,
              models::user_label(user->id, user->display_name, connection.username));
  out.push_back(Outgoing{.connection = connection.id,
                         .message = protocol::RoomCreated{
                             .room_id = room_id, .room_name = room_name, .capacity = capacity}});
  broadcast_room_list(out);
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
    // Adopted only if it is a name and not a payload. See
    // models::is_valid_display_name: a tab in here moves which field every
    // other client reads as the user id, and their moderation menu with it.
    //
    // Ignored rather than refused, and the join goes through under the name
    // the account already has. Refusing would answer an attempt to forge a
    // name by locking the account out of rooms, which punishes the one case
    // where this fires by accident and gains nothing against the case where it
    // does not - the name is dropped either way.
    if (models::is_valid_display_name(message.display_name)) {
      user.display_name = message.display_name;
    } else {
      DV_LOG_WARN("Rejected a display name carrying control characters from {}",
                  models::user_label(user.id, user.display_name, connection.username));
    }
  }

  // Read from the store rather than taken from the identity this connection
  // logged in with, for the reason restrictions_of gives: a restriction
  // applied while somebody was connected has to be true of the room they are
  // about to walk into. The cached copy is from login time, and the copy that
  // goes out in `user_joined` is what every other client renders them from.
  user.restrictions = restrictions_of(user.id);
  authenticated_user->restrictions = user.restrictions;
  if (user.restrictions.banned) {
    // Unreachable while banning revokes the tokens and drops the connection.
    // Here because a room is the one place a banned account being present
    // would be visible to other people, and the cost of the check is a lookup
    // that has already happened.
    reply_error(out, connection.id,
                Error{.code = "account_banned",
                      .message = "this account has been suspended by an administrator"});
    return;
  }

  if (const auto failure = rooms_.join(message.room_id, user)) {
    reply_error(out, connection.id, *failure);
    return;
  }
  connection.room_id = message.room_id;
  authenticated_user->display_name = user.display_name;

  // Applied before anybody is told about the joiner, so that the first thing
  // the room learns about them is already correct. `by_admin` is what makes it
  // hold: without it the participant's own client would release it with the
  // first click on a microphone that turned itself off.
  //
  // Nothing is broadcast for it. The restrictions travel on the user object of
  // `user_joined`, which every client in the room reads anyway, and a second
  // announcement of the same fact is a second thing to keep in step.
  if (user.restrictions.muted) {
    (void)rooms_.set_muted(message.room_id, user.id, true, true);
  }

  const models::Room* room = rooms_.find(message.room_id);

  // Everyone already in the room, then the joiner themselves. The client uses
  // its own user_joined as the signal that the initial state is complete, so it
  // has to arrive last. See section 7 of docs/06-protocol.md.
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
    out.push_back(
        Outgoing{.connection = connection.id,
                 .message = protocol::ScreenShareStarted{.room_id = message.room_id,
                                                         .user_id = sharer->user.id,
                                                         .has_audio = sharer->sharing_audio}});
  }

  // What was already said, so that somebody joining a persistent room arrives
  // into the conversation rather than into an empty panel and a room that
  // apparently has nothing to say. Sent without being asked, because a client
  // that had to ask would show the empty panel first either way.
  //
  // After the participant list, so that every message it carries is about
  // somebody the client has already been told about.
  out.push_back(Outgoing{.connection = connection.id, .message = chat_history(message.room_id, 0)});

  broadcast(out, message.room_id, protocol::UserJoined{.room_id = message.room_id, .user = user},
            user.id);
  // Everybody else's list carries how many are in each room, and it is on the
  // first screen they see. A count that only moved when a room appeared or was
  // closed showed nought for a room somebody was sitting in.
  broadcast_room_list(out);
  // Resolved once. The line below and the SFU want the same two names, and the
  // SFU keeps its copies for the life of the session rather than asking again:
  // it runs on its own thread and nothing here would be safe for it to read.
  const std::string joined_label =
      models::user_label(user.id, user.display_name, connection.username);
  const std::string joined_room = models::room_label(room->id, room->name);
  DV_LOG_INFO("User {} joined room {} ({} participants)", joined_label, joined_room, room->size());

  // Last, so that the participant already knows who is in the room by the time
  // the SFU starts negotiating media with them.
  if (media_signals_ != nullptr) {
    media_signals_->on_participant_joined(message.room_id, joined_room, user, joined_label);
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
  broadcast_room_list(out);
  DV_LOG_INFO("User {} left room {}", user_label(message.user_id), room_label(message.room_id));

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
                              const std::string& room_id, const std::string& user_id, bool sharing,
                              bool with_audio) {
  const models::User* user = authenticated(out, connection);
  if (user == nullptr) {
    return;
  }
  if (user_id != user->id) {
    reply_error(out, connection.id, unauthorized("user_id does not match this session"));
    return;
  }

  // Only on the way in. Stopping is never refused: a share that an
  // administrator has just blocked has to be stoppable by the person running
  // it, and by the server on their behalf, or the frames keep coming.
  if (sharing && restrictions_of(user_id).screen_share_blocked) {
    reply_error(out, connection.id,
                Error{.code = "forbidden",
                      .message = "an administrator has blocked screen sharing for this account"});
    return;
  }

  const auto failure = sharing ? rooms_.start_screen_share(room_id, user_id, with_audio)
                               : rooms_.stop_screen_share(room_id, user_id);
  if (failure) {
    reply_error(out, connection.id, *failure);
    return;
  }

  if (sharing) {
    DV_LOG_INFO("User {} started sharing in room {}", user_label(user_id), room_label(room_id));
    broadcast(out, room_id,
              protocol::ScreenShareStarted{
                  .room_id = room_id, .user_id = user_id, .has_audio = with_audio});
  } else {
    DV_LOG_INFO("User {} stopped sharing in room {}", user_label(user_id), room_label(room_id));
    broadcast(out, room_id, protocol::ScreenShareStopped{.room_id = room_id, .user_id = user_id});
  }
}

// --- chat --------------------------------------------------------------------

protocol::ChatHistory Hub::chat_history(const std::string& room_id, int limit) const {
  return protocol::ChatHistory{.room_id = room_id, .messages = chat_->list(room_id, limit)};
}

void Hub::handle_chat(std::vector<Outgoing>& out, Connection& connection,
                      const protocol::ChatMessage& message) {
  const models::User* user = authenticated(out, connection);
  if (user == nullptr) {
    return;
  }
  if (message.message.user_id != user->id) {
    reply_error(out, connection.id, unauthorized("user_id does not match this session"));
    return;
  }

  const std::string& room_id = message.message.room_id;
  const models::Room* room = rooms_.find(room_id);
  if (room == nullptr) {
    reply_error(out, connection.id,
                Error{.code = "room_not_found", .message = "no room with id " + room_id});
    return;
  }
  const models::Participant* participant = room->find(user->id);
  if (participant == nullptr) {
    reply_error(out, connection.id,
                Error{.code = "not_in_room", .message = "sender is not in " + room_id});
    return;
  }

  // Before the text is validated, so that somebody who may not speak is told
  // that rather than being told their message was the wrong length.
  //
  // Read from the store on every message rather than from the copy the room
  // holds, which is the one place in this class where that costs something: a
  // chat message is not rare. It buys the thing tools/dbadmin's README claims,
  // that a restriction written straight into the database takes effect on the
  // account's next message and not on its next login. The message is already
  // one synchronous store write below this line, so the lookup is a share of
  // what this handler costs rather than a new kind of cost.
  if (restrictions_of(user->id).silenced) {
    reply_error(out, connection.id,
                Error{.code = "forbidden",
                      .message = "an administrator has silenced this account in chat"});
    return;
  }

  if (!models::is_valid_chat_text(message.message.text)) {
    reply_error(
        out, connection.id,
        Error{.code = "invalid_value",
              .message = "a message is between 1 and " + std::to_string(models::kMaxChatTextBytes) +
                         " bytes once trimmed"});
    return;
  }

  models::ChatMessage written;
  written.room_id = room_id;
  written.user_id = user->id;
  // The name the room knows them by, and not the one the sender put in the
  // message. A client that could choose it per message could sign somebody
  // else's name to what it says.
  written.display_name = participant->user.display_name;
  written.text = models::trim_chat_text(message.message.text);

  // Stored before it is announced, and a failure stops it here. That is the
  // opposite of what an administrative action does with the audit log, and the
  // reason is in ChatStore: a message everybody saw and nobody can find again
  // is worse than one the sender was told about.
  auto appended = chat_->append(std::move(written));
  if (!appended) {
    DV_LOG_ERROR("A message from {} in room {} was not stored: {}",
                 models::user_label(user->id, user->display_name, connection.username),
                 room_label(room_id), appended.error().message);
    reply_error(out, connection.id, appended.error());
    return;
  }
  const models::ChatMessage stored = std::move(appended).take();

  // The whole room, the sender included, and everybody displays the server's
  // copy: same identifier, same timestamp, same order on every screen.
  //
  // The size and not the text. What people say to each other has no business
  // in an operator's log file, and a message is the one field of this protocol
  // that is written by a person for other people rather than for the server.
  // Assembled from what is already in hand and never through `user_label`,
  // which would reach the store. spdlog's macros here carry no level guard, so
  // the arguments of a debug line are evaluated even when debug is off, and
  // this one is written once per message somebody sends.
  DV_LOG_DEBUG("Message {} from {} in room {}, {} bytes", stored.id,
               models::user_label(stored.user_id, stored.display_name, connection.username),
               room_label(room_id), stored.text.size());
  broadcast(out, room_id, protocol::ChatMessage{.message = stored});
}

void Hub::handle_list_chat(std::vector<Outgoing>& out, Connection& connection,
                           const protocol::ListChat& message) {
  const models::User* user = authenticated(out, connection);
  if (user == nullptr) {
    return;
  }

  const models::Room* room = rooms_.find(message.room_id);
  if (room == nullptr) {
    reply_error(out, connection.id,
                Error{.code = "room_not_found", .message = "no room with id " + message.room_id});
    return;
  }
  // A conversation is readable by the people it happened in front of, and this
  // is what says so. Without it any account could read any room by trying six
  // characters at a time, which is a search of sixteen million and not a wall.
  // An administrator is not excepted: administration is in section 4.7 of
  // docs/06-protocol.md, and reading what people said to each other is not in it.
  if (!room->contains(user->id)) {
    reply_error(out, connection.id,
                Error{.code = "not_in_room", .message = "you are not in " + message.room_id});
    return;
  }

  out.push_back(Outgoing{.connection = connection.id,
                         .message = chat_history(message.room_id, message.limit)});
}

void Hub::handle_send_notice(std::vector<Outgoing>& out, Connection& connection,
                             const protocol::SendNotice& message) {
  const models::User* actor = authenticated(out, connection);
  if (actor == nullptr) {
    return;
  }

  if (!models::is_valid_notice_text(message.text)) {
    reply_error(out, connection.id,
                Error{.code = "invalid_value",
                      .message = "a notice must not be empty and must fit in " +
                                 std::to_string(models::kMaxNoticeTextBytes) + " bytes"});
    return;
  }

  // The account has to exist. Not a formality: a notice to an identifier
  // nobody answers to is a row that will never be delivered and never be
  // acknowledged, and the administrator who mistyped it would be waiting on an
  // answer from nobody.
  const auto target = users_->find_by_id(message.user_id);
  if (!target.has_value()) {
    reply_error(out, connection.id,
                Error{.code = "user_not_found", .message = "no account with that identifier"});
    return;
  }

  auto written =
      notices_->append(models::Notice{.user_id = target->user.id,
                                      .from_user_id = actor->id,
                                      // The name they hold now, kept with the notice. The
                                      // recipient may read it a week later, by which time the
                                      // administrator may have renamed themselves or gone.
                                      .from_display_name = actor->display_name,
                                      .text = models::trim_notice_text(message.text)});
  if (!written) {
    // Refused rather than delivered anyway, which is the opposite of what an
    // audit failure does and the same as what a chat failure does. The store
    // is the notice: an unwritten one has no identifier, so nobody could
    // acknowledge it, and an administrator would be told it was sent while
    // nothing was.
    reply_error(out, connection.id, written.error());
    return;
  }

  const models::Notice notice = std::move(written).take();
  const protocol::Notice delivered{notice};

  // To the recipient if they are here, and to the administrator either way.
  // The administrator's copy is the confirmation - it carries the identifier
  // and the moment the store assigned - and a client tells the two apart by
  // whether the notice names it as the recipient.
  //
  // Once and not twice when an administrator writes to their own account,
  // which is allowed and is a reasonable way to leave oneself a note. Two
  // copies of one row would be two boxes to dismiss and only one of them
  // acknowledgeable.
  // Through `hand_over_notice`, which is what tells the heartbeat's pass not
  // to hand it over a second time five seconds from now.
  const std::optional<ConnectionId> target_connection = connection_of_user(notice.user_id);
  if (target_connection.has_value()) {
    if (Connection* recipient = find_connection(*target_connection)) {
      (void)hand_over_notice(out, *recipient, notice);
    }
  }
  if (target_connection != connection.id) {
    out.push_back(Outgoing{.connection = connection.id, .message = delivered});
  }

  DV_LOG_INFO("{} sent a notice to {}", user_label(actor->id), user_label(notice.user_id));
  record(*actor, "send_notice", notice.user_id, {}, "notice=" + notice.id + " " + notice.text);
}

void Hub::handle_acknowledge_notice(std::vector<Outgoing>& out, Connection& connection,
                                    const protocol::AcknowledgeNotice& message) {
  const models::User* actor = authenticated(out, connection);
  if (actor == nullptr) {
    return;
  }

  // The account is the connection's own and is not a field of the message, so
  // there is nothing here anybody could aim somewhere else. Same argument as
  // protocol::ChangePassword.
  auto acknowledged = notices_->acknowledge(message.notice_id, actor->id);
  if (!acknowledged) {
    reply_error(out, connection.id, acknowledged.error());
    return;
  }

  const models::Notice notice = std::move(acknowledged).take();
  DV_LOG_INFO("{} acknowledged the notice {} sent them", user_label(actor->id),
              user_label(notice.from_user_id));

  // The audit log is where this lands and the only place it does. Nothing goes
  // back to the administrator on the wire, on purpose: they may well not be
  // connected - a notice exists precisely because the two of them need not be
  // here at the same time - and a receipt that only arrives when they happen
  // to be online is a receipt nobody can rely on. The entry is there whether
  // they read it a minute or a month later.
  //
  // The actor is the person acknowledging, which makes this the one entry in
  // the log written by somebody who is not an administrator. It belongs there
  // all the same: it is the second half of an administrative action, and an
  // action whose outcome is recorded somewhere else is one nobody can follow
  // through.
  record(*actor, "acknowledge_notice", actor->id, {},
         "notice=" + notice.id + " from=" + user_label(notice.from_user_id));
}

std::vector<Outgoing> Hub::on_disconnect(ConnectionId connection, Clock::time_point /*now*/) {
  std::vector<Outgoing> out;

  const auto it = connections_.find(connection);
  if (it == connections_.end()) {
    return out;
  }
  // Before the copy and the erase, because it writes to the connection: the
  // row it closes is the one this socket opened, and after the erase there is
  // nowhere to record that it has been dealt with.
  close_session(it->second);
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
  broadcast_room_list(out);

  if (media_signals_ != nullptr) {
    media_signals_->on_participant_left(*room_id, user_id);
  }

  // From the copy of the connection taken before it was erased, because the
  // identity behind it is no longer reachable through `user_label`.
  DV_LOG_INFO("Connection {} closed, user {} removed from room {}", connection,
              models::user_label(user_id, state.user->display_name, state.username),
              room_label(*room_id));
  return out;
}

std::vector<Outgoing> Hub::tick(Clock::time_point now, std::vector<ConnectionId>& timed_out) {
  std::vector<Outgoing> out;

  // Collected in the same pass that pings, and written in one call below. A
  // connection this pass has given up on is left out: it is about to be closed
  // by the caller, and saying it was seen just now is the one thing that would
  // keep it looking present in a presence report for another interval.
  std::vector<std::string> alive;
  alive.reserve(connections_.size());

  for (auto& [id, state] : connections_) {
    if (now - state.last_seen >= options_.heartbeat_timeout) {
      timed_out.push_back(id);
      continue;
    }
    if (!state.session_id.empty()) {
      alive.push_back(state.session_id);
    }
    if (now - state.last_ping >= options_.heartbeat_interval) {
      state.last_ping = now;
      out.push_back(Outgoing{.connection = id, .message = protocol::Ping{}});
    }
  }

  // One write for every open session at once, which is what keeps a database
  // out of the per connection cost of a heartbeat. See SessionStore::touch.
  if (auto failure = sessions_->touch(alive)) {
    DV_LOG_WARN("Could not refresh {} session(s): {}", alive.size(), failure->message);
  }

  authenticator_.expire_tokens(now);

  // After the timed-out connections have been collected and before the caller
  // closes them, which is the order that matters: a connection this pass is
  // about to give up on is not one worth taking a microphone from, and the
  // `timed_out` list above is only reported, so it is still in `connections_`
  // and would be looked up for nothing. That is a lookup, not a bug, and it
  // costs less than the alternative of walking the map twice.
  apply_restrictions_written_elsewhere(out);
  deliver_notices_written_elsewhere(out);
  return out;
}

void Hub::apply_restrictions_written_elsewhere(std::vector<Outgoing>& out) {
  std::vector<WatchedAccount> watched;
  watched.reserve(connections_.size());
  for (const auto& [id, state] : connections_) {
    if (!state.user.has_value()) {
      continue;
    }
    watched.push_back(WatchedAccount{.user_id = state.user->id, .known = state.user->restrictions});
  }
  if (watched.empty()) {
    return;
  }

  // Collected before anything is enforced. `enforce` rewrites the identity
  // cached on the connection, and one of its branches drops that identity
  // altogether, so a loop that asked the source while acting on its answers
  // would be reading a list it was in the middle of invalidating.
  const RestrictionPoll found = restrictions_->poll(watched);

  for (const RestrictionChange& change : found.changed) {
    const auto account = users_->find_by_id(change.user_id);
    if (!account.has_value()) {
      continue;
    }

    DV_LOG_INFO("Restrictions on {} were changed outside this server: {}",
                models::user_label(change.user_id, account->user.display_name, account->username),
                models::describe(change.after));

    // No actor, and no audit entry written here. Whoever edited the account
    // wrote its audit entry themselves -- tools/dbadmin does, in the server's
    // own vocabulary -- and a second entry from this side would record the
    // moment the server noticed rather than the moment somebody decided, which
    // is not a fact anybody is looking for. The empty actor travels to the
    // clients as an empty `by_user_id`, which they already render as "an
    // administrator": exactly as much as this server knows.
    enforce(out, models::User{}, account->user, change.before, "");
  }

  // An account removed from the store while somebody was signed in as it.
  //
  // The document is already gone, so there is nothing left to read and nothing
  // to restrict; what is left is the half the store cannot reach. The person is
  // still in a room, still holding tokens, and still an identity on a
  // connection that every handler will go on trusting, because every handler
  // trusts `Connection::user` and that copy was loaded at login from a row
  // which no longer exists.
  //
  // Which is also why this is the whole of the fix: the account is *already*
  // in memory here, on the connection, and that is exactly what has to be
  // taken away. `end_session_of` is the same call `delete_user` makes, so
  // somebody removed from the client's panel and somebody removed from the
  // database leave a room the same way and look identical to everybody's
  // client and to the SFU.
  for (const std::string& user_id : found.gone) {
    // Resolved before the session ends, while the connection still carries the
    // identity: the account itself is already gone from the store.
    const std::string label = user_label(user_id);
    DV_LOG_INFO("Account {} was removed outside this server, ending the session it held", label);
    end_session_of(out, user_id, "the account was removed");
  }

  // An operator asking, from the terminal, for somebody to be signed out.
  //
  // The same call a ban makes and nothing else: the account is left exactly
  // as it is, and the person may sign in again the moment they like. That is
  // the whole difference between this and `banned`, and it is why the request
  // is spent here rather than left on the account - a mark that stayed would
  // end the next session as well, and "until an administrator says
  // otherwise" is what a ban is for.
  for (const std::string& user_id : found.session_end_requested) {
    // A ban found on the same pass has already done this. Ending twice would
    // be harmless, but the log line would say something happened when it did
    // not.
    if (connection_of_user(user_id).has_value()) {
      DV_LOG_INFO("An operator asked outside this server for the session of {} to end",
                  user_label(user_id));
      end_session_of(out, user_id, "the session was ended by an administrator");
    }
    (void)discard_session_end_request(user_id, "the session it was about has been ended");
  }
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
  broadcast_room_list(out);
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

  DV_LOG_INFO("{} kicked {} from room {}",
              models::user_label(actor->id, actor->display_name, connection.username),
              user_label(message.user_id), room_label(message.room_id));
  record(*actor, "kick", message.user_id, message.room_id, message.reason);
  evict(out, message.room_id, message.user_id, message.reason);
}

void Hub::handle_force_mute(std::vector<Outgoing>& out, Connection& connection,
                            const protocol::ForceMute& message) {
  const models::User* actor = authenticated(out, connection);
  if (actor == nullptr) {
    return;
  }

  // A forced unmute is about this room; the account restriction is about the
  // account, and it is the stronger statement of the two. Letting the room
  // level one win would hand the microphone back to somebody an administrator
  // muted everywhere, and the only sign of it would be them talking.
  //
  // Refused rather than quietly lifting the restriction as well: an
  // administrator who meant that has a control that says so, and one who did
  // not would be undoing a decision this message never mentioned.
  if (!message.muted && restrictions_of(message.user_id).muted) {
    reply_error(out, connection.id,
                Error{.code = "invalid_target",
                      .message = "this account is muted by a restriction, lift that instead"});
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

void Hub::handle_restrict_user(std::vector<Outgoing>& out, Connection& connection,
                               const protocol::RestrictUser& message) {
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
  // The same refusal delete_user and the self kick give, and for the strongest
  // version of the same reason: an administrator who bans themselves has taken
  // away the account that would have to lift it.
  if (account->user.id == actor->id) {
    reply_error(out, connection.id,
                Error{.code = "invalid_target",
                      .message = "an administrator cannot restrict their own account"});
    return;
  }

  const models::Restrictions before = account->user.restrictions;
  models::Restrictions after = before;
  // Absent means unchanged, which is what makes two administrators working at
  // once safe: unmuting somebody does not quietly lift the ban a colleague
  // applied while this panel was open.
  if (message.banned.has_value()) {
    after.banned = *message.banned;
  }
  if (message.muted.has_value()) {
    after.muted = *message.muted;
  }
  if (message.silenced.has_value()) {
    after.silenced = *message.silenced;
  }
  if (message.screen_share_blocked.has_value()) {
    after.screen_share_blocked = *message.screen_share_blocked;
  }

  if (after == before) {
    // Nothing written, nothing recorded, nothing announced. A form submitted
    // with the boxes it already had ticked is not an administrative action,
    // and an audit log full of those is a log nobody reads. The answer is
    // still the list, so the panel that asked ends up in a known state.
    out.push_back(Outgoing{.connection = connection.id, .message = user_list()});
    return;
  }

  // The third way to end up with a system nobody can administer, next to
  // deleting the last administrator and demoting them.
  if (after.banned && !before.banned && account->user.role == models::Role::Admin &&
      users_->count_with_role(models::Role::Admin) <= 1) {
    reply_error(
        out, connection.id,
        Error{.code = "last_administrator", .message = "the last administrator cannot be banned"});
    return;
  }

  account->user.restrictions = after;
  if (auto failure = users_->update(*account)) {
    reply_error(out, connection.id, *failure);
    return;
  }

  // Named field by field with what it became, in the vocabulary update_user
  // already writes: "banned=true muted=false" reads the same way "role=admin
  // password" does, and says which of the four moved rather than only what the
  // account is now under.
  std::string detail;
  const auto note = [&detail](std::string_view name, bool value) {
    if (!detail.empty()) {
      detail += ' ';
    }
    detail += name;
    detail += value ? "=true" : "=false";
  };
  if (after.banned != before.banned) {
    note("banned", after.banned);
  }
  if (after.muted != before.muted) {
    note("muted", after.muted);
  }
  if (after.silenced != before.silenced) {
    note("silenced", after.silenced);
  }
  if (after.screen_share_blocked != before.screen_share_blocked) {
    note("screen_share_blocked", after.screen_share_blocked);
  }
  // The reason goes into the log as well as to the person. A moderation
  // entry that records what was done and not why is the half of the record
  // that nobody has to be told twice.
  if (!message.reason.empty()) {
    detail += " reason=" + message.reason;
  }

  DV_LOG_INFO("{} restricted {}: {}",
              models::user_label(actor->id, actor->display_name, connection.username),
              user_label(message.user_id), detail);
  record(*actor, "restrict_user", message.user_id, rooms_.room_of(message.user_id).value_or(""),
         detail);

  enforce(out, *actor, account->user, before, message.reason);
  out.push_back(Outgoing{.connection = connection.id, .message = user_list()});
}

void Hub::end_session_of(std::vector<Outgoing>& out, const std::string& user_id,
                         const std::string& reason) {
  // Out of the room first. `evict` reads the participant before announcing, so
  // it has to run while the room still holds them.
  if (const auto room_id = rooms_.room_of(user_id)) {
    evict(out, *room_id, user_id, reason);
  }

  authenticator_.revoke_tokens_of(user_id);

  if (const auto target = connection_of_user(user_id)) {
    if (Connection* state = find_connection(*target)) {
      // The presence row goes with the identity, and for the same reason: from
      // here the socket is nobody, so an open session naming this account
      // would be a report that somebody banned two minutes ago is still on the
      // platform. The socket itself stays open - see the note above this
      // function - and if they log in again that is a new session and a new
      // row, which is exactly what it is.
      close_session(*state);
      state->user.reset();
      state->room_id.reset();
      state->username.clear();
      state->delivered_notice_ids.clear();
    }
    user_to_connection_.erase(user_id);
  }
}

void Hub::enforce(std::vector<Outgoing>& out, const models::User& actor, const models::User& target,
                  const models::Restrictions& before, const std::string& reason) {
  const std::string& user_id = target.id;
  const models::Restrictions& after = target.restrictions;
  const std::optional<std::string> room_id = rooms_.room_of(user_id);

  const protocol::UserRestricted announcement{.user_id = user_id,
                                              .restrictions = after,
                                              .by_user_id = actor.id,
                                              .reason = reason,
                                              .room_id = room_id.value_or("")};

  // The person it is about first, and while they still have a session to be
  // told on. A ban that closed the connection first would leave them the only
  // one who never heard why.
  if (const auto target_connection = connection_of_user(user_id)) {
    out.push_back(Outgoing{.connection = *target_connection, .message = announcement});
  }
  if (room_id.has_value()) {
    broadcast(out, *room_id, announcement, user_id);
  }

  // The cached identity on their connection, so that the next message they
  // send is checked against what is now true without waiting for the store
  // lookup each handler does anyway to disagree with what the panel showed.
  if (const auto target_connection = connection_of_user(user_id)) {
    if (Connection* state = find_connection(*target_connection);
        state != nullptr && state->user.has_value()) {
      state->user->restrictions = after;
    }
  }

  if (after.banned && !before.banned) {
    end_session_of(out, user_id, reason.empty() ? "the account was suspended" : reason);
    // Everything below is about a room this account is no longer in.
    return;
  }

  if (!room_id.has_value()) {
    return;
  }

  if (after.muted != before.muted) {
    // Announced as an ordinary mute carrying who did it, the same shape a
    // force_mute produces, so a client that knows nothing about restrictions
    // still draws the microphone correctly.
    if (const auto failure = rooms_.set_muted(*room_id, user_id, after.muted, true); !failure) {
      if (after.muted) {
        broadcast(out, *room_id,
                  protocol::Mute{.room_id = *room_id, .user_id = user_id, .by_user_id = actor.id});
      } else {
        broadcast(
            out, *room_id,
            protocol::Unmute{.room_id = *room_id, .user_id = user_id, .by_user_id = actor.id});
      }
    }
  }

  if (after.screen_share_blocked && !before.screen_share_blocked) {
    const models::Room* room = rooms_.find(*room_id);
    const models::Participant* participant = room == nullptr ? nullptr : room->find(user_id);
    if (participant != nullptr && participant->sharing_screen) {
      // A block that waited for the next attempt would leave whatever is
      // already on everybody's screen there, which is the one thing the
      // administrator was reaching for the control to stop.
      if (const auto failure = rooms_.stop_screen_share(*room_id, user_id); !failure) {
        DV_LOG_INFO("{} stopped the share of {} in room {}", user_label(actor.id),
                    user_label(user_id), room_label(*room_id));
        broadcast(out, *room_id,
                  protocol::ScreenShareStopped{.room_id = *room_id, .user_id = user_id});
      }
    }
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
                                               .participant_count = static_cast<int>(room.size()),
                                               .capacity = room.capacity});
  }
  // The RoomManager holds rooms in a hash map, so its order is whatever the
  // hashing produced. Sorting here means a panel that refreshes does not
  // reshuffle its rows under the cursor.
  std::ranges::sort(list.rooms, {}, &protocol::RoomSummary::id);
  return list;
}

void Hub::broadcast_room_list(std::vector<Outgoing>& out) const {
  // Every authenticated connection, not just whoever caused the change. A room
  // list is only ever asked for once, when a client opens a screen that shows
  // one, and it used to stay frozen from then on: a room created or closed by
  // somebody else left every other client showing a list that no longer
  // matched the server. Joining one of those rows failed with room_not_found,
  // which reads like the server losing rooms and is really the client holding
  // an old answer.
  //
  // Sent on every arrival and departure too, not only when a room appears or
  // goes. The list carries how many people are in each room and it is the
  // first thing somebody sees after signing in, so a count frozen at whatever
  // it was when the room was created is a wrong number on the main screen
  // rather than a stale detail in an administrator's tab. That costs one
  // message per connection per join, which rooms bounded by
  // max_participants_per_room make a rounding error next to the audio each
  // of those connections is relaying.
  const protocol::RoomList list = room_list();
  for (const auto& entry : user_to_connection_) {
    out.push_back(Outgoing{.connection = entry.second, .message = list});
  }
}

void Hub::handle_list_users(std::vector<Outgoing>& out, Connection& connection) {
  out.push_back(Outgoing{.connection = connection.id, .message = user_list()});
}

void Hub::handle_list_rooms(std::vector<Outgoing>& out, Connection& connection) {
  out.push_back(Outgoing{.connection = connection.id, .message = room_list()});
}

void Hub::handle_change_password(std::vector<Outgoing>& out, Connection& connection,
                                 const protocol::ChangePassword& message) {
  const models::User* actor = authenticated(out, connection);
  if (actor == nullptr) {
    return;
  }

  // A copy, not the pointer. Everything below the change signs this connection
  // out, and signing out clears `connection.user` - which is what `actor`
  // points into.
  const models::User account = *actor;

  if (auto failure = authenticator_.change_password(account.id, message.current_password,
                                                    message.new_password)) {
    // Nothing has been written at this point: the authenticator checks the
    // current password before it derives anything. A refusal here leaves the
    // session exactly as it was, which is what lets somebody who mistyped one
    // field try again without logging back in.
    reply_error(out, connection.id, *failure);
    return;
  }

  // Recorded before the session ends, so the entry is written while the
  // account is still resolvable to a username. What it does not record is
  // either password, in any form - the action and who took it is the whole of
  // what an audit log has any business knowing about this.
  DV_LOG_INFO("{} changed their own password",
              models::user_label(account.id, account.display_name, connection.username));
  record(account, "change_password", account.id, {}, "password");

  // Every session of the account, this one included: out of any room, tokens
  // revoked, the identity dropped and the presence row closed. A password is
  // most often changed because the old one is believed to be loose, and a
  // change that leaves the tokens the old one minted alive for another eight
  // hours has not closed the door it was opened to close. The cost is one
  // sign-in, paid by somebody who has just proved they know the new password.
  //
  // Through `end_session_of` and not written out here, which it used to be.
  // That function says why in its own comment: the same four steps
  // transcribed in several places is how two of them quietly stop doing one of
  // them, and this copy had already stopped doing the fourth - it left the
  // session record open, so a database somebody else was reading went on
  // reporting this account as connected.
  //
  // The room is left first either way, because `evict` reads the participant
  // before announcing them gone. The interface only offers this change from
  // the home screen, so a caller in a room is the defensive path rather than
  // the usual one - but "the client would not do that" is not a property the
  // server can rely on.
  end_session_of(out, account.id, "the password was changed");

  // Sent after the session is gone, and it is the only thing that makes the
  // change visible to the client: nothing else about this connection changes
  // shape. Without it the client would find out at its next message, as an
  // `unauthorized` it has no way to explain.
  out.push_back(Outgoing{.connection = connection.id, .message = protocol::PasswordChanged{}});
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
  DV_LOG_INFO("{} created account {} with role {}",
              models::user_label(actor->id, actor->display_name, connection.username),
              models::user_label(user.id, message.display_name, message.username),
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

  // Before the document goes, so that nobody is left talking to an account
  // that no longer exists, and so that the tokens go with it: a session opened
  // a minute ago would otherwise keep working until it expired on its own,
  // which for an account somebody has just decided to remove is exactly the
  // wrong answer.
  end_session_of(out, message.user_id, "the account was removed");

  if (auto failure = users_->remove(message.user_id)) {
    reply_error(out, connection.id, *failure);
    return;
  }

  // After the account has gone, and not allowed to fail the deletion. These
  // are messages written to a named person, so leaving them behind would be a
  // row addressed to an identifier nothing answers to - but the account is
  // already removed by this point, and refusing now would leave the two
  // collections disagreeing in the worse direction.
  if (auto failure = notices_->clear_for(message.user_id)) {
    DV_LOG_ERROR("Notices for the deleted account {} were not removed: {}",
                 models::user_label(message.user_id, account->user.display_name, account->username),
                 failure->message);
  }

  DV_LOG_INFO("{} deleted account {}",
              models::user_label(actor->id, actor->display_name, connection.username),
              models::user_label(message.user_id, account->user.display_name, account->username));
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
  // Read while there is still a room to ask. `remove_room` below takes it away,
  // and the log line comes after that.
  std::string closed_label = message.room_id;
  if (const models::Room* room = rooms_.find(message.room_id); room != nullptr) {
    closed_label = models::room_label(room->id, room->name);
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

  DV_LOG_INFO("{} closed room {}",
              models::user_label(actor->id, actor->display_name, connection.username),
              closed_label);
  record(*actor, "delete_room", message.room_id, message.room_id,
         "participants=" + std::to_string(occupants.size()));
  broadcast_room_list(out);
}

void Hub::handle_list_audit(std::vector<Outgoing>& out, Connection& connection,
                            const protocol::ListAudit& message) {
  out.push_back(Outgoing{
      .connection = connection.id,
      .message = protocol::AuditList{.entries = audit_->list(message.limit, message.actor_id)}});
}

}  // namespace dv::server
