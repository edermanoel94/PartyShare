#pragma once

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <variant>

#include <rtc/rtc.hpp>

#include <dv/protocol/message.hpp>

namespace dv::testing {

/// A real WebSocket client, used to drive the signaling server end to end.
///
/// Everything waits on a condition variable rather than sleeping, so the tests
/// stay fast when the server answers quickly and do not become flaky when the
/// machine is loaded.
class WebSocketTestClient {
 public:
  using Clock = std::chrono::steady_clock;

  explicit WebSocketTestClient(std::uint16_t port) {
    socket_ = std::make_shared<rtc::WebSocket>();

    socket_->onOpen([this] {
      const std::lock_guard<std::mutex> lock(mutex_);
      open_ = true;
      changed_.notify_all();
    });

    socket_->onClosed([this] {
      const std::lock_guard<std::mutex> lock(mutex_);
      closed_ = true;
      changed_.notify_all();
    });

    socket_->onMessage([this](rtc::message_variant data) {
      if (!std::holds_alternative<std::string>(data)) {
        return;
      }
      auto parsed = protocol::parse(std::get<std::string>(data));
      if (!parsed) {
        return;
      }
      const std::lock_guard<std::mutex> lock(mutex_);
      received_.push_back(std::move(parsed).take());
      changed_.notify_all();
    });

    socket_->open("ws://127.0.0.1:" + std::to_string(port) + "/");
  }

  ~WebSocketTestClient() {
    if (socket_) {
      socket_->forceClose();
    }
  }

  WebSocketTestClient(const WebSocketTestClient&) = delete;
  WebSocketTestClient& operator=(const WebSocketTestClient&) = delete;
  WebSocketTestClient(WebSocketTestClient&&) = delete;
  WebSocketTestClient& operator=(WebSocketTestClient&&) = delete;

  [[nodiscard]] bool wait_until_open(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return open_; });
  }

  [[nodiscard]] bool wait_until_closed(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mutex_);
    return changed_.wait_for(lock, timeout, [this] { return closed_; });
  }

  void send(const protocol::Message& message) { socket_->send(protocol::serialize(message)); }

  void send_raw(const std::string& payload) { socket_->send(payload); }

  /// Waits for the first message of type T, consuming everything before it.
  template <typename T>
  [[nodiscard]] std::optional<T> wait_for(std::chrono::milliseconds timeout) {
    const auto deadline = Clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      while (!received_.empty()) {
        protocol::Message message = std::move(received_.front());
        received_.pop_front();
        if (std::holds_alternative<T>(message)) {
          return std::get<T>(message);
        }
      }
      if (changed_.wait_until(lock, deadline) == std::cv_status::timeout && received_.empty()) {
        return std::nullopt;
      }
    }
  }

  /// Waits for the first message of type T that `matches`, consuming
  /// everything before it.
  ///
  /// Where a client is sent several announcements of the same kind and only
  /// one of them is the one under test. The room list is pushed on every
  /// arrival and departure, so "the next room list" stopped meaning "the room
  /// list caused by what I just did": the queue can already hold one from
  /// somebody joining a moment earlier.
  template <typename T, typename Predicate>
  [[nodiscard]] std::optional<T> wait_for_matching(std::chrono::milliseconds timeout,
                                                   Predicate matches) {
    const auto deadline = Clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    while (true) {
      while (!received_.empty()) {
        protocol::Message message = std::move(received_.front());
        received_.pop_front();
        if (std::holds_alternative<T>(message) && matches(std::get<T>(message))) {
          return std::get<T>(message);
        }
      }
      if (changed_.wait_until(lock, deadline) == std::cv_status::timeout && received_.empty()) {
        return std::nullopt;
      }
    }
  }

  /// Collects messages of type T until `timeout` elapses. Used where the order
  /// of several announcements matters.
  template <typename T>
  [[nodiscard]] std::vector<T> collect(std::chrono::milliseconds timeout, std::size_t expected) {
    std::vector<T> found;
    const auto deadline = Clock::now() + timeout;
    std::unique_lock<std::mutex> lock(mutex_);
    while (found.size() < expected) {
      while (!received_.empty() && found.size() < expected) {
        protocol::Message message = std::move(received_.front());
        received_.pop_front();
        if (std::holds_alternative<T>(message)) {
          found.push_back(std::get<T>(message));
        }
      }
      if (found.size() >= expected) {
        break;
      }
      if (changed_.wait_until(lock, deadline) == std::cv_status::timeout && received_.empty()) {
        break;
      }
    }
    return found;
  }

  /// Asserts that nothing of type T shows up within the window.
  template <typename T>
  [[nodiscard]] bool none_arrives(std::chrono::milliseconds window) {
    return !wait_for<T>(window).has_value();
  }

  /// Drops the connection without a closing handshake, the way a crashed or
  /// unplugged client would.
  void force_close() { socket_->forceClose(); }

  void close() { socket_->close(); }

 private:
  std::shared_ptr<rtc::WebSocket> socket_;
  std::mutex mutex_;
  std::condition_variable changed_;
  std::deque<protocol::Message> received_;
  bool open_ = false;
  bool closed_ = false;
};

}  // namespace dv::testing
