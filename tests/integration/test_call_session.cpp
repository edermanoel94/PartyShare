// End to end tests for the application core of the client.
//
// The signaling and the server are real. The media is not: a stand-in
// AudioSession records what it was asked to do and answers as libwebrtc would.
// That is the point of the interface, and it is what lets the whole order of
// operations of a call be tested without a sound card or a second machine.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "app/call_session.hpp"
#include "signaling/server.hpp"

namespace {

using namespace std::chrono_literals;
using dv::client::app::CallSession;
using dv::client::app::Participant;
using dv::server::SignalingServer;

namespace audio = dv::client::audio;
namespace proto = dv::protocol;

constexpr auto kTimeout = 5000ms;

template <typename Predicate>
[[nodiscard]] bool wait_until(Predicate predicate, std::chrono::milliseconds timeout = kTimeout) {
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (predicate()) {
      return true;
    }
    std::this_thread::sleep_for(10ms);
  }
  return predicate();
}

/// What the fake session recorded, kept apart from the session itself.
///
/// leave() destroys the media session, so anything the test wants to assert
/// afterwards has to outlive it. That is not a detail of the fake: it is how
/// the real session behaves too.
struct FakeAudioState {
  std::mutex mutex;
  std::vector<std::string> offers;
  std::vector<audio::IceCandidate> candidates;
  std::atomic<bool> muted{false};
  std::atomic<bool> closed{false};

  [[nodiscard]] std::size_t offer_count() {
    const std::lock_guard<std::mutex> lock(mutex);
    return offers.size();
  }
  [[nodiscard]] std::string last_offer() {
    const std::lock_guard<std::mutex> lock(mutex);
    return offers.empty() ? std::string{} : offers.back();
  }
  [[nodiscard]] std::size_t candidate_count() {
    const std::lock_guard<std::mutex> lock(mutex);
    return candidates.size();
  }
};

/// Stands in for the libwebrtc session. It answers immediately and reports the
/// connection as established, which is the sequence the real one follows once
/// ICE is done.
class FakeAudioSession : public audio::AudioSession {
 public:
  FakeAudioSession(Callbacks callbacks, std::shared_ptr<FakeAudioState> state)
      : callbacks_(std::move(callbacks)), state_(std::move(state)) {}

  dv::Result<std::monostate> apply_remote_offer(const std::string& sdp) override {
    {
      const std::lock_guard<std::mutex> lock(state_->mutex);
      state_->offers.push_back(sdp);
    }
    if (callbacks_.on_local_answer) {
      callbacks_.on_local_answer("v=0\r\no=- 1 1 IN IP4 127.0.0.1\r\nanswer to " + sdp);
    }
    if (callbacks_.on_local_candidate) {
      callbacks_.on_local_candidate(
          audio::IceCandidate{"candidate:1 1 UDP 100 127.0.0.1 40000 typ host", "0", 0});
    }
    if (callbacks_.on_state) {
      callbacks_.on_state(audio::MediaState::Connected);
    }
    return std::monostate{};
  }

  dv::Result<std::monostate> add_remote_candidate(const audio::IceCandidate& candidate) override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->candidates.push_back(candidate);
    return std::monostate{};
  }

  void set_microphone_muted(bool muted) override { state_->muted = muted; }
  [[nodiscard]] bool microphone_muted() const override { return state_->muted.load(); }

  [[nodiscard]] audio::AudioStats stats() const override {
    audio::AudioStats stats;
    stats.round_trip_time_ms = 12;
    stats.jitter_ms = 3;
    stats.packets_received = 100;
    return stats;
  }

  [[nodiscard]] audio::MediaState state() const override {
    return state_->closed ? audio::MediaState::Closed : audio::MediaState::Connected;
  }

  void close() override { state_->closed = true; }

  /// Reports a remote participant's audio, the way a track arriving would.
  void report_remote_audio(const std::string& user_id, bool active) {
    if (callbacks_.on_remote_audio) {
      callbacks_.on_remote_audio(user_id, active);
    }
  }

 private:
  Callbacks callbacks_;
  std::shared_ptr<FakeAudioState> state_;
};

