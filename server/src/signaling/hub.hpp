#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <dv/models/audit.hpp>
#include <dv/protocol/message.hpp>

#include "rooms/room_manager.hpp"
#include "signaling/authenticator.hpp"
#include "signaling/restriction_source.hpp"
#include "store/audit_log.hpp"
#include "store/chat_store.hpp"
#include "store/notice_store.hpp"
#include "store/room_store.hpp"
#include "store/session_store.hpp"
#include "store/user_store.hpp"

namespace dv::server {

using ConnectionId = std::uint64_t;

/// One message addressed at one connection. The Hub never writes to a socket
/// itself: it returns what should be sent, and the transport layer sends it.
/// That is what lets the whole protocol be tested without any networking.
struct Outgoing {
  ConnectionId connection = 0;
  protocol::Message message;
};

/// What the media layer needs to be told about.
///
/// The Hub knows nothing about media: it reports what happened in the room and
/// hands over the frames addressed to the SFU, and something else decides what
/// that means. Keeping it an interface is also what lets the Hub tests assert
/// on media routing without an SFU anywhere near them.
class MediaSignals {
 public:
  MediaSignals() = default;
  virtual ~MediaSignals() = default;

  MediaSignals(const MediaSignals&) = delete;
  MediaSignals& operator=(const MediaSignals&) = delete;
  MediaSignals(MediaSignals&&) = delete;
  MediaSignals& operator=(MediaSignals&&) = delete;

  /// `room_name` is what people call the room and `user_label` is how the
  /// participant should read in a log line. Both are resolved here and handed
  /// over rather than looked up on the other side, because the implementation
  /// runs on its own thread and may not touch the Hub's state to ask.
  virtual void on_participant_joined(const std::string& room_id, const std::string& room_name,
                                     const models::User& user, const std::string& user_label) = 0;
  virtual void on_participant_left(const std::string& room_id, const std::string& user_id) = 0;

  /// An `answer` or `ice_candidate` addressed to protocol::kSfuUserId, already
  /// checked to come from a participant of `room_id`.
  virtual void on_media_signal(const std::string& room_id, const std::string& from_user_id,
                               const protocol::Message& message) = 0;
};

/// Translates the signaling protocol into RoomManager operations.
///
/// Every message that carries a `user_id` or a `from_user_id` is checked
/// against the identity the connection authenticated as. Without that check any
/// participant could mute another, or send an offer in someone else's name.
///
/// Not thread safe. The server serializes all calls onto one mutex, which is
/// ample for rooms of the size this server is configured to allow.
class Hub {
 public:
  using Clock = Authenticator::Clock;

  struct Options {
    /// The largest room anybody may create here. A room's own size is chosen
    /// when it is made; see RoomManager::Options.
    int max_participants_per_room = 20;
    std::chrono::milliseconds heartbeat_interval{5000};
    std::chrono::milliseconds heartbeat_timeout{15000};
    /// Fixing this makes room identifiers reproducible in tests.
    std::optional<std::uint32_t> room_id_seed;

    /// Persistence. All six are null by default, and the Hub then creates
    /// in-memory ones and owns them, which is the server without a database.
    /// A caller that provides one has to keep it alive longer than the Hub.
    store::UserStore* users = nullptr;
    store::RoomStore* rooms = nullptr;
    store::ChatStore* chat = nullptr;
    store::NoticeStore* notices = nullptr;
    store::SessionStore* sessions = nullptr;
    store::AuditLog* audit = nullptr;

    /// Where a restriction written by something other than this server is
    /// noticed. Null means a StoreRestrictionSource over `users`, which is
    /// what every deployment gets; a caller that provides one has to keep it
    /// alive longer than the Hub.
    RestrictionSource* restrictions = nullptr;
  };

  Hub();
  explicit Hub(Options options);

  /// Installs the media layer. Without one, a frame addressed to the SFU is
  /// answered with `media_unavailable` rather than silently dropped.
  ///
  /// The pointer is not owned and has to outlive the Hub.
  void set_media_signals(MediaSignals* signals) noexcept { media_signals_ = signals; }

  /// The connection a user is on, which is how the media layer addresses the
  /// frames it produces. Empty when the user is not connected.
  [[nodiscard]] std::optional<ConnectionId> connection_for_user(const std::string& user_id) const;

