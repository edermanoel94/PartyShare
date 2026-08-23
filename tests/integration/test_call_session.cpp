// End to end tests for the application core of the client.
//
// The signaling and the server are real. The media is not: a stand-in
// MediaSession records what it was asked to do and answers as libwebrtc would.
// That is the point of the interface, and it is what lets the whole order of
// operations of a call be tested without a sound card or a second machine.

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <gtest/gtest.h>

#include <dv/models/chat.hpp>

#include "app/call_session.hpp"
#include "signaling/server.hpp"

namespace {

using namespace std::chrono_literals;
using dv::client::app::CallSession;
using dv::client::app::Participant;
using dv::server::SignalingServer;

namespace media = dv::client::media;
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
struct FakeMediaState {
  std::mutex mutex;
  std::vector<std::string> offers;
  std::vector<media::IceCandidate> candidates;
  std::unordered_map<std::string, double> volumes;
  std::string input_device;
  std::string output_device;
  std::atomic<bool> muted{false};
  std::atomic<bool> closed{false};
  std::atomic<bool> sharing{false};
  std::string shared_monitor;
  /// Set to make start_screen_share fail, the way a refused permission does.
  std::string share_failure;
  std::atomic<int> share_starts{0};
  int video_min_kbps = 0;
  int video_max_kbps = 0;
  std::atomic<int> share_stops{0};
  /// What the last accepted set_capture_options asked for.
  dv::client::video::ScreenCaptureOptions capture;
  std::atomic<int> capture_changes{0};
  /// Set to make set_capture_options fail, the way a monitor that went away
  /// mid-call makes the restart fail.
  std::string capture_failure;

