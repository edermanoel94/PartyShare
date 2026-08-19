#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <dv/protocol/message.hpp>

#include "rooms/room_manager.hpp"
#include "signaling/authenticator.hpp"

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

  virtual void on_participant_joined(const std::string& room_id, const models::User& user) = 0;
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
/// ample for the five participants per room the MVP targets.
class Hub {
 public:
  using Clock = Authenticator::Clock;

  struct Options {
    int max_participants_per_room = 5;
    std::chrono::milliseconds heartbeat_interval{5000};
    std::chrono::milliseconds heartbeat_timeout{15000};
    /// Fixing this makes room identifiers reproducible in tests.
    std::optional<std::uint32_t> room_id_seed;
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

  void on_connect(ConnectionId connection, Clock::time_point now);

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
    std::optional<std::string> room_id;
    Clock::time_point last_seen{};
    Clock::time_point last_ping{};
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
                           const std::string& room_id, const std::string& user_id, bool sharing);

  Options options_;
  MediaSignals* media_signals_ = nullptr;
  RoomManager rooms_;
  Authenticator authenticator_;
  std::unordered_map<ConnectionId, Connection> connections_;
  std::unordered_map<std::string, ConnectionId> user_to_connection_;
};

}  // namespace dv::server
