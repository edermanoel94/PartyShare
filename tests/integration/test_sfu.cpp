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
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>
#include <rtc/rtc.hpp>

#include "network/signaling_client.hpp"
#include "sfu/media_router.hpp"
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
/// The same for the screen it shares.
constexpr std::uint32_t kOutgoingVideoSsrc = 0xBEEFCAFE;

constexpr int kOpusPayloadType = 111;
constexpr int kH264PayloadType = 96;

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

/// The port out of an SDP candidate line, as in
/// "a=candidate:1 1 UDP 2130706431 127.0.0.1 50003 typ host".
///
/// The address and the port are the two fields before "typ", which is a more
/// stable place to count from than the start of the line: the foundation is an
/// arbitrary token and libdatachannel may or may not prefix the "a=".
[[nodiscard]] std::optional<std::uint16_t> candidate_port(const std::string& line) {
  std::istringstream fields(line);
  std::vector<std::string> parts;
  for (std::string part; fields >> part;) {
    parts.push_back(std::move(part));
  }
  for (std::size_t i = 1; i < parts.size(); ++i) {
    if (parts[i] != "typ") {
      continue;
    }
    try {
      const int value = std::stoi(parts[i - 1]);
      if (value < 1 || value > 65535) {
        return std::nullopt;
      }
      return static_cast<std::uint16_t>(value);
    } catch (const std::exception&) {
      return std::nullopt;
    }
  }
  return std::nullopt;
}