  [[nodiscard]] double volume_of(const std::string& user_id) {
    const std::lock_guard<std::mutex> lock(mutex);
    const auto it = volumes.find(user_id);
    return it == volumes.end() ? -1.0 : it->second;
  }
  [[nodiscard]] std::string input() {
    const std::lock_guard<std::mutex> lock(mutex);
    return input_device;
  }
  [[nodiscard]] std::string monitor() {
    const std::lock_guard<std::mutex> lock(mutex);
    return shared_monitor;
  }
  [[nodiscard]] dv::client::video::ScreenCaptureOptions capture_options() {
    const std::lock_guard<std::mutex> lock(mutex);
    return capture;
  }

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
class FakeMediaSession : public media::MediaSession {
 public:
  FakeMediaSession(Callbacks callbacks, std::shared_ptr<FakeMediaState> state)
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
          media::IceCandidate{"candidate:1 1 UDP 100 127.0.0.1 40000 typ host", "0", 0});
    }
    if (callbacks_.on_state) {
      callbacks_.on_state(media::MediaState::Connected);
    }
    return std::monostate{};
  }

  dv::Result<std::monostate> add_remote_candidate(const media::IceCandidate& candidate) override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->candidates.push_back(candidate);
    return std::monostate{};
  }

  void set_microphone_muted(bool muted) override { state_->muted = muted; }
  [[nodiscard]] bool microphone_muted() const override { return state_->muted.load(); }

  dv::Result<std::monostate> set_participant_volume(const std::string& user_id,
                                                    double volume) override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->volumes[user_id] = volume;
    return std::monostate{};
  }

  dv::Result<std::monostate> set_input_device(const std::string& device_id) override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->input_device = device_id;
    return std::monostate{};
  }

  dv::Result<std::monostate> set_output_device(const std::string& device_id) override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->output_device = device_id;
    return std::monostate{};
  }

  /// Reports levels the way the real session does, from its own thread.
  void report_levels(std::vector<media::AudioLevel> levels) {
    if (callbacks_.on_levels) {
      callbacks_.on_levels(std::move(levels));
    }
  }

  dv::Result<std::monostate> start_screen_share(const std::string& monitor_id) override {
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->share_failure.empty()) {
      return dv::Result<std::monostate>::failure("capture_unavailable", state_->share_failure);
    }
    state_->shared_monitor = monitor_id;
    state_->sharing = true;
    state_->share_starts.fetch_add(1);
    return std::monostate{};
  }

  void stop_screen_share() override {
    state_->sharing = false;
    state_->share_stops.fetch_add(1);
  }

  [[nodiscard]] bool sharing_screen() const override { return state_->sharing.load(); }

  dv::Result<std::monostate> set_video_bitrate(int min_kbps, int max_kbps) override {
    if (min_kbps <= 0 || max_kbps < min_kbps) {
      return dv::Result<std::monostate>::failure("invalid_value", "bad bitrate range");
    }
    const std::lock_guard<std::mutex> lock(state_->mutex);
    state_->video_min_kbps = min_kbps;
    state_->video_max_kbps = max_kbps;
    return std::monostate{};
  }

  dv::Result<std::monostate> set_capture_options(
      const dv::client::video::ScreenCaptureOptions& options) override {
    if (options.max_size.empty() || options.max_fps < 1) {
      return dv::Result<std::monostate>::failure("invalid_value", "bad capture options");
    }
    const std::lock_guard<std::mutex> lock(state_->mutex);
    if (!state_->capture_failure.empty()) {
      return dv::Result<std::monostate>::failure("capture_unavailable", state_->capture_failure);
    }
    state_->capture = options;
    state_->capture_changes.fetch_add(1);
    return std::monostate{};
  }

  [[nodiscard]] media::VideoStats video_stats() const override {
    media::VideoStats stats;
    stats.send_width = 1280;
    stats.send_height = 720;
    stats.frames_sent = state_->sharing ? 30 : 0;
    return stats;
  }

  /// Reports a decoded frame of somebody else's screen.
  void report_remote_video(int width, int height) {
    if (callbacks_.on_remote_video) {
      const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                         static_cast<std::size_t>(dv::client::video::VideoFrame::kBytesPerPixel);
      callbacks_.on_remote_video(dv::client::video::VideoFrame{
          dv::client::video::Size{width, height}, std::vector<std::uint8_t>(bytes)});
    }
  }

  /// Reports that the capture ended on its own.
  void report_share_ended(dv::Error reason) {
    state_->sharing = false;
    if (callbacks_.on_screen_share_ended) {
      callbacks_.on_screen_share_ended(std::move(reason));
    }
  }

  [[nodiscard]] media::AudioStats stats() const override {
    media::AudioStats stats;
    stats.round_trip_time_ms = 12;
    stats.jitter_ms = 3;
    stats.packets_received = 100;
    return stats;
  }

  [[nodiscard]] media::MediaState state() const override {
    return state_->closed ? media::MediaState::Closed : media::MediaState::Connected;
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
  std::shared_ptr<FakeMediaState> state_;
};

