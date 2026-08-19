#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <rtc/rtc.hpp>

#include "sfu/media_router.hpp"
#include "signaling/hub.hpp"

namespace dv::server {

/// WebSocket transport for the signaling protocol.
///
/// The WebSocket implementation comes from libdatachannel, which is already the
/// dependency the SFU will use in M4. Using it here too means one networking
/// stack instead of two.
///
/// libdatachannel delivers callbacks from its own threads, so every touch of
/// the Hub goes through `mutex_`. The Hub itself stays single threaded by
/// construction, which keeps the protocol logic free of locking.
class SignalingServer {
 public:
  struct Options {
    std::string bind_address = "0.0.0.0";
    /// Port 0 asks the operating system for a free port. `port()` then reports
    /// which one was chosen, which is what the integration tests use.
    std::uint16_t port = 8080;
    Hub::Options hub;
    /// Media routing. Turning it off leaves a signaling only server, which is
    /// what the M2 tests drive and what a deployment that relays nothing but
    /// negotiation would run.
    bool enable_sfu = true;
    sfu::MediaRouter::Options sfu;
  };

  explicit SignalingServer(Options options);
  ~SignalingServer();

  SignalingServer(const SignalingServer&) = delete;
  SignalingServer& operator=(const SignalingServer&) = delete;
  SignalingServer(SignalingServer&&) = delete;
  SignalingServer& operator=(SignalingServer&&) = delete;

  void start();
  void stop();

  [[nodiscard]] std::uint16_t port() const;

  /// Accounts have to be registered before clients can authenticate. The MVP
  /// has no signup flow, so this is how users come into existence.
  [[nodiscard]] Result<models::User> add_user(const std::string& username,
                                              const std::string& password,
                                              const std::string& display_name);

  [[nodiscard]] std::size_t connection_count();

  /// Null when the server was configured without media routing.
  [[nodiscard]] sfu::MediaRouter* media_router() noexcept { return router_.get(); }

 private:
  void on_client(std::shared_ptr<rtc::WebSocket> socket);
  /// Sends one frame the SFU produced to the connection its user is on.
  void send_to_user(const std::string& user_id, const protocol::Message& message);
  /// Sends everything the Hub produced. Must be called with `mutex_` held.
  void dispatch(const std::vector<Outgoing>& messages);
  void close_connection(ConnectionId id);
  void heartbeat_loop();

  Options options_;
  Hub hub_;
  /// Declared after the Hub so that it is destroyed first: the Hub holds a
  /// pointer to it while media routing is on.
  std::unique_ptr<sfu::MediaRouter> router_;

  mutable std::mutex mutex_;
  std::unique_ptr<rtc::WebSocketServer> server_;
  std::unordered_map<ConnectionId, std::shared_ptr<rtc::WebSocket>> sockets_;
  ConnectionId next_connection_id_ = 1;

  std::atomic<bool> running_{false};
  std::thread heartbeat_;
};

}  // namespace dv::server
