// The pieces every test that drives real media needs: a signaling server with
// the SFU on, and a client that is the real CallSession over the real
// libwebrtc.
//
// Shared by the end to end tests and by the benchmarks, which need the same
// setup and differ only in what they measure.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "app/call_session.hpp"
#include "sfu/media_router.hpp"
#include "signaling/server.hpp"
#include "video/screen_capturer.hpp"

namespace {

using namespace std::chrono_literals;
using dv::client::app::CallSession;
using dv::client::app::Participant;
using dv::server::SignalingServer;

namespace media = dv::client::media;

/// Media takes longer than signaling: ICE has to gather, DTLS has to
/// handshake, and the first packets only follow after that.
constexpr auto kMediaTimeout = 20000ms;

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
                              std::chrono::milliseconds timeout = kMediaTimeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(50ms);
  }
  return predicate();
}

class Client {
 public:
  Client(std::uint16_t port, std::string username, bool echo_cancellation = true)
      : username_(std::move(username)),
        session_(std::make_unique<CallSession>(make_options(port, echo_cancellation))) {
    session_->on_events({
        .on_state = [this](CallSession::State state, std::string) { state_ = state; },
        .on_participants =
            [this](std::vector<Participant> list) {
              const std::lock_guard<std::mutex> lock(mutex_);
              participants_ = std::move(list);
            },
        .on_metrics = [this](media::AudioStats stats) { last_stats_ = stats; },
        .on_local_level =
            [this](double level, bool speaking) {
              highest_local_level_ = std::max(highest_local_level_.load(), level);
              if (speaking) {
                local_speaking_seen_ = true;
              }
            },
        .on_error = [this](dv::Error) { errors_.fetch_add(1); },
        .on_remote_video =
            [this](dv::client::video::VideoFrame frame) {
              const std::lock_guard<std::mutex> lock(mutex_);
              ++remote_frames_;
              remote_frame_size_ = dv::client::video::Size{frame.width(), frame.height()};
            },
        .on_screen_share =
            [this](std::string user_id) {
              const std::lock_guard<std::mutex> lock(mutex_);
              sharer_ = std::move(user_id);
            },
    });
    session_->on_room_created([this](std::string room_id) {
      const std::lock_guard<std::mutex> lock(mutex_);
      created_room_ = std::move(room_id);
    });
  }

  ~Client() { session_.reset(); }

  Client(const Client&) = delete;
  Client& operator=(const Client&) = delete;
  Client(Client&&) = delete;
  Client& operator=(Client&&) = delete;

  [[nodiscard]] bool login() {
    if (!session_->connect_and_authenticate(username_, "password").ok()) {
      return false;
    }
    return wait_until([this] { return !session_->local_user().id.empty(); }, 5000ms);
  }

  [[nodiscard]] std::string create_room() {
    if (!session_->create_room("").ok()) {
      return {};
    }
    if (!wait_until([this] { return !room_created().empty(); }, 5000ms)) {
      return {};
    }
    return room_created();
  }

  [[nodiscard]] bool join(const std::string& room_id) {
    return session_->join(room_id, username_).ok();
  }

  [[nodiscard]] bool wait_until_in_call() {
    return wait_until([this] { return state_.load() == CallSession::State::InCall; });
  }

  /// True once captured audio has actually left this machine.
  ///
  /// A different question from whether the call connected, and from whether a
  /// device is listed. Windows enumerates a wireless headset that is switched
  /// off, a permission prompt can silence an input that still appears in the
  /// list, and DV_AUDIO_NULL_DEVICE captures nothing on purpose. All three
  /// give a session that negotiates, connects, and sends not one byte.
  ///
  /// A test that asserts something about captured audio has to tell that apart
  /// from the thing it is testing. Otherwise it reports a broken echo canceller
  /// every time somebody's headset is off, which is a bug report pointing at
  /// the wrong half of the program.
  [[nodiscard]] bool wait_until_sending_audio() {
    return wait_until([this] { return session_->stats().bytes_sent > 0; });
  }

