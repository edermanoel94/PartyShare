// What a bad network does to a call, measured rather than assumed.
//
// Section 22 of SPEC.md asks the call to survive 5% packet loss with a smooth
// degradation and no drop, and task 2 of M8 asks for that to be produced with
// `tc netem`. scripts/netem.sh is that, and needs root. This is the other half,
// described in client/src/media/network_impairment.hpp: the client damages its
// own UDP sockets, which needs no privilege, runs on every platform, and
// impairs exactly the link between one participant and the SFU.
//
// Everything below the injector is real: real Opus, real SRTP, real jitter
// buffer, real RTCP feedback. What is simulated is only the wire.

#include <cstdio>

#include "media/network_impairment.hpp"
#include "media_test_client.hpp"

namespace {

using dv::client::media::network_impairment_counters;
using dv::client::media::NetworkImpairment;

/// Long enough for the loss to be more than a coincidence: at 50 audio packets
/// a second in each direction, fifteen seconds is some fifteen hundred packets
/// per link and around seventy five losses.
constexpr auto kUnderImpairment = 15000ms;

/// What 5% of loss sounds like, in concealment events per second at the
/// listener, measured on this machine on 2026-09-03 by the two tests below.
/// Without any repair the link produced 4.2 to 4.8 across runs, with 6% to 8%
/// of the samples invented. With RED alone it produced 0.46, with 0.74%: what
/// RED cannot reach is two packets lost back to back, which at the 10% the two
/// legs add up to happens about once every two seconds. With RED and
/// retransmission together it produced 0.00 to 0.13, with at most 0.33%: the
/// SFU asked for some seventy packets and all but a handful came back.
///
/// Both thresholds sit between the measurements with room on each side, so a
/// drift in either direction is caught before it is a regression.
constexpr double kConcealmentWithoutRepairPerSecond = 2.5;
constexpr double kConcealmentWithRepairPerSecond = 1.0;

class ImpairedNetworkTest : public MediaEndToEndTest {
 protected:
  void TearDown() override {
    // The impairment is process wide, and leaving it on would quietly damage
    // whatever runs next in this binary.
    dv::client::media::set_network_impairment({});
    MediaEndToEndTest::TearDown();
  }

  /// Two clients in a room, both connected, with audio moving between them.
  void start_a_call() {
    Client& ana = add("ana");
    ASSERT_TRUE(ana.login());
    room_ = ana.create_room();
    ASSERT_FALSE(room_.empty());
    ASSERT_TRUE(ana.join(room_));
    ASSERT_TRUE(ana.wait_until_in_call());

    Client& bruno = add("bruno");
    ASSERT_TRUE(bruno.login());
    ASSERT_TRUE(bruno.join(room_));
    ASSERT_TRUE(bruno.wait_until_in_call());

    ASSERT_TRUE(wait_until([&] { return server_->media_router()->audio_packets_forwarded() > 0; }))
        << "no audio was flowing before the network was touched, so nothing below would mean "
           "anything";
  }

  [[nodiscard]] Client& ana() { return *clients_.at(0); }
  [[nodiscard]] Client& bruno() { return *clients_.at(1); }

  std::string room_;

  /// A statistics report newer than the last one seen, with the moment it
  /// arrived. Reports come every few seconds and carry totals, so a rate is
  /// only honest between two reports and over the time that really passed
  /// between them.
  struct Report {
    media::AudioStats stats;
    std::chrono::steady_clock::time_point at;
  };
  [[nodiscard]] static Report fresh_report(Client& client) {
    const auto previous = client.last_stats_at();
    EXPECT_TRUE(wait_until([&] { return client.last_stats_at() > previous; }))
        << "no statistics report arrived";
    return Report{client.last_stats(), client.last_stats_at()};
  }