  /// Accounts are registered through this. The MVP has no signup flow.
  [[nodiscard]] Authenticator& authenticator() noexcept { return authenticator_; }
  [[nodiscard]] const RoomManager& rooms() const noexcept { return rooms_; }
  [[nodiscard]] RoomManager& rooms() noexcept { return rooms_; }
  [[nodiscard]] store::ChatStore& chat() noexcept { return *chat_; }
  [[nodiscard]] store::AuditLog& audit() noexcept { return *audit_; }

  /// `remote_address` is where the socket came from, as the transport reports
  /// it, and is remembered rather than used: nothing is decided by it here.
  /// It is written into the session record once the connection says who it is,
  /// which is the only reason the Hub is told at all - see
  /// store::SessionStore. An empty one is fine and travels through as empty.
  void on_connect(ConnectionId connection, std::string remote_address, Clock::time_point now);

  /// Handles one received frame. Never throws on malformed input: it answers
  /// with an `error` message instead.
  [[nodiscard]] std::vector<Outgoing> on_message(ConnectionId connection, std::string_view payload,
                                                 Clock::time_point now);

  /// Cleans up after a connection goes away, whether it left politely or the
  /// socket dropped.
  [[nodiscard]] std::vector<Outgoing> on_disconnect(ConnectionId connection, Clock::time_point now);

  /// Emits heartbeat pings and reports connections that stopped answering.
  /// The caller is responsible for closing whatever lands in `timed_out`.
  [[nodiscard]] std::vector<Outgoing> tick(Clock::time_point now,
                                           std::vector<ConnectionId>& timed_out);

  [[nodiscard]] std::size_t connection_count() const noexcept { return connections_.size(); }

 private:
  struct Connection {
    ConnectionId id = 0;
    /// Set once the connection has authenticated.
    std::optional<models::User> user;
    /// The account name behind `user`, remembered at login.
    ///
    /// Here rather than looked up per log line because the store may be a
    /// database, and a log line that costs a query is a log line somebody
    /// eventually deletes. It is also the half of a label that cannot change
    /// while the connection is open: the display name is read from `user` each
    /// time instead, because a participant may rename themselves on the way
    /// into a room.
    std::string username;
    std::optional<std::string> room_id;
    /// Where the socket came from, kept from `on_connect` so that it is still
    /// there when the connection authenticates, which is when there is an
    /// account to attach it to.
    std::string remote_address;
    /// The row in the session store, empty until this connection has an
    /// identity and again once it has lost one.
    ///
    /// Held here and not looked up, because the two moments that need it -
    /// the heartbeat and the socket closing - are both moments where the
    /// alternative is a query per connection. It is also what makes closing
    /// exact: the row this connection opened is closed, and not "the open row
    /// for this account", which after a second login would be somebody else's.
    std::string session_id;
    /// The notices this connection has already been handed, by identifier.
    ///
    /// What keeps the heartbeat from handing them over again. A notice stays
    /// pending in the store until the person acknowledges it, and the pass in
    /// `deliver_notices_written_elsewhere` asks the store for exactly that
    /// set every interval; without this, somebody with a box on their screen
    /// would be given the same box every five seconds until they clicked.
    /// Cleared with the identity, because a session that ends and starts
    /// again is owed everything still pending, which is what the login hands
    /// over.
    std::unordered_set<std::string> delivered_notice_ids;
    Clock::time_point last_seen;
    Clock::time_point last_ping;
  };

  [[nodiscard]] Connection* find_connection(ConnectionId connection);
  [[nodiscard]] std::optional<ConnectionId> connection_of_user(const std::string& user_id) const;

  /// Appends `message` for every participant of `room_id`, skipping `except`
  /// when it is set.
  void broadcast(std::vector<Outgoing>& out, const std::string& room_id,
                 const protocol::Message& message,
                 const std::optional<std::string>& except_user_id = std::nullopt) const;

  static void reply_error(std::vector<Outgoing>& out, ConnectionId connection, const Error& error);

  // One handler per message type. Each returns the messages to send.
  void handle_authenticate(std::vector<Outgoing>& out, Connection& connection,
                           const protocol::Authenticate& message, Clock::time_point now);
  /// The user on an authenticated connection, or nullptr after answering the
  /// caller with an error. Every handler but handle_authenticate starts here.
  [[nodiscard]] static models::User* authenticated(std::vector<Outgoing>& out,
                                                   Connection& connection);