/// Owns a CallSession plus everything its callbacks reach into, so that the
/// session is always destroyed before them.
class Client {
 public:
  Client(std::uint16_t port, std::string username)
      : username_(std::move(username)),
        media_state_(std::make_shared<FakeMediaState>()),
        session_(std::make_unique<CallSession>(
            make_options(port),
            [this](const media::MediaSessionOptions& options,
                   media::MediaSession::Callbacks callbacks)
                -> dv::Result<std::unique_ptr<media::MediaSession>> {
              {
                const std::lock_guard<std::mutex> lock(mutex_);
                remote_audio_ = callbacks.on_remote_audio;
              }
              {
                // The real session reads the capture size and rate here, at
                // creation, rather than being told them afterwards. Recording
                // them is what lets a choice made before a call be checked
                // where it actually lands.
                const std::lock_guard<std::mutex> lock(media_state_->mutex);
                media_state_->capture = options.capture;
              }
              auto fake = std::make_unique<FakeMediaSession>(std::move(callbacks), media_state_);
              {
                const std::lock_guard<std::mutex> lock(mutex_);
                levels_ = [raw = fake.get()](std::vector<media::AudioLevel> levels) {
                  raw->report_levels(std::move(levels));
                };
              }
              return std::unique_ptr<media::MediaSession>(std::move(fake));
            })) {
    session_->on_events({
        .on_state = [this](CallSession::State state, std::string) { last_state_ = state; },
        .on_participants =
            [this](std::vector<Participant> list) {
              const std::lock_guard<std::mutex> lock(mutex_);
              participants_ = std::move(list);
            },
        .on_metrics = [this](media::AudioStats) { metrics_reports_.fetch_add(1); },
        .on_local_level =
            [this](double level, bool speaking) {
              local_level_ = level;
              local_speaking_ = speaking;
            },
        .on_error =
            [this](dv::Error error) {
              const std::lock_guard<std::mutex> lock(mutex_);
              errors_.push_back(std::move(error));
            },
        .on_remote_video =
            [this](dv::client::video::VideoFrame frame) {
              const std::lock_guard<std::mutex> lock(mutex_);
              ++remote_frames_;
              last_frame_size_ = {frame.width(), frame.height()};
            },
        .on_screen_share =
            [this](std::string user_id) {
              const std::lock_guard<std::mutex> lock(mutex_);
              sharer_ = std::move(user_id);
            },
        .on_chat_message =
            [this](dv::models::ChatMessage message) {
              const std::lock_guard<std::mutex> lock(mutex_);
              chat_.push_back(std::move(message));
            },
        .on_chat_history =
            [this](std::vector<dv::models::ChatMessage> messages) {
              // Replaces, exactly as the interface does. See
              // CallSession::Callbacks::on_chat_history.
              const std::lock_guard<std::mutex> lock(mutex_);
              chat_ = std::move(messages);
              ++histories_;
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
    return wait_until([this] { return !session_->local_user().id.empty(); });
  }

  [[nodiscard]] std::string create_room() {
    if (!session_->create_room("dev-room").ok()) {
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
  [[nodiscard]] FakeMediaState& audio() { return *media_state_; }

  /// Reports audio levels, the way the media layer does during a call.
  ///
  /// Only valid while the media session exists: leave() destroys it.
  void report_levels(std::vector<media::AudioLevel> levels) {
    std::function<void(std::vector<media::AudioLevel>)> handler;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handler = levels_;
    }
    if (handler) {
      handler(std::move(levels));
    }
  }

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

  [[nodiscard]] std::uint64_t remote_frames() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return remote_frames_;
  }
  [[nodiscard]] dv::client::video::Size last_frame_size() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_frame_size_;
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
  [[nodiscard]] std::vector<dv::Error> errors() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return errors_;
  }
  [[nodiscard]] std::vector<dv::models::ChatMessage> chat() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return chat_;
  }
  [[nodiscard]] std::uint64_t histories() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return histories_;
  }
  [[nodiscard]] CallSession::State last_state() const { return last_state_.load(); }
  [[nodiscard]] double local_level() const { return local_level_.load(); }
  [[nodiscard]] bool local_speaking() const { return local_speaking_.load(); }
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
  std::vector<dv::models::ChatMessage> chat_;
  std::uint64_t histories_ = 0;
  std::string created_room_;
  std::atomic<CallSession::State> last_state_{CallSession::State::Idle};
  std::atomic<std::uint64_t> metrics_reports_{0};
  std::atomic<double> local_level_{0};
  std::atomic<bool> local_speaking_{false};
  std::uint64_t remote_frames_ = 0;
  dv::client::video::Size last_frame_size_;
  std::string sharer_;
  std::function<void(std::string, bool)> remote_audio_;
  std::function<void(std::vector<media::AudioLevel>)> levels_;
  /// Declared before the session so that it outlives it.
  std::shared_ptr<FakeMediaState> media_state_;
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
    ASSERT_TRUE(server_->add_user("ana", "password", "Ana").ok());
    ASSERT_TRUE(server_->add_user("bruno", "password", "Bruno").ok());
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
  const auto attempted = ana.session().connect_and_authenticate("", "password");
  ASSERT_FALSE(attempted.ok());
  EXPECT_EQ(attempted.error().code, "invalid_value");
}