/// The three header fields a video receiver orders and times a stream by.
///
/// The SFU rewrites the first one per destination and forwards the other two
/// untouched, which is only invisible while a single source ever feeds a given
/// outbound track.
struct RtpHeaderFields {
  std::uint32_t ssrc = 0;
  std::uint16_t sequence = 0;
  std::uint32_t timestamp = 0;
};

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
      const bool is_video = track->description().type() == "video";

      if (track->direction() == rtc::Description::Direction::SendOnly) {
        // The m-lines the server marked recvonly: this participant's own
        // microphone and its own screen.
        //
        // The SSRC has to be declared in the answer. Everything is bundled on
        // one transport, so it is the only thing that tells the server which
        // track an arriving packet belongs to, and without it the media is
        // silently dropped on arrival. libwebrtc always declares one; this is
        // the same thing by hand.
        const std::uint32_t ssrc = is_video ? kOutgoingVideoSsrc : kOutgoingSsrc;
        const std::string label = (is_video ? "screen-" : "participant-") + user_.id;

        rtc::Description::Media media = track->description();
        media.addSSRC(ssrc, label, user_.id, label);
        track->setDescription(std::move(media));

        if (is_video) {
          outgoing_video_ = track;
        } else {
          outgoing_ = track;
        }
        return;
      }

      // Receiver reports, and on video the PLI that asks the sender for an
      // intra frame. libwebrtc does both by itself; here it is explicit.
      track->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

      auto* counter = is_video ? &received_video_rtp_ : &received_rtp_;
      track->onMessage(
          [this, counter, is_video](rtc::binary packet) {
            if (rtc::IsRtcp(packet)) {
              return;
            }
            counter->fetch_add(1);
            if (!is_video || packet.size() < sizeof(rtc::RtpHeader)) {
              return;
            }
            // What a decoder reads before it looks at a single byte of
            // payload. Kept so a test can assert on the shape of the stream a
            // viewer is handed, not merely on how much of it arrives.
            const auto* header = reinterpret_cast<const rtc::RtpHeader*>(packet.data());
            // Not named `lock`: this runs inside the onTrack lambda's scope,
            // which has one of its own, and -Wshadow is an error on GCC.
            const std::lock_guard<std::mutex> recording(mutex_);
            video_headers_.push_back(RtpHeaderFields{.ssrc = header->ssrc(),
                                                     .sequence = header->seqNumber(),
                                                     .timestamp = header->timestamp()});
          },
          [](const rtc::string&) {});
      if (is_video) {
        incoming_video_.push_back(std::move(track));
      } else {
        incoming_.push_back(std::move(track));
      }
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
    if (!signaling_.send(proto::Authenticate{username_, "password"}).ok()) {
      return false;
    }
    return wait_until([this] { return !user_.id.empty(); });
  }

  [[nodiscard]] bool create_room() {
    if (!signaling_.send(proto::CreateRoom{user_.id, ""}).ok()) {
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

  /// Waits for media to come up, and stops the moment it cannot come up.
  ///
  /// This used to be `wait_until(state() == Connected)`, which treats a dead
  /// connection as one that has not arrived yet. `Failed` is terminal -
  /// libdatachannel leaves it only through an ICE restart and nothing here
  /// performs one - and `Closed` is terminal by definition, so polling either
  /// of them for the rest of the ten seconds waits for something that cannot
  /// happen.
  ///
  /// It cost more than the ten seconds. Both outcomes reported the same bare
  /// `Actual: false`, so a connection that died in two milliseconds and one
  /// that was merely slow were indistinguishable in the failure, and the answer
  /// was only in the server log further up - which is where a CI failure of
  /// this test was read from, once. Returning an AssertionResult puts the state
  /// it actually reached in the message, and every `ASSERT_TRUE` around the
  /// twenty-three call sites prints it without changing.
  [[nodiscard]] ::testing::AssertionResult wait_until_media_connected() {
    const auto deadline = std::chrono::steady_clock::now() + kTimeout;
    for (;;) {
      const rtc::PeerConnection::State state = connection_->state();
      if (state == rtc::PeerConnection::State::Connected) {
        return ::testing::AssertionSuccess();
      }
      if (state == rtc::PeerConnection::State::Failed ||
          state == rtc::PeerConnection::State::Closed) {
        return ::testing::AssertionFailure() << username_ << "'s media connection reached " << state
                                             << ", which it does not leave";
      }
      // Disconnected is deliberately not in that list: ICE recovers from it,
      // and a test that gave up there would fail on a hiccup the product is
      // built to survive.
      if (std::chrono::steady_clock::now() >= deadline) {
        return ::testing::AssertionFailure() << username_ << "'s media connection was still "
                                             << state << " after " << kTimeout.count() << " ms";
      }
      std::this_thread::sleep_for(10ms);
    }
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
                                  static_cast<std::uint32_t>(i) * kSamplesPer20ms,
                                  kOpusPayloadType));
      std::this_thread::sleep_for(5ms);
    }
    return true;
  }

  /// Sends `count` video packets on the screen share track, spaced like frames
  /// at 30 FPS on the 90 kHz clock H.264 uses.
  [[nodiscard]] bool send_video(int count) { return send_video_from(0, 0, count); }

  /// The same, starting from a given sequence number and timestamp.
  ///
  /// Two participants never agree on either: RFC 3550 section 5.1 has both
  /// chosen at random when a source starts, and each encoder keeps its own
  /// clock afterwards. A test that wants to know what a viewer sees when the
  /// screen share changes hands has to say so rather than let both sides start
  /// at zero by accident.
  [[nodiscard]] bool send_video_from(std::uint16_t base_sequence, std::uint32_t base_timestamp,
                                     int count) {
    std::shared_ptr<rtc::Track> track;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      track = outgoing_video_;
    }
    if (!track || !track->isOpen()) {
      return false;
    }

    constexpr std::uint32_t kTicksPerFrame = 3000;  // 90 kHz / 30 FPS
    for (int i = 0; i < count; ++i) {
      track->send(make_rtp_packet(kOutgoingVideoSsrc, static_cast<std::uint16_t>(base_sequence + i),
                                  base_timestamp + static_cast<std::uint32_t>(i) * kTicksPerFrame,
                                  kH264PayloadType));
      std::this_thread::sleep_for(5ms);
    }
    return true;
  }

  /// Sends video packets carrying RTP padding, which is what libwebrtc does
  /// when it probes for bandwidth.
  ///
  /// libdatachannel's sender report builder asserts on a padded packet, so
  /// forwarding one aborts the whole server. Any participant can send these,
  /// so the SFU has to survive them.
  [[nodiscard]] bool send_padded_video(int count) {
    std::shared_ptr<rtc::Track> track;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      track = outgoing_video_;
    }
    if (!track || !track->isOpen()) {
      return false;
    }

    for (int i = 0; i < count; ++i) {
      rtc::binary packet = make_rtp_packet(kOutgoingVideoSsrc, static_cast<std::uint16_t>(i),
                                           static_cast<std::uint32_t>(i) * 3000, kH264PayloadType);
      // RFC 3550 section 5.1: the padding bit in the header, and the number of
      // padding octets in the last one.
      auto* first = reinterpret_cast<std::uint8_t*>(packet.data());
      *first |= 0x20U;
      packet.back() = std::byte{4};
      track->send(std::move(packet));
      std::this_thread::sleep_for(5ms);
    }
    return true;
  }

  /// Tells the SFU how much this viewer's own link can carry, which is what a
  /// receiver's congestion controller does through REMB.
  [[nodiscard]] bool request_bitrate(unsigned int bitrate_bps) {
    std::shared_ptr<rtc::Track> track;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (incoming_video_.empty()) {
        return false;
      }
      track = incoming_video_.front();
    }
    return track->requestBitrate(bitrate_bps);
  }

  /// Asks the sender for an intra frame, the way a viewer that joined mid
  /// transmission does.
  [[nodiscard]] bool request_keyframe() {
    std::shared_ptr<rtc::Track> track;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (incoming_video_.empty()) {
        return false;
      }
      track = incoming_video_.front();
    }
    return track->requestKeyframe();
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
  /// The ports of every candidate the SFU has sent so far. Its own sockets,
  /// not this participant's, which is what makes them worth asserting on.
  [[nodiscard]] std::vector<std::uint16_t> sfu_candidate_ports() {
    const std::lock_guard<std::mutex> lock(mutex_);
    std::vector<std::uint16_t> ports;
    for (const std::string& candidate : remote_candidates_) {
      if (const auto port = candidate_port(candidate)) {
        ports.push_back(*port);
      }
    }
    return ports;
  }
  [[nodiscard]] std::uint64_t received_video_rtp() const { return received_video_rtp_.load(); }
  /// Forgets what has arrived so far, so that a test can let one participant
  /// talk at a time and still tell whose packets it is looking at.
  void forget_received() {
    received_rtp_.store(0);
    received_video_rtp_.store(0);
  }
  /// Every video packet header this participant has been handed, in order.
  [[nodiscard]] std::vector<RtpHeaderFields> video_headers() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return video_headers_;
  }
  [[nodiscard]] std::size_t incoming_track_count() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return incoming_.size();
  }
  [[nodiscard]] std::size_t incoming_video_track_count() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return incoming_video_.size();
  }
  /// Whether this participant can send on its microphone track yet.
  ///
  /// Open, and not merely present. The two are different moments and the tests
  /// below depend on the second one: onTrack fires when the remote description
  /// is applied, which is well before DTLS finishes, so the track object exists
  /// for a while during which send() has nowhere to put a packet.
  ///
  /// Waiting on existence and then calling send_audio was a race the whole file
  /// carried. It lost it under AddressSanitizer with five participants, where
  /// the gap between the two moments is widest, and the failure read as
  /// `send_audio(50)` returning false for no stated reason.
  [[nodiscard]] bool has_open_outgoing_track() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return outgoing_ != nullptr && outgoing_->isOpen();
  }
  /// The same, for the screen share.
  [[nodiscard]] bool has_open_outgoing_video_track() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return outgoing_video_ != nullptr && outgoing_video_->isOpen();
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
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        remote_candidates_.push_back(candidate->candidate);
      }
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
  std::atomic<std::uint64_t> received_video_rtp_{0};

  std::mutex mutex_;
  std::shared_ptr<rtc::Track> outgoing_;
  std::shared_ptr<rtc::Track> outgoing_video_;
  std::vector<std::shared_ptr<rtc::Track>> incoming_;
  std::vector<std::shared_ptr<rtc::Track>> incoming_video_;
  std::vector<RtpHeaderFields> video_headers_;
  std::string last_offer_sdp_;
  std::string last_answer_sdp_;
  std::vector<std::string> remote_candidates_;
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
      ASSERT_TRUE(server_->add_user(name, "password", name).ok());
    }
    server_->start();
    ASSERT_NE(server_->port(), 0);
    ASSERT_NE(server_->media_router(), nullptr);
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
  EXPECT_TRUE(wait_until([&] { return server_->media_router()->session_count() == 1; }));
  EXPECT_TRUE(wait_until([&] { return !ana.last_offer_sdp().empty(); }));
  EXPECT_TRUE(wait_until([&] { return ana.has_open_outgoing_track(); }));

  // Alone in the room, there is nobody to listen to yet.
  EXPECT_EQ(server_->media_router()->outbound_track_count(ana.user().id), 0U);
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

  auto* router = server_->media_router();
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
  ASSERT_TRUE(wait_until([&] { return ana.has_open_outgoing_track(); }));

  ASSERT_TRUE(ana.send_audio(50));

  EXPECT_TRUE(wait_until([&] { return bruno.received_rtp() > 0; }))
      << "no audio arrived. The SFU received " << server_->media_router()->audio_packets_received()
      << " packets and forwarded " << server_->media_router()->audio_packets_forwarded();
  EXPECT_GT(server_->media_router()->audio_packets_forwarded(), 0U);

  // The sender must not hear themselves: forwarding to the source would be an
  // echo the client cannot cancel.
  EXPECT_EQ(ana.received_rtp(), 0U);
}