  /// The role an account holds right now, read from the store rather than from
  /// the identity cached on the connection when it logged in.
  ///
  /// One lookup per administrative message, and none for anything else, which
  /// is what makes it affordable. What it buys: promoting or demoting somebody
  /// takes effect on their next action instead of on their next login, so
  /// revoking an administrator is not a request to please reconnect.
  ///
  /// An account that has been deleted answers `Role::User`, the role that can
  /// do least.
  [[nodiscard]] models::Role current_role(const std::string& user_id) const;

  /// What an account may not do right now, read from the store rather than
  /// from the identity the connection logged in with.
  ///
  /// The same reasoning as current_role, and the same cost: a restriction
  /// applied while somebody is mid-call has to hold for their next message,
  /// not for their next login. An account that has been deleted answers an
  /// empty set, because there is nothing left to take away from it and the
  /// handlers that follow will refuse it on other grounds.
  [[nodiscard]] models::Restrictions restrictions_of(const std::string& user_id) const;

  /// How `user_id` should read in a log line: their name and their account,
  /// per `models::user_label`.
  ///
  /// Answers from the open connections first, which costs nothing and covers
  /// every line about somebody who is here. The store is asked only for
  /// somebody who is not, which is a real case and not a defensive one: the
  /// line about a disconnect is written after the connection has been
  /// forgotten. An identifier nothing answers to comes back as itself.
  ///
  /// Never call this from inside a DV_LOG_DEBUG or DV_LOG_TRACE. spdlog's
  /// macros here expand straight to `logger->log(...)` with no level guard, so
  /// the arguments are evaluated even when the level is off, and a store
  /// lookup per suppressed line is a cost nobody can see to remove.
  [[nodiscard]] std::string user_label(const std::string& user_id) const;

  /// What to call `room_id` in a log line: its name, or the identifier itself
  /// when it has none or when no room answers to it. See `models::room_label`.
  [[nodiscard]] std::string room_label(const std::string& room_id) const;

  /// Writes one entry, and complains loudly if it cannot.
  ///
  /// The action goes ahead either way: refusing to remove a disruptive
  /// participant because the audit database is unreachable protects the record
  /// at the expense of the thing being recorded. See docs/13-security.md.
  void record(const models::User& actor, std::string action, std::string target_id,
              std::string room_id, std::string detail);

  /// Takes one participant out of a room and tells everyone, which is what a
  /// kick and a room being closed both come down to.
  void evict(std::vector<Outgoing>& out, const std::string& room_id, const std::string& user_id,
             const std::string& reason);

  /// Replaces the password of the connection's own account, and ends the
  /// session that asked for it.
  ///
  /// The only handler an ordinary user reaches that writes to the account
  /// store. There is no target to check because protocol::ChangePassword has
  /// no target field: the account is whichever one this connection's token
  /// resolved to.
  void handle_change_password(std::vector<Outgoing>& out, Connection& connection,
                              const protocol::ChangePassword& message);
  void handle_create_room(std::vector<Outgoing>& out, Connection& connection,
                          const protocol::CreateRoom& message);
  void handle_join_room(std::vector<Outgoing>& out, Connection& connection,
                        const protocol::JoinRoom& message);
  void handle_leave_room(std::vector<Outgoing>& out, Connection& connection,
                         const protocol::LeaveRoom& message);
  void handle_relay(std::vector<Outgoing>& out, Connection& connection,
                    const protocol::Message& message, const std::string& room_id,
                    const std::string& from_user_id, const std::string& to_user_id);
  void handle_mute(std::vector<Outgoing>& out, Connection& connection, const std::string& room_id,
                   const std::string& user_id, bool muted);
  void handle_screen_share(std::vector<Outgoing>& out, Connection& connection,
                           const std::string& room_id, const std::string& user_id, bool sharing,
                           bool with_audio);

  void handle_chat(std::vector<Outgoing>& out, Connection& connection,
                   const protocol::ChatMessage& message);
  void handle_list_chat(std::vector<Outgoing>& out, Connection& connection,
                        const protocol::ListChat& message);

  /// Writes one administrator's message to one account, and delivers it if
  /// they are here.
  ///
  /// Administration, and the only one of the three notice handlers that is:
  /// see the table in permissions.hpp. The store is written first and the
  /// delivery follows, never the other way round - a notice somebody was shown
  /// and that was never written down is one they cannot acknowledge and one
  /// nobody can find afterwards.
  ///
  /// What is written goes back to the administrator as well, carrying the
  /// identifier and the time the store assigned. That is the confirmation, and
  /// the reason it is the same message rather than a new type: the panel and
  /// the recipient both want the row that now exists, and it is the recipient
  /// field that says which of the two a client is looking at.
  void handle_send_notice(std::vector<Outgoing>& out, Connection& connection,
                          const protocol::SendNotice& message);