/// Owns a CallSession plus everything its callbacks reach into, so that the
/// session is always destroyed before them.
class Client {
 public:
  Client(std::uint16_t port, std::string username)
      : username_(std::move(username)),
        audio_state_(std::make_shared<FakeAudioState>()),
        session_(std::make_unique<CallSession>(
            make_options(port),
            [this](const audio::AudioSessionOptions&, audio::AudioSession::Callbacks callbacks)
                -> dv::Result<std::unique_ptr<audio::AudioSession>> {
              {
                const std::lock_guard<std::mutex> lock(mutex_);
                remote_audio_ = callbacks.on_remote_audio;
              }
              return std::unique_ptr<audio::AudioSession>(
                  std::make_unique<FakeAudioSession>(std::move(callbacks), audio_state_));
            })) {
    session_->on_events({
        .on_state = [this](CallSession::State state, std::string) { last_state_ = state; },
        .on_participants =
            [this](std::vector<Participant> list) {
              const std::lock_guard<std::mutex> lock(mutex_);
              participants_ = std::move(list);
            },
        .on_metrics = [this](audio::AudioStats) { metrics_reports_.fetch_add(1); },
        .on_error =
            [this](dv::Error error) {
              const std::lock_guard<std::mutex> lock(mutex_);
              errors_.push_back(std::move(error));
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
    if (!session_->connect_and_authenticate(username_, "senha").ok()) {
      return false;
    }
    return wait_until([this] { return !session_->local_user().id.empty(); });
  }

  [[nodiscard]] std::string create_room() {
    if (!session_->create_room("sala-dev").ok()) {
      return {};
    }
    if (!wait_until([this] { return !room_created().empty(); })) {
      return {};
    }
    return room_created();
  }

  [[nodiscard]] bool join(const std::string& room_id) {
    if (!session_->join(room_id, username_).ok()) {
      return false;
    }
    return wait_until([this] {
      for (const Participant& participant : participants()) {
        if (participant.user.id == session_->local_user().id) {
          return true;
        }
      }
      return false;
    });
  }

  [[nodiscard]] CallSession& session() { return *session_; }
  /// Outlives the media session, so it can still be read after leave().
  [[nodiscard]] FakeAudioState& audio() { return *audio_state_; }

  /// Reports a remote participant's audio, the way an arriving track would.
  void report_remote_audio(const std::string& user_id, bool active) {
    std::function<void(std::string, bool)> handler;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handler = remote_audio_;
    }
    if (handler) {
      handler(user_id, active);
    }
  }

  [[nodiscard]] std::vector<Participant> participants() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return participants_;
  }
  [[nodiscard]] std::string room_created() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return created_room_;
  }
  [[nodiscard]] std::vector<dv::Error> errors() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return errors_;
  }
  [[nodiscard]] CallSession::State last_state() const { return last_state_.load(); }
  [[nodiscard]] std::uint64_t metrics_reports() const { return metrics_reports_.load(); }

 private:
  static CallSession::Options make_options(std::uint16_t port) {
    CallSession::Options options;
    options.signaling_url = "ws://127.0.0.1:" + std::to_string(port);
    options.metrics_interval = 200ms;
    return options;
  }

  std::string username_;
  std::mutex mutex_;
  std::vector<Participant> participants_;
  std::vector<dv::Error> errors_;
  std::string created_room_;
  std::atomic<CallSession::State> last_state_{CallSession::State::Idle};
  std::atomic<std::uint64_t> metrics_reports_{0};
  std::function<void(std::string, bool)> remote_audio_;
  /// Declared before the session so that it outlives it.
  std::shared_ptr<FakeAudioState> audio_state_;
  std::unique_ptr<CallSession> session_;
};

class CallSessionTest : public ::testing::Test {
 protected:
  void SetUp() override {
    SignalingServer::Options options;
    options.bind_address = "127.0.0.1";
    options.port = 0;
    options.hub.max_participants_per_room = 5;
    options.hub.heartbeat_interval = 1000ms;
    options.hub.heartbeat_timeout = 30000ms;
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
    server_->stop();
    server_.reset();
  }

  Client& add(const std::string& username) {
    clients_.push_back(std::make_unique<Client>(server_->port(), username));
    return *clients_.back();
  }

  std::unique_ptr<SignalingServer> server_;
  std::vector<std::unique_ptr<Client>> clients_;
};

TEST_F(CallSessionTest, RefusesEmptyCredentials) {
  Client& ana = add("ana");
  const auto attempted = ana.session().connect_and_authenticate("", "senha");
  ASSERT_FALSE(attempted.ok());
  EXPECT_EQ(attempted.error().code, "invalid_value");
}