TEST_F(SfuTest, EveryoneIsHeardWhenTheRoomFillsUpWithoutPausing) {
  // The other tests wait for each participant's media to be connected before
  // the next one joins, which is not how a room fills up. People click at the
  // same time, and every join renegotiates with everybody already inside: the
  // SFU has to defer an offer to a peer that is mid exchange and pick it up
  // again afterwards, and a microphone that is never negotiated is a
  // microphone that stays silent until its owner leaves and comes back.
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));

  std::vector<Participant*> everyone{&ana};
  for (const char* name : {"bruno", "carla", "diego", "elena"}) {
    Participant& participant = add(name);
    ASSERT_TRUE(participant.login()) << name;
    // No wait_until_media_connected here, on purpose: that is the pause this
    // test exists to remove.
    ASSERT_TRUE(participant.join(room)) << name;
    everyone.push_back(&participant);
  }

  for (Participant* participant : everyone) {
    ASSERT_TRUE(participant->wait_until_media_connected());
    ASSERT_TRUE(wait_until([&] { return participant->has_open_outgoing_track(); }));
    ASSERT_TRUE(wait_until([&] {
      return participant->incoming_track_count() == everyone.size() - 1;
    })) << "a participant was never given a track for everybody else";
  }

  // One at a time, so that a packet that arrives says who it came from.
  for (Participant* talker : everyone) {
    for (Participant* listener : everyone) {
      listener->forget_received();
    }

    ASSERT_TRUE(talker->send_audio(30));

    for (Participant* listener : everyone) {
      if (listener == talker) {
        continue;
      }
      EXPECT_TRUE(wait_until([&] { return listener->received_rtp() > 0; }))
          << "a microphone reached nobody: the SFU received "
          << server_->media_router()->audio_packets_received() << " packets and forwarded "
          << server_->media_router()->audio_packets_forwarded();
    }
  }
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

  auto* router = server_->media_router();
  EXPECT_TRUE(wait_until([&] { return router->session_count() == 5; }));

  for (Participant* participant : everyone) {
    EXPECT_TRUE(wait_until([&] {
      return router->outbound_track_count(participant->user().id) == 4;
    })) << "participant "
        << participant->user().id << " is missing tracks";
    EXPECT_TRUE(participant->wait_until_media_connected());
  }

  // One of them talking has to reach the other four.
  ASSERT_TRUE(wait_until([&] { return everyone.back()->has_open_outgoing_track(); }));
  ASSERT_TRUE(everyone.back()->send_audio(50));

  EXPECT_TRUE(wait_until([&] { return router->audio_packets_forwarded() >= 4; }))
      << "forwarded " << router->audio_packets_forwarded();
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

  auto* router = server_->media_router();
  ASSERT_TRUE(wait_until([&] { return router->outbound_track_count(bruno.user().id) == 1; }));

  ASSERT_TRUE(ana.leave());

  EXPECT_TRUE(wait_until([&] { return router->session_count() == 1; }));
  EXPECT_TRUE(wait_until([&] { return router->outbound_track_count(bruno.user().id) == 0; }));
}