  /// One account saying it read one notice. Open to anybody: see
  /// permissions.hpp for why being told something is not a power.
  void handle_acknowledge_notice(std::vector<Outgoing>& out, Connection& connection,
                                 const protocol::AcknowledgeNotice& message);

  /// Hands a connection whatever its account has not answered yet.
  ///
  /// Called at the end of a successful login. A notice written while somebody
  /// was away has no other moment to arrive in, and a notice they were shown
  /// and did not acknowledge arrives again next time, which is the whole
  /// reason the store keeps a timestamp rather than deleting the row.
  void deliver_pending_notices(std::vector<Outgoing>& out, Connection& connection);

  /// Hands every connected account the notices written to it since it was
  /// last looked at.
  ///
  /// The other writer again: tools/dbadmin appends to the notices collection
  /// with no server to hand the notice to, and `handle_send_notice` is never
  /// called for it. Run from `tick`, once per heartbeat, for the same reason
  /// and at the same cost as `apply_restrictions_written_elsewhere`: one
  /// store lookup per connected participant per interval. What each
  /// connection has already been given is remembered on it, so a notice
  /// arrives once and not once per heartbeat until it is acknowledged.
  void deliver_notices_written_elsewhere(std::vector<Outgoing>& out);

  /// Gives one connection one notice, unless it already has it.
  ///
  /// The single place a notice is handed to its recipient, so that the three
  /// moments it can happen - the login, an administrator sending it now, and
  /// the heartbeat finding one written elsewhere - cannot disagree about what
  /// "already delivered" means. Returns whether anything was sent.
  static bool hand_over_notice(std::vector<Outgoing>& out, Connection& connection,
                               const models::Notice& notice);

  /// Zeroes `Account::session_end_requested_at` for one account, and says
  /// whether there was one to zero.
  ///
  /// Two callers: the heartbeat, which has just ended the session the
  /// request was about, and the login, which is arriving after it. In both
  /// the request has been answered, and a request left standing would end
  /// the next session too. `why` goes in the log line, because the two are
  /// different facts about what happened.
  bool discard_session_end_request(const std::string& user_id, std::string_view why);

  /// Records that this connection's account is present, and from where.
  ///
  /// Best effort: a session that could not be written is logged and the person
  /// is let in anyway. See store::SessionStore for why this one is not the
  /// audit log's trade.
  void open_session(Connection& connection);

  /// Ends the session row this connection opened, if it opened one. Safe to
  /// call twice and safe to call for a connection that never authenticated.
  void close_session(Connection& connection);

  /// What was said in `room_id`, as the message that carries it.
  [[nodiscard]] protocol::ChatHistory chat_history(const std::string& room_id, int limit) const;

  // Administration. Each one starts at administrator() above.
  void handle_kick(std::vector<Outgoing>& out, Connection& connection,
                   const protocol::KickUser& message);
  void handle_force_mute(std::vector<Outgoing>& out, Connection& connection,
                         const protocol::ForceMute& message);
  void handle_restrict_user(std::vector<Outgoing>& out, Connection& connection,
                            const protocol::RestrictUser& message);

  /// Makes a set of restrictions true of a call already in progress, and
  /// announces it.
  ///
  /// The write to the store is what makes a restriction survive; this is what
  /// makes it mean something before the account next logs in. A ban ends the
  /// session, a mute takes the microphone, a block on sharing stops a share
  /// that is running. Without it every restriction would be a promise about
  /// the future while the thing it was applied over carried on.
  /// `target` carries the restrictions as they now are; `before` is what they
  /// were, and only what actually moved between the two is acted on. Without
  /// that comparison, lifting a ban would also release a forced mute somebody
  /// applied in the room a minute ago, which is a different administrator's
  /// decision being undone by a message that said nothing about it.
  void enforce(std::vector<Outgoing>& out, const models::User& actor, const models::User& target,
               const models::Restrictions& before, const std::string& reason);

