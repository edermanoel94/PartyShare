// End to end tests for the audio SFU.
//
// Real WebSocket signaling, real ICE, real DTLS and real RTP, all over the
// loopback interface. The participants are libdatachannel peers driven by the
// project's own SignalingClient, so what is under test is the whole path a
// participant takes: authenticate, join, answer the server's offer, and then
// have their audio reach the other end.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <rtc/rtc.hpp>

#include "network/signaling_client.hpp"
#include "sfu/audio_router.hpp"
#include "signaling/server.hpp"

namespace {

using namespace std::chrono_literals;
using dv::client::SignalingClient;
using dv::server::SignalingServer;

namespace proto = dv::protocol;

constexpr auto kTimeout = 10000ms;

/// The SSRC a participant sends its own audio under. The SFU rewrites it per
/// destination, so it never reaches another participant as is.
constexpr std::uint32_t kOutgoingSsrc = 0xDEADBEEF;

/// Waits for `predicate` to hold, polling instead of sleeping a fixed time so
/// that a fast machine finishes fast and a loaded one does not go flaky.
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

/// One Opus packet, as a participant's encoder would hand it over. The payload
/// is not decodable audio, and does not need to be: the SFU forwards packets
/// without ever looking inside them.
[[nodiscard]] rtc::binary make_rtp_packet(std::uint32_t ssrc, std::uint16_t sequence,
                                          std::uint32_t timestamp, int payload_type) {
  constexpr std::size_t kPayloadSize = 80;
  rtc::binary packet(sizeof(rtc::RtpHeader) + kPayloadSize, std::byte{0});

  auto* header = reinterpret_cast<rtc::RtpHeader*>(packet.data());
  header->preparePacket();
  header->setSsrc(ssrc);
  header->setSeqNumber(sequence);
  header->setTimestamp(timestamp);
  header->setPayloadType(static_cast<std::uint8_t>(payload_type));
  return packet;
}

/// A participant: the project's signaling client plus a libdatachannel peer
/// that answers whatever the SFU offers.
class Participant {
 public:
  Participant(std::uint16_t port, std::string username)
      : username_(std::move(username)),
        signaling_(SignalingClient::Options{"ws://127.0.0.1:" + std::to_string(port)}) {
    // No ICE servers: everything here is on the loopback interface, so host
    // candidates are all it takes, and no test should depend on the internet.
    connection_ = std::make_shared<rtc::PeerConnection>(rtc::Configuration{});

    connection_->onLocalDescription([this](rtc::Description description) {
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        last_answer_sdp_ = std::string(description);
      }
      // The SFU offers and the participant answers, always.
      (void)signaling_.send(proto::Answer{room_id_, user_.id, std::string(proto::kSfuUserId),
                                          std::string(description)});
    });

    connection_->onLocalCandidate([this](rtc::Candidate candidate) {
      (void)signaling_.send(proto::IceCandidate{room_id_, user_.id, std::string(proto::kSfuUserId),
                                                std::string(candidate), candidate.mid(), 0});
    });

    connection_->onTrack([this](std::shared_ptr<rtc::Track> track) {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (track->direction() == rtc::Description::Direction::SendOnly) {
        // The m-line the server marked recvonly: this is the microphone.
        //
        // The SSRC has to be declared in the answer. Everything is bundled on
        // one transport, so it is the only thing that tells the server which
        // track an arriving packet belongs to, and without it the audio is
        // silently dropped on arrival. libwebrtc always declares one; this is
        // the same thing by hand.
        rtc::Description::Media media = track->description();
        media.addSSRC(kOutgoingSsrc, "participant-" + user_.id, user_.id, user_.id);
        track->setDescription(std::move(media));

        outgoing_ = track;
        return;
      }

      track->onMessage(
          [this](rtc::binary packet) {
            if (!rtc::IsRtcp(packet)) {
              received_rtp_.fetch_add(1);
            }
          },
          [](const rtc::string&) {});
      incoming_.push_back(std::move(track));
    });

    signaling_.on_message([this](proto::Message message) { handle(std::move(message)); });
  }

  ~Participant() {
    // The signaling client goes first: after this returns nothing calls back
    // into this object any more.
    signaling_.disconnect();
    connection_->close();
  }

  Participant(const Participant&) = delete;
  Participant& operator=(const Participant&) = delete;
  Participant(Participant&&) = delete;
  Participant& operator=(Participant&&) = delete;

  [[nodiscard]] bool login() {
    if (!signaling_.connect().ok()) {
      return false;
    }
    if (!wait_until([this] { return signaling_.is_connected(); })) {
      return false;
    }
    if (!signaling_.send(proto::Authenticate{username_, "senha"}).ok()) {
      return false;
    }
    return wait_until([this] { return !user_.id.empty(); });
  }