TEST_F(SfuTest, EverySessionHasItsVideoTracksFromTheStart) {
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));

  auto* router = server_->media_router();

  // Both video m-lines exist before anyone asks to share anything. That is
  // what makes starting and stopping a share cost no renegotiation, which
  // section 5.2 of SPEC.md needs to work without interrupting the call.
  //
  // The server's half needs no connection: the tracks are created with the
  // session. The participant's half does - a track is open only once media is
  // up - so the wait for the connection comes between the two, and for the
  // reason every other test here has it: a connection that died at birth is
  // then reported as what it is, rather than as ten seconds of a track that
  // never opened. The CI run of PR #53 spent exactly those ten seconds here,
  // on a connection the SFU had declared failed 52 ms after creating it.
  EXPECT_TRUE(wait_until([&] { return router->has_video_tracks(ana.user().id); }));
  ASSERT_TRUE(ana.wait_until_media_connected());
  EXPECT_TRUE(wait_until([&] { return ana.has_open_outgoing_video_track(); }))
      << "the participant was never offered an m-line to send its screen on";
  EXPECT_TRUE(wait_until([&] { return ana.incoming_video_track_count() == 1; }))
      << "the participant was never offered the shared screen";
}

TEST_F(SfuTest, OneParticipantsScreenReachesTheOthers) {
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));

  Participant& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));

  ASSERT_TRUE(ana.wait_until_media_connected());
  ASSERT_TRUE(bruno.wait_until_media_connected());
  ASSERT_TRUE(wait_until([&] { return ana.has_open_outgoing_video_track(); }));
  ASSERT_TRUE(wait_until([&] { return bruno.incoming_video_track_count() == 1; }));

  ASSERT_TRUE(ana.send_video(50));

  auto* router = server_->media_router();
  EXPECT_TRUE(wait_until([&] { return router->video_packets_received() > 0; }))
      << "the SFU received no video";
  EXPECT_TRUE(wait_until([&] { return bruno.received_video_rtp() > 0; }))
      << "the SFU received " << router->video_packets_received() << " video packets and forwarded "
      << router->video_packets_forwarded();

  // And it does not come back to whoever sent it.
  EXPECT_EQ(ana.received_video_rtp(), 0U) << "a participant is watching their own screen";
}