  /// Takes an account's session away: out of its room, tokens revoked, and the
  /// identity dropped from the connection holding it.
  ///
  /// One copy because there are four callers and they have to agree. Deleting
  /// an account, banning one, changing one's own password, and finding that
  /// somebody else deleted one all end the same way, and four transcriptions of
  /// the same steps is how two of them quietly stop revoking tokens. The
  /// password handler was written out by hand for a while and is the evidence:
  /// it had stopped closing the session record.
  ///
  /// The socket is left open on purpose. The connection stops being anybody --
  /// its next message is refused for want of a login -- but a client that is
  /// hung up on cannot be told why, and the announcements above this call are
  /// worth more than the file descriptor.
  ///
  /// The connection is told with `session_ended`, and told first: before the
  /// room hears `user_kicked`, and before the token stops resolving. Why it
  /// has to come first is in the function body. Without it a client in a room
  /// read the kick as "still signed in" and was refused at its next request,
  /// and a client in no room was told nothing at all.
  ///
  /// `reason` is what the room is told, and what the person is told. Safe to
  /// call for an account with no room and for one with no connection.
  void end_session_of(std::vector<Outgoing>& out, const std::string& user_id,
                      const std::string& reason);

  /// Enforces what somebody else wrote into the accounts store.
  ///
  /// The other half of `handle_restrict_user`, for the writer that cannot send
  /// a message. `restrictions_of` already made a restriction written straight
  /// into the database hold for the account's *next* message, which is what
  /// tools/dbadmin's README promises; this is what makes it hold for the
  /// microphone that is already on, the share that is already running and the
  /// session that is already open.
  ///
  /// The same pass answers the two other things that tool can only say by
  /// writing: an account it deleted loses the session it was holding, and an
  /// account it marked with `session_end_requested_at` is signed out, as a
  /// ban would sign it out, without the ban.
  ///
  /// Run from `tick`, so the loop the server already turns is the only thing
  /// driving it, and no second thread reaches a UserStore that is documented
  /// not to be thread safe.
  void apply_restrictions_written_elsewhere(std::vector<Outgoing>& out);

  /// Sends the current room list to every authenticated connection, so that
  /// a room appearing or being closed reaches clients that did not cause it.
  void broadcast_room_list(std::vector<Outgoing>& out) const;

  void handle_list_users(std::vector<Outgoing>& out, Connection& connection);
  void handle_create_user(std::vector<Outgoing>& out, Connection& connection,
                          const protocol::CreateUser& message);
  void handle_update_user(std::vector<Outgoing>& out, Connection& connection,
                          const protocol::UpdateUser& message);
  void handle_delete_user(std::vector<Outgoing>& out, Connection& connection,
                          const protocol::DeleteUser& message);
  void handle_list_rooms(std::vector<Outgoing>& out, Connection& connection);
  void handle_delete_room(std::vector<Outgoing>& out, Connection& connection,
                          const protocol::DeleteRoom& message);
  void handle_list_audit(std::vector<Outgoing>& out, Connection& connection,
                         const protocol::ListAudit& message);

  /// The account list as an administrator sees it, with the salt and the hash
  /// left behind. Sent as the answer to list_users and after every change, so
  /// the panel never shows a state the server has already moved past.
  [[nodiscard]] protocol::UserList user_list() const;
  [[nodiscard]] protocol::RoomList room_list() const;

  Options options_;
  MediaSignals* media_signals_ = nullptr;

  // Declared before everything that uses them, so they are destroyed last.
  // Null when the caller supplied its own, and `users_`, `chat_` and `audit_`
  // are what the rest of the class talks to either way. The room store has no
  // pointer of its own here because nothing but the RoomManager reads it.
  std::unique_ptr<store::UserStore> owned_users_;
  std::unique_ptr<store::RoomStore> owned_rooms_;
  std::unique_ptr<store::ChatStore> owned_chat_;
  std::unique_ptr<store::NoticeStore> owned_notices_;
  std::unique_ptr<store::SessionStore> owned_sessions_;
  std::unique_ptr<store::AuditLog> owned_audit_;
  store::UserStore* users_;
  store::ChatStore* chat_;
  store::NoticeStore* notices_;
  store::SessionStore* sessions_;
  store::AuditLog* audit_;

  // Same arrangement as the stores above, and declared after `users_` because
  // the one built here reads it.
  std::unique_ptr<RestrictionSource> owned_restrictions_;
  RestrictionSource* restrictions_;

  RoomManager rooms_;
  Authenticator authenticator_;
  std::unordered_map<ConnectionId, Connection> connections_;
  std::unordered_map<std::string, ConnectionId> user_to_connection_;
};

}  // namespace dv::server