TEST_F(CallSessionTest, RefusesToJoinBeforeAuthenticating) {
  Client& ana = add("ana");
  const auto attempted = ana.session().join("8F42A1", "Ana");
  ASSERT_FALSE(attempted.ok());
  EXPECT_EQ(attempted.error().code, "unauthorized");
}

// Mistyping a password is the most ordinary thing that happens on a login
// screen, and it used to end the session for good. The server answers the
// refusal with an error and leaves the socket standing, so the second attempt
// found a connection already open, reported `already_connected`, and put
// "disconnect() before connecting again" in front of the user. The button did
// nothing from then on and the only way back was to close the window.
TEST_F(CallSessionTest, AWrongPasswordCanBeFollowedByTheRightOne) {
  Client& ana = add("ana");

  ASSERT_TRUE(ana.session().connect_and_authenticate("ana", "wrong").ok());
  ASSERT_TRUE(wait_until([&] { return !ana.errors().empty(); }))
      << "the session never reported the refusal";
  EXPECT_EQ(ana.errors().front().code, "unauthorized");
  EXPECT_TRUE(ana.session().local_user().id.empty()) << "a wrong password authenticated";

  ASSERT_TRUE(ana.session().connect_and_authenticate("ana", "password").ok())
      << "the second attempt was refused before it reached the server";
  EXPECT_TRUE(wait_until([&] { return !ana.session().local_user().id.empty(); }))
      << "the right password after a wrong one did not authenticate";
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

TEST_F(CallSessionTest, AForcedMuteActuallyStopsTheMicrophone) {
  // The whole point of the feature, and the one thing that cannot be checked by
  // looking at the flag the interface reads: the capture itself has to stop.
  // Setting muted_ without telling the media session would leave the button
  // saying "unmute", every other client showing a silent participant, and that
  // participant's voice still going into the room.
  ASSERT_TRUE(server_->add_user("carla", "password", "Carla", dv::models::Role::Admin).ok());

  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  Client& carla = add("carla");
  ASSERT_TRUE(carla.login());
  ASSERT_TRUE(carla.join(room));
  ASSERT_TRUE(wait_until([&] { return carla.participants().size() == 2; }));

  ASSERT_FALSE(ana.audio().muted.load());
  ASSERT_TRUE(carla.session().force_mute(ana.session().local_user().id, true).ok());

  EXPECT_TRUE(wait_until([&] { return ana.audio().muted.load(); }));
  EXPECT_TRUE(wait_until([&] { return ana.session().muted(); }));

  // And releasing it gives the microphone back, rather than only saying so.
  ASSERT_TRUE(carla.session().force_mute(ana.session().local_user().id, false).ok());
  EXPECT_TRUE(wait_until([&] { return !ana.audio().muted.load(); }));
  EXPECT_TRUE(wait_until([&] { return !ana.session().muted(); }));
}

TEST_F(CallSessionTest, BeingKickedEndsTheCallForWhoeverWasRemoved) {
  ASSERT_TRUE(server_->add_user("carla", "password", "Carla", dv::models::Role::Admin).ok());

  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  Client& carla = add("carla");
  ASSERT_TRUE(carla.login());
  ASSERT_TRUE(carla.join(room));
  ASSERT_TRUE(wait_until([&] { return carla.participants().size() == 2; }));

  ASSERT_TRUE(carla.session().kick(ana.session().local_user().id, "off topic").ok());

  // Out of the room and back to being merely signed in, without the connection
  // dropping: the account is still good and another room is still available.
  EXPECT_TRUE(wait_until([&] { return ana.last_state() == CallSession::State::Authenticated; }));
  EXPECT_TRUE(wait_until([&] { return ana.session().room_id().empty(); }));
  EXPECT_TRUE(wait_until([&] { return carla.participants().size() == 1; }));
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

TEST_F(CallSessionTest, VolumeChosenBeforeTheCallIsAppliedWhenItStarts) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());

  // Nobody has spoken yet, and the interface is allowed to set this anyway.
  ASSERT_TRUE(ana.session().set_participant_volume("someone", 0.4).ok());

  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  EXPECT_TRUE(wait_until([&] { return ana.audio().volume_of("someone") == 0.4; }));
}