TEST_F(SfuTest, TheSecondSharerReachesTheViewerAsOneContinuousStream) {
  // Ana shares, stops, and Bruno shares instead. Carla watches throughout.
  //
  // Both of them arrive on the one outbound video track Carla was given when
  // she joined, under the one SSRC the SFU stamps on it. A receiver reorders,
  // dejitters and times a stream by the sequence number and the timestamp it
  // finds under a given SSRC, so those have to keep moving forwards across the
  // handover or the second sharer looks to the decoder like a burst of very
  // old packets from the first one.
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));

  Participant& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));

  Participant& carla = add("carla");
  ASSERT_TRUE(carla.login());
  ASSERT_TRUE(carla.join(room));

  ASSERT_TRUE(ana.wait_until_media_connected());
  ASSERT_TRUE(bruno.wait_until_media_connected());
  ASSERT_TRUE(carla.wait_until_media_connected());
  ASSERT_TRUE(wait_until([&] { return ana.has_open_outgoing_video_track(); }));
  ASSERT_TRUE(wait_until([&] { return bruno.has_open_outgoing_video_track(); }));
  ASSERT_TRUE(wait_until([&] { return carla.incoming_video_track_count() == 1; }));

  // Ana's screen, from wherever her encoder happened to start.
  constexpr std::uint16_t kAnaFirstSequence = 40000;
  constexpr std::uint32_t kAnaFirstTimestamp = 900000;
  ASSERT_TRUE(ana.send_video_from(kAnaFirstSequence, kAnaFirstTimestamp, 30));
  ASSERT_TRUE(wait_until([&] { return carla.received_video_rtp() >= 30; }))
      << "the first sharer never reached the viewer";
  const std::size_t after_ana = carla.video_headers().size();

  // She stops, and Bruno starts. His encoder chose its own starting point, as
  // every encoder does, and it has no relation to hers.
  constexpr std::uint16_t kBrunoFirstSequence = 100;
  constexpr std::uint32_t kBrunoFirstTimestamp = 5000;
  ASSERT_TRUE(bruno.send_video_from(kBrunoFirstSequence, kBrunoFirstTimestamp, 30));
  ASSERT_TRUE(wait_until([&] { return carla.video_headers().size() > after_ana; }))
      << "the second sharer never reached the viewer at all";

  const std::vector<RtpHeaderFields> headers = carla.video_headers();
  ASSERT_GE(headers.size(), 2U);

  // One SSRC for the whole handover, which is the design: the outbound track
  // belongs to the viewer, not to whoever is sharing.
  for (const RtpHeaderFields& header : headers) {
    EXPECT_EQ(header.ssrc, headers.front().ssrc) << "the outbound video SSRC changed mid call";
  }

  // And therefore one sequence space and one clock. Compared as signed 16 bit
  // differences, which is how RFC 3550 says a receiver tells a later packet
  // from an earlier one under wraparound.
  for (std::size_t i = 1; i < headers.size(); ++i) {
    const auto step = static_cast<std::int16_t>(headers[i].sequence - headers[i - 1].sequence);
    EXPECT_GT(step, 0) << "packet " << i << " went backwards in the sequence space, from "
                       << headers[i - 1].sequence << " to " << headers[i].sequence;
    EXPECT_GE(headers[i].timestamp, headers[i - 1].timestamp)
        << "packet " << i << " went backwards in time, from " << headers[i - 1].timestamp << " to "
        << headers[i].timestamp;
  }
}

