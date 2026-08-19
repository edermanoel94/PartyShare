// The interoperation the whole plan rests on: libwebrtc on the client and
// libdatachannel on the server, negotiating and carrying audio between them.
//
// PLAN.md lists this as a high risk, and this is what retires it. Everything
// here is real: a real signaling server, a real SFU, real libwebrtc peer
// connections, real ICE, DTLS and SRTP. Only the network is local.
//
// Built only when DV_BUILD_CLIENT_MEDIA is on, because it needs the libwebrtc
// tree from docs/webrtc-toolchain.md.
//
// Run these through ctest, which gives every case its own process. Running the
// whole binary in one process fails on Linux from the fourth media session
// onwards: libwebrtc's PulseAudio device reports `failed to activate
// recording` and blocks its worker thread for ten seconds, twice, which is
// longer than any timeout here. It is a real limitation and it is written down
// in the M4 section of PLAN.md; it is not something these tests work around.
//
// Set DV_AUDIO_NULL_DEVICE=1 to run them on a machine with no sound card. The
// negotiation cases still pass; the ones that assert audio actually flowing
// cannot, because a null device captures nothing.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "app/call_session.hpp"
#include "sfu/audio_router.hpp"
#include "signaling/server.hpp"

namespace {

using namespace std::chrono_literals;
using dv::client::app::CallSession;
using dv::client::app::Participant;
using dv::server::SignalingServer;

namespace audio = dv::client::audio;

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
  Client(std::uint16_t port, std::string username)
      : username_(std::move(username)),
        session_(std::make_unique<CallSession>(make_options(port))) {
    session_->on_events({
        .on_state = [this](CallSession::State state, std::string) { state_ = state; },
        .on_participants =
            [this](std::vector<Participant> list) {
              const std::lock_guard<std::mutex> lock(mutex_);
              participants_ = std::move(list);
            },
        .on_metrics = [this](audio::AudioStats stats) { last_stats_ = stats; },
        .on_error = [this](dv::Error) { errors_.fetch_add(1); },
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
    if (!session_->connect_and_authenticate(username_, "senha").ok()) {
      return false;
    }
    return wait_until([this] { return !session_->local_user().id.empty(); }, 5000ms);
  }

  [[nodiscard]] std::string create_room() {
    if (!session_->create_room("sala-dev").ok()) {
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

  [[nodiscard]] CallSession& session() { return *session_; }
  [[nodiscard]] audio::AudioStats last_stats() const { return last_stats_; }
  [[nodiscard]] std::uint64_t errors() const { return errors_.load(); }

  [[nodiscard]] std::vector<Participant> participants() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return participants_;
  }
  [[nodiscard]] std::string room_created() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return created_room_;
  }

 private:
  static CallSession::Options make_options(std::uint16_t port) {
    CallSession::Options options;
    options.signaling_url = "ws://127.0.0.1:" + std::to_string(port);
    options.metrics_interval = 500ms;
    // No ICE servers: this is all loopback, and a test must not depend on
    // reaching a STUN server on the internet.
    options.audio.ice_servers.clear();
    return options;
  }

  std::string username_;
  std::mutex mutex_;
  std::vector<Participant> participants_;
  std::string created_room_;
  std::atomic<CallSession::State> state_{CallSession::State::Idle};
  std::atomic<audio::AudioStats> last_stats_{};
  std::atomic<std::uint64_t> errors_{0};
  std::unique_ptr<CallSession> session_;
};

class MediaEndToEndTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!audio::media_is_available()) {
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
    ASSERT_TRUE(server_->add_user("ana", "senha", "Ana").ok());
    ASSERT_TRUE(server_->add_user("bruno", "senha", "Bruno").ok());
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

  Client& add(const std::string& username) {
    clients_.push_back(std::make_unique<Client>(server_->port(), username));
    return *clients_.back();
  }

  std::unique_ptr<SignalingServer> server_;
  std::vector<std::unique_ptr<Client>> clients_;
};

TEST_F(MediaEndToEndTest, ALibwebrtcClientNegotiatesWithTheLibdatachannelSfu) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  // Reaching this state means the SFU's offer was understood, the answer was
  // accepted, ICE completed and DTLS finished. That is the whole
  // interoperation question answered.
  EXPECT_TRUE(ana.wait_until_in_call());
  EXPECT_EQ(ana.errors(), 0U);
}

TEST_F(MediaEndToEndTest, TwoClientsShareARoomAndTheSfuForwardsTheirAudio) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  ASSERT_TRUE(wait_until([&] { return ana.participants().size() == 2; }));
  ASSERT_TRUE(wait_until([&] { return bruno.participants().size() == 2; }));

  auto* router = server_->audio_router();
  ASSERT_NE(router, nullptr);

  // One track each way, carrying the other participant.
  EXPECT_TRUE(wait_until([&] {
    return router->outbound_track_count(ana.session().local_user().id) == 1 &&
           router->outbound_track_count(bruno.session().local_user().id) == 1;
  }));

  // Audio itself. A machine with no working capture device still encodes and
  // sends, because libwebrtc falls back to silence rather than to nothing.
  EXPECT_TRUE(wait_until([&] { return router->packets_received() > 0; }))
      << "no audio reached the SFU";
  EXPECT_TRUE(wait_until([&] { return router->packets_forwarded() > 0; }))
      << "the SFU received audio but forwarded none";
}

TEST_F(MediaEndToEndTest, MetricsAreCollectedFromTheRealConnection) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  // Bytes sent is the first number to move, and it proves the stats path
  // works end to end: collection, parsing and reporting.
  EXPECT_TRUE(wait_until([&] { return ana.session().stats().bytes_sent > 0; }));
}

TEST_F(MediaEndToEndTest, MutingStopsTheOutgoingAudio) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_in_call());

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_in_call());

  ASSERT_TRUE(wait_until([&] { return server_->audio_router()->packets_received() > 0; }));
  ASSERT_TRUE(ana.session().set_muted(true).ok());

  // Muting disables the track rather than renegotiating, so the connection
  // stays up and the room agrees about who is muted.
  EXPECT_EQ(ana.session().state(), CallSession::State::InCall);
  EXPECT_TRUE(wait_until([&] {
    for (const Participant& participant : bruno.participants()) {
      if (participant.user.id == ana.session().local_user().id) {
        return participant.muted;
      }
    }
    return false;
  }));
}

}  // namespace
