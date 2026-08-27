#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <variant>

#include <dv/core/result.hpp>
#include <dv/protocol/message.hpp>

namespace rtc {
class WebSocket;
}  // namespace rtc

namespace dv::client {

/// The client end of the signaling protocol.
///
/// The WebSocket comes from libdatachannel, the same library the server uses,
/// which keeps one networking stack in the project instead of two.
///
/// Nothing here knows about Qt, and nothing here runs on the UI thread: the
/// callbacks are invoked from libdatachannel's own threads. Whoever wires this
/// into the interface is responsible for hopping threads, which in Qt means a
/// queued connection. See section 15 of SPEC.md for the layering rule.
///
/// A connection that drops is retried on its own, with the delay growing after
/// each failure. Only `disconnect()` stops it: a server that goes away comes
/// back, and the client should be waiting when it does.
class SignalingClient {
 public:
  enum class State : std::uint8_t {
    Disconnected,
    Connecting,
    Connected,
    /// The connection was lost or refused. `connect()` may be called again.
    Failed,
    /// Down, and a retry is already scheduled. Apart from `Failed` because the
    /// interface has something different to say about each: one is over, the
    /// other is waiting.
    Reconnecting,
  };

  struct Options {
    /// For example ws://127.0.0.1:8080.
    ///
    /// Where to start. Once the client exists, `url()` is the answer to which
    /// server it is pointed at, because `set_url` can move it.
    std::string url;

    /// Retry on its own when the connection drops or is refused.
    bool auto_reconnect = true;
    /// How long to wait before the first retry. Short, because most drops are
    /// a moment of network and not a server that went away.
    std::chrono::milliseconds reconnect_initial_delay{500};
    /// The ceiling the delay grows to. A client left running overnight against
    /// a server that is gone should knock once every half minute, not every
    /// half second.
    std::chrono::milliseconds reconnect_max_delay{30000};
    /// How many attempts before giving up and reporting `Failed`. Zero means
    /// never give up.
    int max_reconnect_attempts = 0;
  };

  using MessageHandler = std::function<void(protocol::Message)>;
  using StateHandler = std::function<void(State, const std::string& detail)>;

  explicit SignalingClient(Options options);
  ~SignalingClient();

  SignalingClient(const SignalingClient&) = delete;
  SignalingClient& operator=(const SignalingClient&) = delete;
  SignalingClient(SignalingClient&&) = delete;
  SignalingClient& operator=(SignalingClient&&) = delete;

  /// Both handlers have to be installed before `connect()`. Installing them
  /// later races with messages that have already arrived.
  ///
  /// They run on libdatachannel's threads, so whatever they capture has to
  /// outlive this client. The destructor guarantees that neither runs again
  /// once it returns, and that is the only guarantee there is: destroy the
  /// client before what its handlers reach into.
  void on_message(MessageHandler handler);
  void on_state(StateHandler handler);

  /// Starts connecting and returns immediately. Progress arrives through the
  /// state handler. Fails only when the URL is unusable.
  [[nodiscard]] Result<std::monostate> connect();

  void disconnect();

  /// Points this client at a different server, from the next `connect()` on.
  ///
  /// Deliberately not from the next *reconnection*. A socket that drops in the
  /// middle of a call has to come back on the server that call was placed on,
  /// and an address changed in the interface while a call is running would
  /// otherwise move it silently at the first hiccup - to a server that has
  /// never heard of the room. So the new address is held, and `connect()` is
  /// what adopts it, which is the same moment somebody signs in.
  ///
  /// The address is not validated here. `connect()` is where an empty or
  /// non-ws:// one is refused, because that is where it becomes an attempt.
  void set_url(std::string url);

  /// The address the next `connect()` will use.
  ///
  /// Not always the one the socket in hand is on: see `set_url`.
  [[nodiscard]] std::string url() const;

  /// Sends a ping of our own and starts timing it.
  ///
  /// The server's heartbeat measures the server's view: it asks, we answer,
  /// and nothing about the round trip reaches this side. This is the other
  /// direction, and it is the only measurement available when there is no call
  /// - which is exactly when somebody is sitting on the lobby screen wondering
  /// whether the network is any good.
  ///
  /// Section 4.1 of docs/06-protocol.md exempts the heartbeat from the
  /// authentication gate, so this answers on the login screen too.
  ///
  /// Fails with `not_connected` when there is no socket. Calling it again
  /// before the last one came back is not an error: the reply is matched by
  /// nonce, so a lost ping is left behind rather than mistimed.
  [[nodiscard]] Result<std::monostate> probe();

