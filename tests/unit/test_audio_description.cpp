// The audio m-lines the SFU offers.
//
// This is what decides what every participant encodes and accepts, and one
// wrong character makes libwebrtc drop RED without a word: the redundancy stops
// existing and every other test still passes. So the shape of the offer is
// pinned here, away from any network.
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <rtc/rtc.hpp>

#include "sfu/audio_description.hpp"

namespace {

using dv::server::sfu::add_audio_codecs;
using dv::server::sfu::AudioCodecs;
using dv::server::sfu::opus_profile;

TEST(AudioDescription, RedComesBeforeOpusAndNamesIt) {
  rtc::Description::Audio media("0", rtc::Description::Direction::RecvOnly);
  add_audio_codecs(media, AudioCodecs{});

  // libwebrtc pairs RED with the codec it names only when RED is listed first.
  EXPECT_EQ(media.payloadTypes(), (std::vector<int>{63, 111}));

  const auto* red = media.rtpMap(63);
  ASSERT_NE(red, nullptr);
  EXPECT_EQ(red->format, "red");
  EXPECT_EQ(red->clockRate, 48000);
  EXPECT_EQ(red->encParams, "2");
  EXPECT_EQ(red->fmtps, (std::vector<std::string>{"111/111"}));

  const auto* opus = media.rtpMap(111);
  ASSERT_NE(opus, nullptr);
  EXPECT_EQ(opus->clockRate, 48000);
  EXPECT_EQ(opus->encParams, "2");
}

TEST(AudioDescription, TheOfferSpellsRedTheWayLibwebrtcReadsIt) {
  rtc::Description::Audio media("0", rtc::Description::Direction::SendOnly);
  add_audio_codecs(media, AudioCodecs{});
  const std::string sdp = media.generateSdp("\n", "0.0.0.0", 9);
  EXPECT_NE(sdp.find("a=rtpmap:63 red/48000/2\n"), std::string::npos) << sdp;
  EXPECT_NE(sdp.find("a=fmtp:63 111/111\n"), std::string::npos) << sdp;
  // On the m= line itself, in that order.
  EXPECT_NE(sdp.find(" 63 111\n"), std::string::npos) << sdp;
}

TEST(AudioDescription, InBandFecIsOffWhileRedIsOn) {
  // Opus in-band FEC only exists in SILK, and reported loss would push the
  // encoder there and cap the audio at 8 kHz. RED covers the same loss without
  // that price, so the flag goes when RED comes.
  rtc::Description::Audio media("0", rtc::Description::Direction::RecvOnly);
  add_audio_codecs(media, AudioCodecs{});
  const auto* opus = media.rtpMap(111);
  ASSERT_NE(opus, nullptr);
  ASSERT_EQ(opus->fmtps.size(), 1U);
  EXPECT_EQ(opus->fmtps.front(), "minptime=10;maxaveragebitrate=128000;stereo=1;sprop-stereo=1");
}

TEST(AudioDescription, WithoutRedTheOfferIsWhatItAlwaysWas) {
  // The key to go back. Opus alone, with the in-band FEC the offer carried
  // before RED existed.
  rtc::Description::Audio media("0", rtc::Description::Direction::RecvOnly);
  add_audio_codecs(media, AudioCodecs{.red_payload_type = std::nullopt});
  EXPECT_EQ(media.payloadTypes(), (std::vector<int>{111}));
  const auto* opus = media.rtpMap(111);
  ASSERT_NE(opus, nullptr);
  ASSERT_EQ(opus->fmtps.size(), 1U);
  EXPECT_EQ(opus->fmtps.front(),
            "minptime=10;maxaveragebitrate=128000;stereo=1;sprop-stereo=1;useinbandfec=1");
}

TEST(AudioDescription, RetransmissionIsAskedForOnOpusAndOnlyOpus) {
  // libwebrtc reads the feedback off the codec it encodes with. On RED it
  // would be ignored; on Opus it turns on both the send history and the
  // jitter buffer's requests, even though the answer never echoes it.
  rtc::Description::Audio media("0", rtc::Description::Direction::RecvOnly);
  add_audio_codecs(media, AudioCodecs{});
  const auto* opus = media.rtpMap(111);
  ASSERT_NE(opus, nullptr);
  EXPECT_EQ(opus->rtcpFbs, (std::vector<std::string>{"nack"}));
  const auto* red = media.rtpMap(63);
  ASSERT_NE(red, nullptr);
  EXPECT_TRUE(red->rtcpFbs.empty());

  const std::string sdp = media.generateSdp("\n", "0.0.0.0", 9);
  EXPECT_NE(sdp.find("a=rtcp-fb:111 nack\n"), std::string::npos) << sdp;
}

TEST(AudioDescription, RetransmissionCanBeTurnedOff) {
  // The key to go back: the offer as it was before, with nothing asked for.
  rtc::Description::Audio media("0", rtc::Description::Direction::RecvOnly);
  add_audio_codecs(media, AudioCodecs{.nack = false});
  const auto* opus = media.rtpMap(111);
  ASSERT_NE(opus, nullptr);
  EXPECT_TRUE(opus->rtcpFbs.empty());
}

TEST(AudioDescription, TheCeilingReachesTheProfile) {
  EXPECT_EQ(opus_profile(128, false),
            "minptime=10;maxaveragebitrate=128000;stereo=1;sprop-stereo=1");
  EXPECT_EQ(opus_profile(48, true),
            "minptime=10;maxaveragebitrate=48000;stereo=1;sprop-stereo=1;useinbandfec=1");
}

}  // namespace