  /// What a loss sounds like, per second: every stretch the jitter buffer had
  /// to invent is one event. packets_lost cannot tell repair from damage - a
  /// packet the redundancy replaced still counts as lost - so this is the
  /// number that says whether a repair works.
  [[nodiscard]] static double concealment_events_per_second(const Report& before,
                                                            const Report& after) {
    const double seconds = std::chrono::duration<double>(after.at - before.at).count();
    return static_cast<double>(after.stats.concealment_events - before.stats.concealment_events) /
           seconds;
  }

  static void print_concealment(const Report& before, const Report& after) {
    std::printf("concealment      %.2f events/s, %llu of %llu samples invented\n",
                concealment_events_per_second(before, after),
                static_cast<unsigned long long>(after.stats.concealed_samples -
                                                before.stats.concealed_samples),
                static_cast<unsigned long long>(after.stats.total_samples_received -
                                                before.stats.total_samples_received));
    std::fflush(stdout);
  }
};

/// The same fixture with every repair turned off on the server: the offer as
/// it was before RED and retransmission existed, and what
/// `[audio] redundancy = false` and `[audio] retransmission = false` give back.
class ImpairedNetworkWithoutRepairTest : public ImpairedNetworkTest {
 protected:
  void configure(SignalingServer::Options& options) override {
    options.sfu.red_payload_type.reset();
    options.sfu.audio_nack = false;
  }
};

TEST_F(ImpairedNetworkTest, ACallSurvivesFivePercentPacketLoss) {
  start_a_call();

  const Report before = fresh_report(bruno());
  const std::uint64_t received_before = before.stats.packets_received;
  const std::uint64_t forwarded_before = server_->media_router()->audio_packets_forwarded();
  const std::uint64_t red_before = server_->media_router()->audio_red_packets_received();

  dv::client::media::reset_network_impairment_counters();
  dv::client::media::set_network_impairment({.loss = 0.05});
  std::this_thread::sleep_for(kUnderImpairment);

  const auto counters = network_impairment_counters();
  const Report after = fresh_report(bruno());
  const auto stats = after.stats;

  std::printf("\n--- audio on a link losing 5%% of packets, for %llu s ---\n",
              static_cast<unsigned long long>(kUnderImpairment.count() / 1000));
  std::printf("injected         %llu of %llu out, %llu of %llu in\n",
              static_cast<unsigned long long>(counters.packets_dropped_outbound),
              static_cast<unsigned long long>(counters.packets_sent),
              static_cast<unsigned long long>(counters.packets_dropped_inbound),
              static_cast<unsigned long long>(counters.packets_received));
  std::printf("receiver         %llu packets arrived, %llu counted lost\n",
              static_cast<unsigned long long>(stats.packets_received - received_before),
              static_cast<unsigned long long>(stats.packets_lost));
  std::printf("quality          rtt %.0f ms, jitter %.1f ms\n", stats.round_trip_time_ms,
              stats.jitter_ms);
  std::fflush(stdout);

  // First, that the damage was really done. Without this the rest of the case
  // would pass just as well on a perfect network.
  ASSERT_GT(counters.packets_sent, 500U) << "barely any packets went through the injector";
  ASSERT_GT(counters.packets_received, 500U);
  const double outbound_rate = static_cast<double>(counters.packets_dropped_outbound) /
                               static_cast<double>(counters.packets_sent);
  const double inbound_rate = static_cast<double>(counters.packets_dropped_inbound) /
                              static_cast<double>(counters.packets_received);
  EXPECT_NEAR(outbound_rate, 0.05, 0.02) << "the outgoing loss was not what was asked for";
  EXPECT_NEAR(inbound_rate, 0.05, 0.02) << "the incoming loss was not what was asked for";

  // Then, that the call is still a call.
  EXPECT_EQ(ana().session().state(), CallSession::State::InCall) << "the call dropped";
  EXPECT_EQ(bruno().session().state(), CallSession::State::InCall) << "the call dropped";
  EXPECT_EQ(ana().errors(), 0U);
  EXPECT_EQ(bruno().errors(), 0U);

  // And that audio kept moving throughout, rather than stopping and being
  // counted as survived because nobody hung up. Fifteen seconds at 50 packets
  // a second is 750; asking for 300 leaves room for a slow machine without
  // letting a stalled stream through.
  EXPECT_GT(stats.packets_received - received_before, 300U)
      << "the connection stayed up but the audio stopped";
  EXPECT_GT(server_->media_router()->audio_packets_forwarded() - forwarded_before, 300U);

  // The loss is visible where it should be, in the receiver's own accounting.
  // With retransmission on, the gap a lost packet leaves is filled again
  // before the receiver report counts it, so packets_lost sits at zero on a
  // link that is demonstrably losing 5%. What remains of the loss at this
  // level is the request it caused.
  EXPECT_GT(stats.packets_lost + after.stats.nacks_sent, 0U)
      << "libwebrtc never noticed the loss the injector caused";

  // Degradation, not collapse: the loss the receiver sees is of the order of
  // what was injected, not a multiple of it. Opus carries one frame per packet,
  // so 5% of packets lost is 5% of the audio, which is what "smooth" means
  // here.
  const double observed = static_cast<double>(stats.packets_lost) /
                          static_cast<double>(stats.packets_received + stats.packets_lost);
  EXPECT_LT(observed, 0.15) << "the receiver lost far more than was injected, which means "
                               "something downstream amplified the loss";

  // The repair. packets_lost above counts every packet the injector took,
  // repaired or not; what the listener heard is the concealment, and with the
  // previous frame riding in every packet an isolated loss leaves none.
  print_concealment(before, after);
  EXPECT_GT(server_->media_router()->audio_red_packets_received() - red_before, 0U)
      << "no RED reached the SFU, so whatever was measured above was measured without "
         "the redundancy";
  EXPECT_LT(concealment_events_per_second(before, after), kConcealmentWithRepairPerSecond)
      << "the repairs are negotiated and the listener still hears the loss";

  // The retransmission, on both legs. The SFU asks the sender for what did
  // not arrive and the sender answers; the listener asks the SFU and the SFU
  // answers out of its cache. Each half leaves a count on each side.
  const auto repair = server_->media_router()->audio_repair_stats();
  const media::AudioStats sender = ana().last_stats();
  std::printf(
      "retransmission   the SFU asked %llu times and %llu of %llu came back; ana sent "
      "%llu packets again; bruno asked %llu times, the SFU heard %llu\n",
      static_cast<unsigned long long>(repair.requests_sent),
      static_cast<unsigned long long>(repair.packets_repaired),
      static_cast<unsigned long long>(repair.packets_missing),
      static_cast<unsigned long long>(sender.retransmitted_packets_sent),
      static_cast<unsigned long long>(after.stats.nacks_sent),
      static_cast<unsigned long long>(server_->media_router()->audio_nacks_received()));
  std::fflush(stdout);
  EXPECT_GT(repair.requests_sent, 0U) << "the SFU never asked the sender for a lost audio packet";
  EXPECT_GT(repair.packets_repaired, 0U) << "the SFU asked and nothing ever came back";
  EXPECT_GT(sender.nacks_received, 0U) << "no request from the SFU reached the sender";
  EXPECT_GT(sender.retransmitted_packets_sent, 0U) << "the sender never sent a packet again";
  EXPECT_GT(after.stats.nacks_sent, 0U) << "the listener never asked the SFU for a lost packet";
  EXPECT_GT(server_->media_router()->audio_nacks_received(), 0U)
      << "no request from the listener reached the SFU";
}

TEST_F(ImpairedNetworkWithoutRepairTest, FivePercentLossIsHeardWithoutRepair) {
  // The instrument, calibrated. With Opus alone every lost packet is a hole
  // the jitter buffer has to paper over, and this is how many per second the
  // link above produces. It is the number the test above is measured against:
  // if this ever stops being audible, that test proves nothing.
  start_a_call();

  const Report before = fresh_report(bruno());
  dv::client::media::reset_network_impairment_counters();
  dv::client::media::set_network_impairment({.loss = 0.05});
  std::this_thread::sleep_for(kUnderImpairment);
  const Report after = fresh_report(bruno());

  std::printf("\n--- the same link, Opus alone ---\n");
  print_concealment(before, after);

  EXPECT_EQ(server_->media_router()->audio_red_packets_received(), 0U)
      << "RED arrived with the redundancy turned off, so the key does not turn it off";
  EXPECT_EQ(server_->media_router()->audio_repair_stats().requests_sent, 0U)
      << "the SFU asked for a packet with the retransmission turned off";
  EXPECT_EQ(server_->media_router()->audio_nacks_received(), 0U)
      << "a listener asked for a packet with the retransmission turned off, so the key does "
         "not reach the offer";
  EXPECT_GT(concealment_events_per_second(before, after), kConcealmentWithoutRepairPerSecond)
      << "5% loss left no audible mark without any repair, so the redundancy test cannot "
         "tell a repair from nothing";
}

TEST_F(ImpairedNetworkTest, ACallSurvivesHalfASecondOfLatencyAndJitter) {
  start_a_call();

  const std::uint64_t received_before = bruno().last_stats().packets_received;

  // 250 ms each way is half a second round trip, which is past the 150 ms
  // section 22 asks for and into the range where a conversation is awkward but
  // still a conversation. The jitter is what the jitter buffer has to absorb.
  dv::client::media::reset_network_impairment_counters();
  dv::client::media::set_network_impairment({.delay = 250ms, .jitter = 30ms});
  std::this_thread::sleep_for(kUnderImpairment);

  const auto stats = bruno().last_stats();

  std::printf("\n--- audio on a link with 250 ms of latency and 30 ms of jitter ---\n");
  std::printf("packets held     %llu\n",
              static_cast<unsigned long long>(network_impairment_counters().packets_delayed));
  std::printf("receiver         %llu packets arrived, rtt %.0f ms, jitter %.1f ms\n",
              static_cast<unsigned long long>(stats.packets_received - received_before),
              stats.round_trip_time_ms, stats.jitter_ms);
  std::fflush(stdout);

  EXPECT_GT(network_impairment_counters().packets_delayed, 500U)
      << "no packet was held back, so the latency was never applied";
  EXPECT_EQ(ana().session().state(), CallSession::State::InCall);
  EXPECT_EQ(bruno().session().state(), CallSession::State::InCall);
  EXPECT_GT(stats.packets_received - received_before, 300U);

  // The injected delay shows up in libwebrtc's own measurement, which is the
  // cross check that the injector does what it says: two 250 ms hops make a
  // round trip of about half a second, and the loopback contributes nothing.
  EXPECT_GT(stats.round_trip_time_ms, 300.0)
      << "the round trip time did not move, so the delay was not on the path being measured";
}

TEST_F(ImpairedNetworkTest, AScreenShareKeepsArrivingUnderPacketLoss) {
  if (!dv::client::video::screen_capture_is_available()) {
    GTEST_SKIP() << "no display server attached, so there is no screen to share";
  }

  start_a_call();

  ASSERT_TRUE(ana().session().start_screen_share("").ok());
  ASSERT_TRUE(wait_until([&] { return bruno().remote_frames() > 0; }, 30000ms))
      << "no frame arrived before the network was touched";

  const std::uint64_t frames_before = bruno().remote_frames();

  dv::client::media::reset_network_impairment_counters();
  dv::client::media::set_network_impairment({.loss = 0.05});
  std::this_thread::sleep_for(kUnderImpairment);

  const std::uint64_t frames_after = bruno().remote_frames();

  // Video is where loss hurts most: a lost packet costs a whole frame, and a
  // lost keyframe costs every frame until the next one. What has to hold is
  // that the picture keeps coming, not that it comes at full rate.
  auto* router = server_->media_router();
  const auto repair = router->video_repair_stats();

  std::printf("\n--- a 1280x720 screen share on a link losing 5%% of packets ---\n");
  std::printf("frames           %llu in %llu s\n",
              static_cast<unsigned long long>(frames_after - frames_before),
              static_cast<unsigned long long>(kUnderImpairment.count() / 1000));
  std::printf("SFU              %llu video packets forwarded, %llu keyframe requests carried\n",
              static_cast<unsigned long long>(router->video_packets_forwarded()),
              static_cast<unsigned long long>(router->keyframe_requests_forwarded()));
  std::printf("repair           %llu requests, %llu packets missing, %llu recovered\n",
              static_cast<unsigned long long>(repair.requests_sent),
              static_cast<unsigned long long>(repair.packets_missing),
              static_cast<unsigned long long>(repair.packets_repaired));
  std::fflush(stdout);

  EXPECT_GT(frames_after - frames_before, 30U)
      << "the shared screen froze under loss: " << (frames_after - frames_before)
      << " frames in fifteen seconds, with " << router->video_packets_forwarded()
      << " video packets forwarded and " << router->keyframe_requests_forwarded()
      << " keyframe requests carried to the sharer";
  EXPECT_TRUE(ana().session().sharing_screen());
  EXPECT_EQ(bruno().session().state(), CallSession::State::InCall);
}

TEST_F(ImpairedNetworkTest, TheSenderIsSlowedDownWhenTheLinkStartsLosingPackets) {
  if (!dv::client::video::screen_capture_is_available()) {
    GTEST_SKIP() << "no display server attached, so there is no screen to share";
  }

  start_a_call();
  ASSERT_TRUE(ana().session().start_screen_share("").ok());
  ASSERT_TRUE(wait_until([&] { return bruno().remote_frames() > 0; }, 30000ms));

  auto* router = server_->media_router();
  const auto estimate = [&] { return ana().session().video_stats().available_send_bitrate_kbps; };

  // A healthy link, probed upwards until it stops growing. This is already
  // half the point: without the SFU telling it anything, the sender sits at
  // whatever it started with.
  ASSERT_TRUE(wait_until([&] { return estimate() > 2500; }, 40000ms))
      << "the estimate never grew on a clean link, it sat at " << estimate() << " kbps";
  const double healthy = estimate();

  dv::client::media::set_network_impairment({.loss = 0.20});
  // Twenty percent loss takes a tenth off the target every second, so a third
  // off takes about four seconds. Twenty is patient without being a wait for
  // convergence.
  const bool fell = wait_until([&] { return estimate() < healthy * 0.66; }, 20000ms);
  const double squeezed = estimate();
  const int asked_for = router->video_repair_stats().target_kbps;

  dv::client::media::set_network_impairment({});
  const bool recovered = wait_until([&] { return estimate() > squeezed * 1.3; }, 20000ms);

  std::printf("\n--- what the SFU asks a screen share to aim for ---\n");
  std::printf("clean link       %.0f kbps\n", healthy);
  std::printf("under 20%% loss   %.0f kbps, the SFU asking for %d\n", squeezed, asked_for);
  std::printf("after recovery   %.0f kbps\n", estimate());
  std::fflush(stdout);

  EXPECT_TRUE(fell) << "the link started losing a fifth of its packets and the sender kept "
                       "sending the same amount: "
                    << squeezed << " kbps against " << healthy;
  // The two numbers are the two ends of the same loop: the SFU decides, REMB
  // carries it, and libwebrtc's congestion controller obeys. If they disagree
  // the loop is broken somewhere in between.
  EXPECT_NEAR(squeezed, asked_for, healthy * 0.15)
      << "the sender is not aiming at what the SFU asked for";
  EXPECT_GT(router->video_repair_stats().target_kbps, 0);

  EXPECT_TRUE(recovered) << "the loss stopped and the sender stayed squeezed at " << estimate()
                         << " kbps";
  EXPECT_EQ(ana().session().state(), CallSession::State::InCall);
}

}  // namespace