  [[nodiscard]] CallSession& session() { return *session_; }
  [[nodiscard]] media::AudioStats last_stats() const { return last_stats_; }
  [[nodiscard]] double highest_local_level() const { return highest_local_level_.load(); }
  [[nodiscard]] bool local_speaking_seen() const { return local_speaking_seen_.load(); }
  [[nodiscard]] std::uint64_t errors() const { return errors_.load(); }

  [[nodiscard]] std::uint64_t remote_frames() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return remote_frames_;
  }
  [[nodiscard]] dv::client::video::Size remote_frame_size() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return remote_frame_size_;
  }
  [[nodiscard]] std::string sharer() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sharer_;
  }

  [[nodiscard]] std::vector<Participant> participants() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return participants_;
  }
  [[nodiscard]] std::string room_created() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return created_room_;
  }

 private:
  static CallSession::Options make_options(std::uint16_t port, bool echo_cancellation) {
    CallSession::Options options;
    options.signaling_url = "ws://127.0.0.1:" + std::to_string(port);
    options.metrics_interval = 500ms;
    // No ICE servers: this is all loopback, and a test must not depend on
    // reaching a STUN server on the internet.
    options.media.ice_servers.clear();
    options.media.echo_cancellation = echo_cancellation;
    // scripts/virtual_audio.sh exports these on a machine with no sound card,
    // and then every case here runs on the virtual devices it created. Unset,
    // which is the case on a developer machine, the platform default is used.
    if (const char* input = std::getenv("DV_VIRTUAL_INPUT_DEVICE"); input != nullptr) {
      options.media.input_device = input;
    }
    if (const char* output = std::getenv("DV_VIRTUAL_OUTPUT_DEVICE"); output != nullptr) {
      options.media.output_device = output;
    }
    return options;
  }

  std::string username_;
  std::mutex mutex_;
  std::vector<Participant> participants_;
  std::string created_room_;
  std::atomic<CallSession::State> state_{CallSession::State::Idle};
  std::atomic<media::AudioStats> last_stats_{};
  std::atomic<std::uint64_t> errors_{0};
  std::atomic<double> highest_local_level_{0};
  std::atomic<bool> local_speaking_seen_{false};
  std::uint64_t remote_frames_ = 0;
  dv::client::video::Size remote_frame_size_;
  std::string sharer_;
  std::unique_ptr<CallSession> session_;
};

class MediaEndToEndTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!media::media_is_available()) {
      GTEST_SKIP() << "libwebrtc could not start on this machine";
    }

    SignalingServer::Options options;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.hub.max_participants_per_room = 5;
    options.hub.heartbeat_interval = 2000ms;
    options.hub.heartbeat_timeout = 60000ms;
    options.enable_sfu = true;
    options.sfu.ice_servers.clear();

    server_ = std::make_unique<SignalingServer>(options);
    // Five accounts, because section 22 of SPEC.md sizes a room at five and the
    // benchmarks fill one.
    ASSERT_TRUE(server_->add_user("ana", "password", "Ana").ok());
    ASSERT_TRUE(server_->add_user("bruno", "password", "Bruno").ok());
    ASSERT_TRUE(server_->add_user("carla", "password", "Carla").ok());
    ASSERT_TRUE(server_->add_user("diego", "password", "Diego").ok());
    ASSERT_TRUE(server_->add_user("elena", "password", "Elena").ok());
    server_->start();
    ASSERT_NE(server_->port(), 0);
  }

  void TearDown() override {
    clients_.clear();
    if (server_) {
      server_->stop();
      server_.reset();
    }
  }

  Client& add(const std::string& username, bool echo_cancellation = true) {
    clients_.push_back(std::make_unique<Client>(server_->port(), username, echo_cancellation));
    return *clients_.back();
  }

  std::unique_ptr<SignalingServer> server_;
  std::vector<std::unique_ptr<Client>> clients_;
};
}  // namespace
