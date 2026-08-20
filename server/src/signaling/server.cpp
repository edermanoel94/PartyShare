#include "signaling/server.hpp"

#include <chrono>
#include <utility>
#include <variant>

#include <dv/logging/logger.hpp>

namespace dv::server {

SignalingServer::SignalingServer(Options options)
    : options_(std::move(options)), hub_(options_.hub) {
  if (!options_.enable_sfu) {
    return;
  }

  router_ = std::make_unique<sfu::MediaRouter>(options_.sfu);
  router_->on_signal([this](const std::string& user_id, const protocol::Message& message) {
    send_to_user(user_id, message);
  });
  hub_.set_media_signals(router_.get());
}

SignalingServer::~SignalingServer() {
  stop();
}

void SignalingServer::start() {
  rtc::WebSocketServer::Configuration configuration;
  configuration.port = options_.port;
  if (!options_.bind_address.empty() && options_.bind_address != "0.0.0.0") {
    configuration.bindAddress = options_.bind_address;
  }
  configuration.enableTls = false;  // TLS is terminated upstream for now

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    server_ = std::make_unique<rtc::WebSocketServer>(configuration);
    server_->onClient([this](const std::shared_ptr<rtc::WebSocket>& socket) { on_client(socket); });
  }

  running_ = true;
  heartbeat_ = std::thread([this] { heartbeat_loop(); });

  DV_LOG_INFO("Signaling server listening on {}:{}", options_.bind_address, port());
}

void SignalingServer::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (heartbeat_.joinable()) {
    heartbeat_.join();
  }

  std::unordered_map<ConnectionId, std::shared_ptr<rtc::WebSocket>> sockets;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (server_) {
      server_->stop();
    }
    sockets.swap(sockets_);
  }

  // Closed outside the lock: libdatachannel runs the close callbacks, which
  // come back into this object and take the same mutex.
  for (auto& [id, socket] : sockets) {
    socket->close();
  }

  const std::lock_guard<std::mutex> lock(mutex_);
  server_.reset();
  DV_LOG_INFO("Signaling server stopped");
}

std::uint16_t SignalingServer::port() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return server_ ? server_->port() : options_.port;
}

Result<models::User> SignalingServer::add_user(const std::string& username,
                                               const std::string& password,
                                               const std::string& display_name) {
  const std::lock_guard<std::mutex> lock(mutex_);
  return hub_.authenticator().add_user(username, password, display_name);
}

std::size_t SignalingServer::connection_count() {
  const std::lock_guard<std::mutex> lock(mutex_);
  return hub_.connection_count();
}

void SignalingServer::on_client(const std::shared_ptr<rtc::WebSocket>& socket) {
  ConnectionId id = 0;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    id = next_connection_id_++;
    sockets_.emplace(id, socket);
  }

  socket->onOpen([this, id] {
    const std::lock_guard<std::mutex> lock(mutex_);
    hub_.on_connect(id, Hub::Clock::now());
  });

  socket->onMessage([this, id](rtc::message_variant data) {
    if (!std::holds_alternative<std::string>(data)) {
      // The protocol is text only. A binary frame is a client bug, and is
      // answered rather than silently dropped.
      const std::lock_guard<std::mutex> lock(mutex_);
      dispatch({Outgoing{
          .connection = id,
          .message = protocol::ErrorMessage{
              .code = "invalid_json", .message = "binary frames are not part of this protocol"}}});
      return;
    }

    const std::string& payload = std::get<std::string>(data);
    const std::lock_guard<std::mutex> lock(mutex_);
    dispatch(hub_.on_message(id, payload, Hub::Clock::now()));
  });

  socket->onClosed([this, id] {
    std::vector<Outgoing> out;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      out = hub_.on_disconnect(id, Hub::Clock::now());
      dispatch(out);
      sockets_.erase(id);
    }
  });

  socket->onError([id](const std::string& error) {
    DV_LOG_WARN("Connection {} reported an error: {}", id, error);
  });
}

void SignalingServer::send_to_user(const std::string& user_id, const protocol::Message& message) {
  const std::lock_guard<std::mutex> lock(mutex_);
  const auto connection = hub_.connection_for_user(user_id);
  if (!connection.has_value()) {
    // The participant left between the SFU producing this and it being sent.
    DV_LOG_DEBUG("Dropping a {} for {}, who is no longer connected",
                 protocol::type_name(protocol::type_of(message)), user_id);
    return;
  }
  dispatch({Outgoing{.connection = *connection, .message = message}});
}

void SignalingServer::dispatch(const std::vector<Outgoing>& messages) {
  for (const Outgoing& outgoing : messages) {
    const auto it = sockets_.find(outgoing.connection);
    if (it == sockets_.end() || !it->second->isOpen()) {
      continue;
    }

    // isOpen() above is a hint, not a guarantee: the peer can close between
    // the check and the send, and libdatachannel signals that by throwing.
    // Losing one frame to a socket that is going away is not a reason to take
    // the server down.
    try {
      it->second->send(protocol::serialize(outgoing.message));
      // maybe_unused for the same reason as the connection state handler: the
      // only use of `error` is the debug log, and SPDLOG_DEBUG compiles to
      // nothing in a release build. MSVC says so and GCC does not, because GCC
      // does not warn about unused handler parameters.
    } catch ([[maybe_unused]] const std::exception& error) {
      DV_LOG_DEBUG("Could not send a {} on connection {}: {}",
                   protocol::type_name(protocol::type_of(outgoing.message)), outgoing.connection,
                   error.what());
    }
  }
}

void SignalingServer::close_connection(ConnectionId id) {
  std::shared_ptr<rtc::WebSocket> socket;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    const auto it = sockets_.find(id);
    if (it == sockets_.end()) {
      return;
    }
    socket = it->second;
  }
  socket->close();
}

void SignalingServer::heartbeat_loop() {
  // Woken more often than the heartbeat interval so that stop() is responsive.
  constexpr auto kPollInterval = std::chrono::milliseconds(100);
  auto next_tick = std::chrono::steady_clock::now();

  while (running_) {
    std::this_thread::sleep_for(kPollInterval);
    const auto now = std::chrono::steady_clock::now();
    if (now < next_tick) {
      continue;
    }
    next_tick = now + options_.hub.heartbeat_interval;

    std::vector<ConnectionId> timed_out;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      dispatch(hub_.tick(now, timed_out));
    }

    for (const ConnectionId id : timed_out) {
      DV_LOG_WARN("Connection {} timed out, closing", id);
      close_connection(id);
    }
  }
}

}  // namespace dv::server
