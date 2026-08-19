#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

  void reconnect_loop();

  Options options_;

  mutable std::mutex mutex_;
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