  [[nodiscard]] bool create_room() {
    if (!signaling_.send(proto::CreateRoom{user_.id, "sala-dev"}).ok()) {
      return false;
    }
    return wait_until([this] { return !created_room_id_.empty(); });
  }

  [[nodiscard]] bool join(const std::string& room_id) {
    room_id_ = room_id;
    if (!signaling_.send(proto::JoinRoom{room_id, user_.id, ""}).ok()) {
      return false;
    }
    return wait_until([this] { return joined_.load(); });
  }

  [[nodiscard]] bool leave() { return signaling_.send(proto::LeaveRoom{room_id_, user_.id}).ok(); }

  [[nodiscard]] bool wait_until_media_connected() {
    return wait_until(
        [this] { return connection_->state() == rtc::PeerConnection::State::Connected; });
  }

  /// Sends `count` Opus packets, spaced like the 20 ms frames of section 9 of
  /// SPEC.md, so the SFU sees a realistic stream rather than a burst.
  [[nodiscard]] bool send_audio(int count) {
    std::shared_ptr<rtc::Track> track;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      track = outgoing_;
    }
    if (!track || !track->isOpen()) {
      return false;
    }

    constexpr std::uint32_t kSamplesPer20ms = 960;  // 48 kHz
    for (int i = 0; i < count; ++i) {
      track->send(make_rtp_packet(kOutgoingSsrc, static_cast<std::uint16_t>(i),
                                  static_cast<std::uint32_t>(i) * kSamplesPer20ms, 111));
      std::this_thread::sleep_for(5ms);
    }
    return true;
  }

  [[nodiscard]] const dv::models::User& user() const { return user_; }
  [[nodiscard]] const std::string& created_room_id() const { return created_room_id_; }
  [[nodiscard]] std::uint64_t received_rtp() const { return received_rtp_.load(); }
  [[nodiscard]] std::string last_offer_sdp() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_offer_sdp_;
  }
  [[nodiscard]] std::string last_answer_sdp() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_answer_sdp_;
  }
  [[nodiscard]] std::size_t incoming_track_count() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return incoming_.size();
  }
  [[nodiscard]] bool has_outgoing_track() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return outgoing_ != nullptr;
  }

 private:
  void handle(proto::Message message) {
    if (const auto* authenticated = std::get_if<proto::Authenticated>(&message)) {
      user_ = authenticated->user;
      return;
    }
    if (const auto* created = std::get_if<proto::RoomCreated>(&message)) {
      created_room_id_ = created->room_id;
      return;
    }
    if (const auto* joined = std::get_if<proto::UserJoined>(&message)) {
      if (joined->user.id == user_.id) {
        joined_ = true;
      }
      return;
    }
    if (const auto* offer = std::get_if<proto::Offer>(&message)) {
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        last_offer_sdp_ = offer->sdp;
      }
      // libdatachannel answers on its own once the offer is applied.
      connection_->setRemoteDescription(
          rtc::Description(offer->sdp, rtc::Description::Type::Offer));
      return;
    }
    if (const auto* candidate = std::get_if<proto::IceCandidate>(&message)) {
      connection_->addRemoteCandidate(rtc::Candidate(candidate->candidate, candidate->sdp_mid));
      return;
    }
  }

  std::string username_;
  SignalingClient signaling_;
  std::shared_ptr<rtc::PeerConnection> connection_;

  dv::models::User user_;
  std::string created_room_id_;
  std::string room_id_;
  std::atomic<bool> joined_{false};
  std::atomic<std::uint64_t> received_rtp_{0};

  std::mutex mutex_;
  std::shared_ptr<rtc::Track> outgoing_;
  std::vector<std::shared_ptr<rtc::Track>> incoming_;
  std::string last_offer_sdp_;
  std::string last_answer_sdp_;
};

class SfuTest : public ::testing::Test {
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
    for (const char* name : {"ana", "bruno", "carla", "diego", "elena"}) {
      ASSERT_TRUE(server_->add_user(name, "senha", name).ok());
    }
    server_->start();
    ASSERT_NE(server_->port(), 0);
    ASSERT_NE(server_->audio_router(), nullptr);
  }

  void TearDown() override {
    participants_.clear();
    server_->stop();
    server_.reset();
  }

  Participant& add(const std::string& username) {
    participants_.push_back(std::make_unique<Participant>(server_->port(), username));
    return *participants_.back();
  }

  std::unique_ptr<SignalingServer> server_;
  std::vector<std::unique_ptr<Participant>> participants_;
};