TEST_F(CallSessionTest, VolumeDuringACallReachesTheMediaLayer) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(wait_until([&] { return ana.participants().size() == 2; }));

  const std::string bruno_id = bruno.session().local_user().id;
  ASSERT_TRUE(ana.session().set_participant_volume(bruno_id, 1.5).ok());

  EXPECT_EQ(ana.audio().volume_of(bruno_id), 1.5);
}

TEST_F(CallSessionTest, TheChosenInputDeviceSurvivesIntoTheCall) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.session().set_input_device("Microfone USB").ok());

  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  EXPECT_TRUE(wait_until([&] { return ana.audio().input() == "Microfone USB"; }));
}

TEST_F(CallSessionTest, TheChosenScreenQualitySurvivesIntoTheCall) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.session().set_video_quality({1920, 1080}, 60).ok());

  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  EXPECT_TRUE(wait_until([&] {
    const auto capture = ana.audio().capture_options();
    return capture.max_size == dv::client::video::Size{1920, 1080} && capture.max_fps == 60;
  }));
}

TEST_F(CallSessionTest, ChangingTheScreenQualityDuringACallReachesTheMediaLayer) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  // Joining is not being in a call. The media session is created when the
  // SFU's offer arrives, and until it exists set_video_quality has nobody to
  // tell: it keeps the choice in the options, answers ok, and the media layer
  // is never touched. Waiting here is what makes this test about changing the
  // quality during a call rather than about the timing of the machine running
  // it - on Windows the session happened to be up, and under the sanitizers,
  // which are slower, it was not.
  ASSERT_TRUE(wait_until([&] { return ana.last_state() == CallSession::State::InCall; }));

  // The point of the whole change: nobody should have to leave and rejoin to
  // go from 720p to 1080p60.
  ASSERT_TRUE(ana.session().set_video_quality({1920, 1080}, 60).ok());

  EXPECT_TRUE(wait_until([&] { return ana.audio().capture_changes.load() == 1; }));
  const auto capture = ana.audio().capture_options();
  EXPECT_EQ(capture.max_size, (dv::client::video::Size{1920, 1080}));
  EXPECT_EQ(capture.max_fps, 60);
  EXPECT_EQ(ana.session().video_quality().max_fps, 60);
}

TEST_F(CallSessionTest, ARefusedScreenQualityLeavesThePreviousOneInPlace) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  // As above: with no media session yet there is nothing to refuse, and
  // set_video_quality would answer ok for the wrong reason.
  ASSERT_TRUE(wait_until([&] { return ana.last_state() == CallSession::State::InCall; }));

  const auto before = ana.session().video_quality();
  {
    const std::lock_guard<std::mutex> lock(ana.audio().mutex);
    ana.audio().capture_failure = "the monitor went away";
  }

  // Remembering a quality the media layer refused would leave the settings
  // dialog showing something that is not being sent.
  EXPECT_FALSE(ana.session().set_video_quality({1920, 1080}, 60).ok());
  EXPECT_EQ(ana.session().video_quality().max_size, before.max_size);
  EXPECT_EQ(ana.session().video_quality().max_fps, before.max_fps);
}

