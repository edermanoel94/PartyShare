#include "network/signaling_client.hpp"

#include <algorithm>
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
    case SignalingClient::State::Reconnecting:
      return "reconnecting";
  }
  return "unknown";
}

std::chrono::milliseconds reconnect_delay(int attempt, const SignalingClient::Options& options) {
  if (attempt <= 1) {
    return options.reconnect_initial_delay;
  }

  // Doubling, in integers, and stopping at the ceiling before it can overflow.
  auto delay = options.reconnect_initial_delay;
  for (int i = 1; i < attempt; ++i) {
    if (delay >= options.reconnect_max_delay / 2) {
      return options.reconnect_max_delay;
    }
    delay *= 2;
  }
  return std::min(delay, options.reconnect_max_delay);
}

SignalingClient::SignalingClient(Options options)
    : options_(std::move(options)), url_(options_.url), next_url_(options_.url) {}

SignalingClient::~SignalingClient() {
  disconnect();

  {
    const std::lock_guard<std::mutex> lock(retry_mutex_);
    stopping_ = true;
  }
  retry_changed_.notify_all();
  if (retry_thread_.joinable()) {
    retry_thread_.join();
  }

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

void SignalingClient::set_url(std::string url) {
  const std::lock_guard<std::mutex> lock(mutex_);
  next_url_ = std::move(url);
}

std::string SignalingClient::url() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return next_url_;
}

Result<std::monostate> SignalingClient::probe() {
  std::string nonce;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!socket_) {
      return Result<std::monostate>::failure("not_connected", "there is no signaling connection");
    }
    // Stamped before the send rather than after it, so that whatever queueing
    // the send itself costs is counted. It is part of the round trip a message
    // of ours actually takes, and leaving it out would flatter the number.
    nonce = "probe-" + std::to_string(++probes_sent_);
    probe_nonce_ = nonce;
    probe_sent_at_ = std::chrono::steady_clock::now();
  }
  return send(protocol::Ping{nonce});
}

std::optional<std::chrono::milliseconds> SignalingClient::round_trip() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return round_trip_;
}

void SignalingClient::forget_round_trip() {
  const std::lock_guard<std::mutex> lock(mutex_);
  probe_nonce_.clear();
  round_trip_.reset();
}

Result<std::monostate> SignalingClient::connect() {
  std::string url;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (socket_) {
      return Result<std::monostate>::failure("already_connected",
                                             "disconnect() before connecting again");
    }
    url = next_url_;
  }

  if (url.empty()) {
    return Result<std::monostate>::failure("invalid_value", "the signaling URL is empty");
  }
  if (!url.starts_with("ws://") && !url.starts_with("wss://")) {
    return Result<std::monostate>::failure(
        "invalid_value", "the signaling URL must start with ws:// or wss://: " + url);
  }

  {
    // Adopted here and nowhere else, which is what makes set_url mean "from
    // the next sign-in". A reconnection reads url_ and so goes on knocking at
    // the server the call was placed on. Refused addresses above never get
    // this far, so a typo cannot leave the client pointed at nothing.
    const std::lock_guard<std::mutex> lock(mutex_);
    url_ = url;
  }

  {
    const std::lock_guard<std::mutex> lock(retry_mutex_);
    wanted_ = true;
    attempts_ = 0;
    if (options_.auto_reconnect && !retry_thread_.joinable()) {
      retry_thread_ = std::thread([this] { reconnect_loop(); });
    }
  }

  return open_socket();
}

