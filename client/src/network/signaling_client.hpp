#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
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
/// Reconnection with backoff belongs to M7. Until then a dropped connection is
/// reported through the state callback and stays down.
class SignalingClient {
 public:
  enum class State : std::uint8_t {
    Disconnected,
    Connecting,
    Connected,
    /// The connection was lost or refused. `connect()` may be called again.
    Failed,
  };

  struct Options {
    /// For example ws://127.0.0.1:8080.
    std::string url;
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

 private:
  void handle_payload(const std::string& payload);
  void set_state(State state, const std::string& detail);

  Options options_;

  mutable std::mutex mutex_;
  std::shared_ptr<rtc::WebSocket> socket_;
  MessageHandler message_handler_;
  StateHandler state_handler_;

  std::atomic<State> state_{State::Disconnected};
  std::atomic<std::uint64_t> pings_answered_{0};
};

[[nodiscard]] std::string_view to_string(SignalingClient::State state) noexcept;

}  // namespace dv::client