  /// The round trip of the last probe that came back, or nothing.
  ///
  /// Nothing means it has not been measured *now*: no probe yet, none
  /// answered, or the socket dropped. Deliberately not the last known value
  /// kept warm - a number from a connection that no longer exists is the one
  /// answer worse than no number, because it looks current.
  [[nodiscard]] std::optional<std::chrono::milliseconds> round_trip() const;

  /// Queues one message. Fails with `not_connected` when the socket is not
  /// open, which callers are expected to handle rather than assume away.
  [[nodiscard]] Result<std::monostate> send(const protocol::Message& message);

  [[nodiscard]] State state() const noexcept { return state_.load(); }
  [[nodiscard]] bool is_connected() const noexcept { return state_.load() == State::Connected; }

  /// Number of `ping` frames answered so far. Exposed for the tests, which use
  /// it to prove the client survives the server's heartbeat.
  [[nodiscard]] std::uint64_t pings_answered() const noexcept { return pings_answered_.load(); }

  /// How many times the connection has been re-established after dropping.
  [[nodiscard]] std::uint64_t reconnects() const noexcept { return reconnects_.load(); }

 private:
  void handle_payload(const std::string& payload);
  void set_state(State state, const std::string& detail);

  /// Opens a socket. Must be called without `mutex_` held: publishing a state
  /// runs the interface's handler, which is free to call back in here.
  [[nodiscard]] Result<std::monostate> open_socket();

  /// Called when a socket closes or errors. Decides between giving up and
  /// scheduling another attempt.
  void handle_drop(const std::string& detail);

  /// Throws away the round trip and any probe in flight.
  ///
  /// Called wherever the socket changes, which is the whole rule: the number
  /// describes one connection, so it dies with that connection rather than
  /// carrying over to the next one or outliving all of them.
  void forget_round_trip();

  void reconnect_loop();

  Options options_;

  mutable std::mutex mutex_;

  /// The address the socket in hand was opened on, and the one a reconnection
  /// will use. Guarded, because the retry thread reads it while the interface
  /// thread may be setting the one below.
  std::string url_;
  /// What the next `connect()` will adopt, which is `url_` until somebody
  /// changes the server in the settings dialog.
  std::string next_url_;

  /// The nonce of the probe in flight and when it left, or empty when there is
  /// none. One at a time: a probe every few seconds against a round trip
  /// measured in milliseconds has no reason to overlap, and one outstanding
  /// nonce is what makes a late reply to an abandoned ping impossible to
  /// mistake for a fast reply to the current one.
  std::string probe_nonce_;
  std::chrono::steady_clock::time_point probe_sent_at_;
  /// The last round trip measured on the socket in hand. Reset whenever that
  /// socket goes, because it describes that socket and nothing else.
  std::optional<std::chrono::milliseconds> round_trip_;
  /// Numbers the probe nonces, so that a reply can only match the ping it
  /// answers. Never reset: a nonce reused across a reconnection is a reply
  /// from the old connection that fits the new one.
  std::uint64_t probes_sent_ = 0;

  std::shared_ptr<rtc::WebSocket> socket_;
  MessageHandler message_handler_;
  StateHandler state_handler_;

  std::atomic<State> state_{State::Disconnected};
  std::atomic<std::uint64_t> pings_answered_{0};
  std::atomic<std::uint64_t> reconnects_{0};

  /// Guards the retry schedule. Separate from `mutex_` because the retry
  /// thread has to be able to wake up while a callback holds the other one.
  std::mutex retry_mutex_;
  std::condition_variable retry_changed_;
  std::thread retry_thread_;
  /// True between connect() and disconnect(). A drop only schedules a retry
  /// while this holds: a deliberate disconnect is not a failure to recover
  /// from.
  bool wanted_ = false;
  bool stopping_ = false;
  int attempts_ = 0;
  std::chrono::steady_clock::time_point retry_at_;
};

/// How long to wait before attempt number `attempt`, counting from one.
///
/// Doubles each time and stops at the ceiling. Exposed because this is the
/// part worth testing on its own: everything else about reconnection needs a
/// server to go away.
[[nodiscard]] std::chrono::milliseconds reconnect_delay(int attempt,
                                                        const SignalingClient::Options& options);

[[nodiscard]] std::string_view to_string(SignalingClient::State state) noexcept;

}  // namespace dv::client