TEST_F(CallSessionTest, AnImpossibleScreenQualityNeverReachesTheMediaLayer) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  EXPECT_FALSE(ana.session().set_video_quality({0, 0}, 60).ok());
  EXPECT_FALSE(ana.session().set_video_quality({1920, 1080}, 0).ok());
  EXPECT_EQ(ana.audio().capture_changes.load(), 0);
}

TEST_F(CallSessionTest, LevelsMarkWhoIsSpeaking) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());
  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(wait_until([&] { return ana.participants().size() == 2; }));

  const std::string bruno_id = bruno.session().local_user().id;
  ana.report_levels({
      media::AudioLevel{{}, 0.7, true},
      media::AudioLevel{bruno_id, 0.3, true},
  });

  EXPECT_TRUE(wait_until([&] {
    for (const Participant& participant : ana.participants()) {
      if (participant.user.id == bruno_id) {
        return participant.speaking && participant.level == 0.3;
      }
    }
    return false;
  }));

  // The local microphone is reported apart from the participant list: it is
  // not something the server knows about.
  EXPECT_TRUE(wait_until([&] { return ana.local_speaking() && ana.local_level() == 0.7; }));
}

TEST_F(CallSessionTest, AMessageTypedInOneClientArrivesInTheOther) {
  Client& ana = add("ana");
  Client& bruno = add("bruno");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(bruno.login());

  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(bruno.join(room));

  ASSERT_TRUE(ana.session().send_chat("the build is green").ok());

  // Both ends, the sender included, and both showing what the server stored.
  EXPECT_TRUE(wait_until([&] { return !bruno.chat().empty(); }));
  EXPECT_TRUE(wait_until([&] { return !ana.chat().empty(); }));

  const auto received = bruno.chat().back();
  EXPECT_EQ(received.text, "the build is green");
  EXPECT_EQ(received.display_name, "ana");
  EXPECT_EQ(received.user_id, ana.session().local_user().id);
  EXPECT_FALSE(received.id.empty());
  EXPECT_GT(received.timestamp_seconds, 0);
  EXPECT_EQ(ana.chat().back().id, received.id);
}

TEST_F(CallSessionTest, EmojiArriveAtTheOtherEndIntact) {
  Client& ana = add("ana");
  Client& bruno = add("bruno");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(bruno.login());

  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(bruno.join(room));

  // Over a real socket, through the JSON on the wire and back out of it.
  const std::string text = "deploy feito 🚀🎉 tudo verde ✅";
  ASSERT_TRUE(ana.session().send_chat(text).ok());

  EXPECT_TRUE(wait_until([&] { return !bruno.chat().empty(); }));
  EXPECT_EQ(bruno.chat().back().text, text);
}

TEST_F(CallSessionTest, SomebodyJoiningLaterIsSentTheConversation) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());

  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.session().send_chat("morning").ok());
  ASSERT_TRUE(wait_until([&] { return !ana.chat().empty(); }));

  Client& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));

  // Without being asked for it: a client that had to request the history would
  // show an empty panel first either way.
  EXPECT_TRUE(wait_until([&] { return bruno.histories() > 0; }));
  ASSERT_EQ(bruno.chat().size(), 1U);
  EXPECT_EQ(bruno.chat().front().text, "morning");
}

TEST_F(CallSessionTest, AnEmptyMessageNeverLeavesTheClient) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());

  const std::string room = ana.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(ana.join(room));

  // Refused here rather than by the server, so that pressing return on an
  // empty line costs nothing at all.
  const auto sent = ana.session().send_chat("   \n ");
  ASSERT_FALSE(sent.ok());
  EXPECT_EQ(sent.error().code, "invalid_value");
  EXPECT_TRUE(ana.chat().empty());
}

TEST_F(CallSessionTest, SpeakingOutsideARoomIsRefusedBeforeItIsSent) {
  Client& ana = add("ana");
  ASSERT_TRUE(ana.login());

  const auto sent = ana.session().send_chat("anybody there");
  ASSERT_FALSE(sent.ok());
  EXPECT_EQ(sent.error().code, "not_in_room");
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
