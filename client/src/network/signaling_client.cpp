#include "network/signaling_client.hpp"

#include <utility>
#include <variant>

#include <rtc/rtc.hpp>

#include <dv/logging/logger.hpp>

namespace dv::client {
namespace {

/// libdatachannel needs a path, and the server serves the protocol at the root.
[[nodiscard]] std::string normalize_url(const std::string& url) {
  const std::size_t scheme_end = url.find("://");
  if (scheme_end == std::string::npos) {
    return url;
  }
  return url.find('/', scheme_end + 3) == std::string::npos ? url + "/" : url;
}

}  // namespace

std::string_view to_string(SignalingClient::State state) noexcept {
  switch (state) {
    case SignalingClient::State::Disconnected:
      return "disconnected";
    case SignalingClient::State::Connecting:
      return "connecting";
    case SignalingClient::State::Connected:
      return "connected";
    case SignalingClient::State::Failed:
      return "failed";
  }
  return "unknown";
}

SignalingClient::SignalingClient(Options options) : options_(std::move(options)) {}

SignalingClient::~SignalingClient() {
  disconnect();

  // disconnect() has already detached the socket callbacks, so nothing can be
  // in flight into this object any more. Clearing the handlers is what makes
  // the guarantee complete: once this returns, neither of them will run again.
  const std::lock_guard<std::mutex> lock(mutex_);
  message_handler_ = nullptr;
  state_handler_ = nullptr;
}

void SignalingClient::on_message(MessageHandler handler) {
  const std::lock_guard<std::mutex> lock(mutex_);
  message_handler_ = std::move(handler);
}

void SignalingClient::on_state(StateHandler handler) {
  const std::lock_guard<std::mutex> lock(mutex_);
  state_handler_ = std::move(handler);
}

Result<std::monostate> SignalingClient::connect() {
  if (options_.url.empty()) {
    return Result<std::monostate>::failure("invalid_value", "the signaling URL is empty");
  }
  if (options_.url.rfind("ws://", 0) != 0 && options_.url.rfind("wss://", 0) != 0) {
    return Result<std::monostate>::failure(
        "invalid_value", "the signaling URL must start with ws:// or wss://: " + options_.url);
  }

  std::shared_ptr<rtc::WebSocket> socket;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (socket_) {
      return Result<std::monostate>::failure("already_connected",
                                             "disconnect() before connecting again");
    }
    socket = std::make_shared<rtc::WebSocket>();
    socket_ = socket;
  }

  set_state(State::Connecting, options_.url);

  // The callbacks outlive this call and run on libdatachannel's threads.
  // Capturing `this` is safe because the destructor closes the socket and
  // clears the handlers before any member goes away.
  socket->onOpen([this] { set_state(State::Connected, options_.url); });

  socket->onClosed([this] {
    // A close after `disconnect()` is the expected end of the session, not a
    // failure, and disconnect() has already published Disconnected.
    if (state_.load() != State::Disconnected) {
      set_state(State::Failed, "the connection was closed by the peer");
    }
  });

  socket->onError([this](const std::string& error) { set_state(State::Failed, error); });

  socket->onMessage([this](rtc::message_variant data) {
    if (!std::holds_alternative<std::string>(data)) {
      DV_LOG_WARN("Signaling: ignoring a binary frame, the protocol is text only");
      return;
    }
    handle_payload(std::get<std::string>(data));
  });

  socket->open(normalize_url(options_.url));
  return std::monostate{};
}

void SignalingClient::disconnect() {
  std::shared_ptr<rtc::WebSocket> socket;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    socket.swap(socket_);
  }
  if (!socket) {
    return;
  }

  // Published before closing so that the onClosed callback can tell a
  // deliberate shutdown from a connection that dropped underneath us.
  set_state(State::Disconnected, "closed by the client");

  // Detaching the callbacks before closing is what makes destruction safe:
  // libdatachannel delivers from its own threads, and without this a message
  // already on its way would reach an object that is going away.
  //
  // It waits for a callback that is currently running, so this must not be
  // called from inside one.
  socket->resetCallbacks();

  // Closed outside the lock: libdatachannel runs the close callback inline,
  // and that callback comes back into this object.
  socket->close();
}

Result<std::monostate> SignalingClient::send(const protocol::Message& message) {
  std::shared_ptr<rtc::WebSocket> socket;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    socket = socket_;
  }
  if (!socket || !socket->isOpen()) {
    return Result<std::monostate>::failure(
        "not_connected", std::string("cannot send ") +
                             std::string(protocol::type_name(protocol::type_of(message))) +
                             ": the signaling connection is not open");
  }

  socket->send(protocol::serialize(message));
  return std::monostate{};
}

void SignalingClient::handle_payload(const std::string& payload) {
  Result<protocol::Message> parsed = protocol::parse(payload);
  if (!parsed) {
    // The server is the only thing on the other end, so this means the two
    // sides disagree about the protocol. Worth a log, not worth dropping the
    // connection over.
    DV_LOG_WARN("Signaling: discarding an unparseable frame [{}]: {}", parsed.error().code,
                parsed.error().message);
    return;
  }

  protocol::Message message = std::move(parsed).take();

  // Answered here rather than passed up. The heartbeat of section 13 of
  // SPEC.md is transport level: the server drops a connection that stops
  // replying, so this must not depend on a handler being installed, or on
  // whatever the layer above happens to be doing.
  if (const auto* ping = std::get_if<protocol::Ping>(&message)) {
    if (const auto sent = send(protocol::Pong{ping->nonce}); !sent) {
      DV_LOG_WARN("Signaling: could not answer a ping: {}", sent.error().message);
      return;
    }
    pings_answered_.fetch_add(1);
    return;
  }

  MessageHandler handler;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    handler = message_handler_;
  }
  if (handler) {
    handler(std::move(message));
  }
}

void SignalingClient::set_state(State state, const std::string& detail) {
  const State previous = state_.exchange(state);
  if (previous == state) {
    return;
  }

  DV_LOG_DEBUG("Signaling: {} -> {} ({})", to_string(previous), to_string(state), detail);

  StateHandler handler;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    handler = state_handler_;
  }
  if (handler) {
    handler(state, detail);
  }
}

}  // namespace dv::client