TEST_F(SfuTest, TheSlowestViewerLimitsWhatTheSharerIsAskedFor) {
  // The SFU is the only party that hears every viewer, so it is the only one
  // that can hold the sharer to what the slowest of them can take.
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));

  Participant& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));

  ASSERT_TRUE(ana.wait_until_media_connected());
  ASSERT_TRUE(bruno.wait_until_media_connected());
  ASSERT_TRUE(wait_until([&] { return ana.has_open_outgoing_video_track(); }));
  ASSERT_TRUE(wait_until([&] { return bruno.incoming_video_track_count() == 1; }));

  // The share has to be flowing first: what the SFU asks for is only sent
  // while packets are arriving to report on.
  ASSERT_TRUE(ana.send_video(30));

  constexpr unsigned int kViewerCanTake = 600000;
  ASSERT_TRUE(bruno.request_bitrate(kViewerCanTake));

  auto* router = server_->media_router();
  ASSERT_TRUE(wait_until([&] { return router->video_repair_stats().viewer_reports_received > 0; }))
      << "the viewer's report never reached the SFU";
  EXPECT_EQ(router->video_repair_stats().viewer_ceiling_kbps, 600);

  // And it reaches the sharer. The SFU reports once a second, so this has to
  // be more than a second of video, spaced like the real thing.
  ASSERT_TRUE(ana.send_video(250));
  EXPECT_TRUE(wait_until([&] {
    const int target = router->video_repair_stats().target_kbps;
    return target > 0 && target <= 600;
  })) << "the sharer is still being asked for "
      << router->video_repair_stats().target_kbps << " kbps";
}