TEST_F(CallSessionTest, RefusesToJoinBeforeAuthenticating) {
  Client& ana = add("ana");
  const auto attempted = ana.session().join("8F42A1", "Ana");
  ASSERT_FALSE(attempted.ok());
  EXPECT_EQ(attempted.error().code, "unauthorized");
}

TEST_F(CallSessionTest, AuthenticatesAndReachesTheAuthenticatedState) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  EXPECT_EQ(ana.session().local_user().display_name, "Ana");
  EXPECT_TRUE(wait_until([&] { return ana.last_state() == CallSession::State::Authenticated; }));
}

TEST_F(CallSessionTest, JoiningNegotiatesMediaWithTheSfu) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  // The server offers on its own once the participant is in the room, and the
  // session applies it without being told to.
  ASSERT_TRUE(wait_until([&] { return ana.audio().offer_count() > 0; }));
  EXPECT_NE(ana.audio().last_offer().find("m=audio"), std::string::npos);

  // Answering it takes the session into the call.
  EXPECT_TRUE(wait_until([&] { return ana.last_state() == CallSession::State::InCall; }));

  // And the SFU keeps trickling candidates at it.
  EXPECT_TRUE(wait_until([&] { return ana.audio().candidate_count() > 0; }));
}

TEST_F(CallSessionTest, ParticipantsAppearAndDisappear) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));

  EXPECT_TRUE(wait_until([&] { return ana.participants().size() == 2; }));
  EXPECT_TRUE(wait_until([&] { return bruno.participants().size() == 2; }));

  ASSERT_TRUE(bruno.session().leave().ok());
  EXPECT_TRUE(wait_until([&] { return ana.participants().size() == 1; }));
  EXPECT_EQ(ana.participants().front().user.display_name, "ana");
}

TEST_F(CallSessionTest, MuteIsConfirmedByTheServerBeforeItShows) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(wait_until([&] { return bruno.participants().size() == 2; }));

  ASSERT_TRUE(ana.session().set_muted(true).ok());

  // The microphone stops locally at once, and the other participant learns
  // about it through the server.
  EXPECT_TRUE(ana.audio().muted.load());
  EXPECT_TRUE(wait_until([&] {
    for (const Participant& participant : bruno.participants()) {
      if (participant.user.id == ana.session().local_user().id) {
        return participant.muted;
      }
    }
    return false;
  }));

  ASSERT_TRUE(ana.session().set_muted(false).ok());
  EXPECT_FALSE(ana.audio().muted.load());
  EXPECT_TRUE(wait_until([&] {
    for (const Participant& participant : bruno.participants()) {
      if (participant.user.id == ana.session().local_user().id) {
        return !participant.muted;
      }
    }
    return false;
  }));
}

TEST_F(CallSessionTest, MutingBeforeJoiningSurvivesTheJoin) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.session().set_muted(true).ok());

  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  EXPECT_TRUE(wait_until([&] { return ana.audio().muted.load(); }));
}

TEST_F(CallSessionTest, RemoteAudioMarksTheParticipant) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(wait_until([&] { return ana.participants().size() == 2; }));

  ana.report_remote_audio(bruno.session().local_user().id, true);

  EXPECT_TRUE(wait_until([&] {
    for (const Participant& participant : ana.participants()) {
      if (participant.user.id == bruno.session().local_user().id) {
        return participant.audio_active;
      }
    }
    return false;
  }));
}

TEST_F(CallSessionTest, MetricsAreReportedWhileInACall) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  // The interval is 200 ms in these tests, five seconds in the product.
  EXPECT_TRUE(wait_until([&] { return ana.metrics_reports() >= 2; }));
}

TEST_F(CallSessionTest, LeavingClosesTheMediaSession) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(wait_until([&] { return ana.audio().offer_count() > 0; }));

  ASSERT_TRUE(ana.session().leave().ok());

  // The state object outlives the session leave() destroyed.
  EXPECT_TRUE(ana.audio().closed.load());
  EXPECT_TRUE(ana.participants().empty());
  EXPECT_TRUE(ana.session().room_id().empty());
}

TEST_F(CallSessionTest, AServerErrorReachesTheApplication) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());

  // No such room, so the server answers with an error instead of a join.
  ASSERT_TRUE(ana.session().join("ZZZZZZ", "Ana").ok());

  EXPECT_TRUE(wait_until([&] { return !ana.errors().empty(); }));
  EXPECT_EQ(ana.errors().front().code, "room_not_found");
}

}  // namespace