TEST_F(SfuTest, TheServerOffersAsSoonAsAParticipantJoins) {
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  ASSERT_TRUE(ana.join(ana.created_room_id()));

  // One session, and the offer that carries the participant's own microphone.
  EXPECT_TRUE(wait_until([&] { return server_->audio_router()->session_count() == 1; }));
  EXPECT_TRUE(wait_until([&] { return !ana.last_offer_sdp().empty(); }));
  EXPECT_TRUE(wait_until([&] { return ana.has_outgoing_track(); }));

  // Alone in the room, there is nobody to listen to yet.
  EXPECT_EQ(server_->audio_router()->outbound_track_count(ana.user().id), 0U);
  EXPECT_TRUE(ana.wait_until_media_connected());
}

TEST_F(SfuTest, EachParticipantGetsOneTrackPerOtherParticipant) {
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_media_connected());

  Participant& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_media_connected());

  auto* router = server_->audio_router();
  EXPECT_TRUE(wait_until([&] { return router->session_count() == 2; }));
  EXPECT_TRUE(wait_until([&] { return router->outbound_track_count(ana.user().id) == 1; }));
  EXPECT_TRUE(wait_until([&] { return router->outbound_track_count(bruno.user().id) == 1; }));

  // Whose voice a track carries has to be discoverable by the client, which is
  // what the msid is for.
  EXPECT_TRUE(wait_until(
      [&] { return bruno.last_offer_sdp().find("msid:" + ana.user().id) != std::string::npos; }));
}

TEST_F(SfuTest, AudioReachesTheOtherParticipant) {
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_media_connected());

  Participant& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));
  ASSERT_TRUE(bruno.wait_until_media_connected());

  // Both ends have to have finished the renegotiation that added the tracks.
  ASSERT_TRUE(wait_until([&] { return bruno.incoming_track_count() >= 1; }));
  ASSERT_TRUE(wait_until([&] { return ana.has_outgoing_track(); }));

  ASSERT_TRUE(ana.send_audio(50));

  EXPECT_TRUE(wait_until([&] { return bruno.received_rtp() > 0; }))
      << "no audio arrived. The SFU received " << server_->audio_router()->packets_received()
      << " packets and forwarded " << server_->audio_router()->packets_forwarded();
  EXPECT_GT(server_->audio_router()->packets_forwarded(), 0U);

  // The sender must not hear themselves: forwarding to the source would be an
  // echo the client cannot cancel.
  EXPECT_EQ(ana.received_rtp(), 0U);
}

TEST_F(SfuTest, FiveParticipantsEachGetFourTracks) {
  // The room limit of the MVP, section 3 of SPEC.md. Five participants is
  // twenty forwarding paths, and each of them has to exist.
  Participant& first = add("ana");
  ASSERT_TRUE(first.login());
  ASSERT_TRUE(first.create_room());
  const std::string room = first.created_room_id();
  ASSERT_TRUE(first.join(room));

  std::vector<Participant*> everyone{&first};
  for (const char* name : {"bruno", "carla", "diego", "elena"}) {
    Participant& participant = add(name);
    ASSERT_TRUE(participant.login()) << name;
    ASSERT_TRUE(participant.join(room)) << name;
    everyone.push_back(&participant);
  }

  auto* router = server_->audio_router();
  EXPECT_TRUE(wait_until([&] { return router->session_count() == 5; }));

  for (Participant* participant : everyone) {
    EXPECT_TRUE(wait_until([&] {
      return router->outbound_track_count(participant->user().id) == 4;
    })) << "participant "
        << participant->user().id << " is missing tracks";
    EXPECT_TRUE(participant->wait_until_media_connected());
  }

  // One of them talking has to reach the other four.
  ASSERT_TRUE(wait_until([&] { return everyone.back()->has_outgoing_track(); }));
  ASSERT_TRUE(everyone.back()->send_audio(50));

  EXPECT_TRUE(wait_until([&] { return router->packets_forwarded() >= 4; }))
      << "forwarded " << router->packets_forwarded();
}

TEST_F(SfuTest, LeavingTakesTheSessionAndTheTracksAway) {
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));

  Participant& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));

  auto* router = server_->audio_router();
  ASSERT_TRUE(wait_until([&] { return router->outbound_track_count(bruno.user().id) == 1; }));

  ASSERT_TRUE(ana.leave());

  EXPECT_TRUE(wait_until([&] { return router->session_count() == 1; }));
  EXPECT_TRUE(wait_until([&] { return router->outbound_track_count(bruno.user().id) == 0; }));
}

}  // namespace