TEST_F(SfuTest, APaddedPacketIsDroppedRatherThanTakingTheServerDown) {
  // Found while enabling bandwidth estimation: negotiating abs-send-time makes
  // libwebrtc probe, probing is padding, and libdatachannel asserts on a
  // padded packet while building the sender report. The assert is inside a
  // library we do not control, so the packet has to stop here.
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));

  Participant& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));

  ASSERT_TRUE(ana.wait_until_media_connected());
  ASSERT_TRUE(bruno.wait_until_media_connected());
  ASSERT_TRUE(wait_until([&] { return ana.has_open_outgoing_video_track(); }));
  ASSERT_TRUE(wait_until([&] { return bruno.incoming_video_track_count() == 1; }));

  ASSERT_TRUE(ana.send_padded_video(20));

  auto* router = server_->media_router();
  EXPECT_EQ(router->video_packets_received(), 0U) << "a padded packet was taken as media";
  EXPECT_EQ(bruno.received_video_rtp(), 0U);

  // And the call is still a call: the padded packets cost their own frames and
  // nothing else.
  ASSERT_TRUE(ana.send_video(30));
  EXPECT_TRUE(wait_until([&] { return bruno.received_video_rtp() > 0; }))
      << "the server survived the padded packets but stopped forwarding video";
}

TEST_F(SfuTest, VideoAndAudioTravelWithoutGettingMixedUp) {
  // Both run over one bundled transport with rewritten SSRCs, so the failure
  // this guards against is real: audio arriving on the video track, or the
  // other way round, would still look like traffic flowing.
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));

  Participant& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));

  ASSERT_TRUE(ana.wait_until_media_connected());
  ASSERT_TRUE(bruno.wait_until_media_connected());
  ASSERT_TRUE(wait_until([&] { return ana.has_open_outgoing_track(); }));
  ASSERT_TRUE(wait_until([&] { return ana.has_open_outgoing_video_track(); }));

  ASSERT_TRUE(ana.send_audio(30));
  ASSERT_TRUE(ana.send_video(30));

  EXPECT_TRUE(wait_until([&] { return bruno.received_rtp() > 0; })) << "no audio arrived";
  EXPECT_TRUE(wait_until([&] { return bruno.received_video_rtp() > 0; })) << "no video arrived";

  auto* router = server_->media_router();
  EXPECT_GT(router->audio_packets_forwarded(), 0U);
  EXPECT_GT(router->video_packets_forwarded(), 0U);
}