Result<std::monostate> SignalingClient::open_socket() {
  auto socket = std::make_shared<rtc::WebSocket>();

  // Read once, here, and carried into the callbacks by value. The address can
  // be changed from the interface while this socket is up, and every line
  // below is about *this* attempt: a state that named the address somebody has
  // just typed, over a connection to a different server, would be a lie in the
  // one place a person goes to find out where they are connected.
  std::string url;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    socket_ = socket;
    url = url_;
    // A new socket starts with nothing measured on it, including on a retry,
    // where the previous one's number would otherwise survive the gap.
    probe_nonce_.clear();
    round_trip_.reset();
  }

  // Outside the lock, because publishing a state runs the interface's handler
  // and that handler is free to call back into this client.
  set_state(State::Connecting, url);

  // The callbacks outlive this call and run on libdatachannel's threads.
  // Capturing `this` is safe because the destructor closes the socket and
  // clears the handlers before any member goes away.
  socket->onOpen([this, url] {
    {
      const std::lock_guard<std::mutex> lock(retry_mutex_);
      if (attempts_ > 0) {
        reconnects_.fetch_add(1);
      }
      // The next drop starts counting from zero again. A connection that
      // lasted is evidence that the trouble is over.
      attempts_ = 0;
    }
    set_state(State::Connected, url);
  });

  socket->onClosed([this] { handle_drop("the connection was closed by the peer"); });

  socket->onError([this](const std::string& error) { handle_drop(error); });

  socket->onMessage([this](rtc::message_variant data) {
    if (!std::holds_alternative<std::string>(data)) {
      DV_LOG_WARN("Signaling: ignoring a binary frame, the protocol is text only");
      return;
    }
    handle_payload(std::get<std::string>(data));
  });

  socket->open(normalize_url(url));
  return std::monostate{};
}

void SignalingClient::handle_drop(const std::string& detail) {
  // A close after `disconnect()` is the expected end of the session, not a
  // failure, and disconnect() has already published Disconnected.
  if (state_.load() == State::Disconnected) {
    return;
  }

  // Before the retry is even scheduled. Whoever asks between the drop and the
  // next socket has to be told there is no measurement, not handed the one
  // from the connection that has just gone.
  forget_round_trip();

  bool retry = false;
  std::chrono::milliseconds wait{0};
  {
    const std::lock_guard<std::mutex> lock(retry_mutex_);
    if (wanted_ && options_.auto_reconnect &&
        (options_.max_reconnect_attempts <= 0 || attempts_ < options_.max_reconnect_attempts)) {
      ++attempts_;
      wait = reconnect_delay(attempts_, options_);
      retry_at_ = std::chrono::steady_clock::now() + wait;
      retry = true;
    }
  }

  if (!retry) {
    set_state(State::Failed, detail);
    return;
  }

  set_state(State::Reconnecting, detail + ", retrying in " + std::to_string(wait.count()) + " ms");
  retry_changed_.notify_all();
}

void SignalingClient::reconnect_loop() {
  std::unique_lock<std::mutex> lock(retry_mutex_);
  while (!stopping_) {
    if (retry_at_ == std::chrono::steady_clock::time_point{}) {
      retry_changed_.wait(lock);
      continue;
    }

    const auto due = retry_at_;
    if (retry_changed_.wait_until(lock, due) != std::cv_status::timeout) {
      // Woken early: either the schedule changed or we are shutting down.
      continue;
    }
    if (stopping_ || !wanted_ || retry_at_ != due) {
      continue;
    }
    retry_at_ = {};

    lock.unlock();
    {
      // The old socket is dropped first. libdatachannel will not reuse one
      // that has closed, and holding it would leak a thread per attempt.
      std::shared_ptr<rtc::WebSocket> old;
      {
        const std::lock_guard<std::mutex> socket_lock(mutex_);
        old.swap(socket_);
      }
      if (old) {
        old->resetCallbacks();
      }
      (void)open_socket();
    }
    lock.lock();
  }
}

void SignalingClient::disconnect() {
  {
    // Asked for, so no drop after this counts as something to recover from.
    const std::lock_guard<std::mutex> lock(retry_mutex_);
    wanted_ = false;
    retry_at_ = {};
  }
  retry_changed_.notify_all();

  std::shared_ptr<rtc::WebSocket> socket;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    socket.swap(socket_);
    probe_nonce_.clear();
    round_trip_.reset();
  }
  if (!socket) {
    set_state(State::Disconnected, "closed by the client");
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

  // A pong answering a probe of ours is a measurement, not a message: nothing
  // above this layer asked for it and nothing above it knows what to do with
  // it. One that does not match is somebody else's, or ours from a connection
  // that has since dropped, and is dropped rather than timed.
  if (const auto* pong = std::get_if<protocol::Pong>(&message)) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (!probe_nonce_.empty() && pong->nonce == probe_nonce_) {
      round_trip_ = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - probe_sent_at_);
      probe_nonce_.clear();
      return;
    }
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