TEST_F(SfuTest, AKeyframeRequestReachesTheSharer) {
  // Section 5.2 of SPEC.md: someone who joins in the middle of a transmission
  // sees nothing until the sender produces an intra frame. Nothing between the
  // two decodes anything, so the request has to be carried across by the SFU.
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));

  Participant& bruno = add("bruno");
  ASSERT_TRUE(bruno.login());
  ASSERT_TRUE(bruno.join(room));

  ASSERT_TRUE(ana.wait_until_media_connected());
  ASSERT_TRUE(bruno.wait_until_media_connected());
  ASSERT_TRUE(wait_until([&] { return ana.has_open_outgoing_video_track(); }));
  ASSERT_TRUE(wait_until([&] { return bruno.incoming_video_track_count() == 1; }));

  // Video has to be flowing first, which is also the situation this is for: a
  // viewer asks for an intra frame because a transmission is already under way
  // and they arrived in the middle of it. It is the arriving packets that tell
  // the receiver which stream to address the request to.
  ASSERT_TRUE(ana.send_video(20));
  ASSERT_TRUE(wait_until([&] { return bruno.received_video_rtp() > 0; }));

  auto* router = server_->media_router();
  ASSERT_EQ(router->keyframe_requests_forwarded(), 0U);

  ASSERT_TRUE(bruno.request_keyframe()) << "the viewer could not ask for an intra frame";

  EXPECT_TRUE(wait_until([&] { return router->keyframe_requests_forwarded() > 0; }))
      << "the request stopped at the SFU instead of reaching the sender";
}

TEST_F(SfuTest, TheMediaIsEncryptedAndNothingElseIsOffered) {
  // Section 17 of SPEC.md: no audio or video without encryption. WebRTC gives
  // that by mandating DTLS-SRTP, and this is the evidence rather than the
  // claim: every media line is negotiated over the secure profile, and both
  // ends publish a certificate fingerprint.
  Participant& ana = add("ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  const std::string room = ana.created_room_id();
  ASSERT_TRUE(ana.join(room));
  ASSERT_TRUE(ana.wait_until_media_connected());

  const std::string offer = ana.last_offer_sdp();
  const std::string answer = ana.last_answer_sdp();
  ASSERT_FALSE(offer.empty()) << "no offer was seen, so nothing was checked";
  ASSERT_FALSE(answer.empty()) << "no answer was seen, so nothing was checked";

  for (const auto& [name, sdp] : {std::pair{"offer", offer}, std::pair{"answer", answer}}) {
    EXPECT_NE(sdp.find("a=fingerprint:"), std::string::npos)
        << name << " carries no certificate fingerprint, so the DTLS peer is unauthenticated";
    EXPECT_NE(sdp.find("RTP/SAVPF"), std::string::npos)
        << name << " does not use the secure RTP profile";
    // RTP/AVP is the same thing without encryption. A media line offering it
    // would be one that could carry plaintext.
    EXPECT_EQ(sdp.find(" RTP/AVP "), std::string::npos) << name << " offers unencrypted media";
  }
}

// The SFU's own media sockets, and the only reason the range setting exists: a
// firewall in front of the server can allow these ports and nothing else.
TEST(SfuPortRange, TheSfuBindsInsideTheConfiguredRange) {
  constexpr std::uint16_t kBegin = 50000;
  constexpr std::uint16_t kEnd = 50100;

  SignalingServer::Options options;
  options.bind_address = "127.0.0.1";
  options.port = 0;
  options.hub.max_participants_per_room = 5;
  options.hub.heartbeat_interval = 1000ms;
  options.hub.heartbeat_timeout = 30000ms;
  options.enable_sfu = true;
  // No STUN, so every candidate the SFU sends is a host candidate on a socket
  // it bound itself, which is exactly what the range governs.
  options.sfu.ice_servers.clear();
  options.sfu.port_range_begin = kBegin;
  options.sfu.port_range_end = kEnd;

  SignalingServer server(options);
  ASSERT_TRUE(server.add_user("ana", "password", "Ana").ok());
  server.start();

  Participant ana(server.port(), "ana");
  ASSERT_TRUE(ana.login());
  ASSERT_TRUE(ana.create_room());
  ASSERT_TRUE(ana.join(ana.created_room_id()));

  ASSERT_TRUE(wait_until([&] { return !ana.sfu_candidate_ports().empty(); }));
  const std::vector<std::uint16_t> ports = ana.sfu_candidate_ports();
  for (const std::uint16_t port : ports) {
    EXPECT_GE(port, kBegin);
    EXPECT_LE(port, kEnd);
  }

  server.stop();
}

}  // namespace
