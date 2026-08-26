// What one viewer's outbound video track looks like as the screen share
// changes hands. The end to end case is in integration/test_sfu.cpp; these are
// the parts of it that are hard to arrange over a real socket: wraparound, an
// encoder that restarts under the same name, and packets that arrive out of
// order.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "sfu/video_stitcher.hpp"

namespace {

using dv::server::sfu::VideoStitcher;

constexpr std::uint32_t kTicksPerFrame = 3000;  // 90 kHz at 30 FPS

/// One participant's encoder, from wherever it happened to start.
class Encoder {
 public:
  Encoder(std::string user_id, std::uint16_t sequence, std::uint32_t timestamp)
      : user_id_(std::move(user_id)), sequence_(sequence), timestamp_(timestamp) {}

  VideoStitcher::Rewritten send(VideoStitcher& stitcher) {
    const VideoStitcher::Rewritten out = stitcher.rewrite(user_id_, sequence_, timestamp_);
    ++sequence_;
    timestamp_ += kTicksPerFrame;
    return out;
  }

  /// Skips `count` packets without sending them, the way a lossy link does.
  void lose(int count) {
    sequence_ = static_cast<std::uint16_t>(sequence_ + count);
    timestamp_ += static_cast<std::uint32_t>(count) * kTicksPerFrame;
  }

 private:
  std::string user_id_;
  std::uint16_t sequence_;
  std::uint32_t timestamp_;
};

/// Asserts that a viewer reading these in this order never has to go backwards.
void expect_one_stream(const std::vector<VideoStitcher::Rewritten>& sent) {
  ASSERT_GE(sent.size(), 2U);
  for (std::size_t i = 1; i < sent.size(); ++i) {
    const auto step = static_cast<std::int16_t>(sent[i].sequence - sent[i - 1].sequence);
    EXPECT_GT(step, 0) << "packet " << i << " went backwards in the sequence space";
    EXPECT_GE(sent[i].timestamp, sent[i - 1].timestamp)
        << "packet " << i << " went backwards in time";
  }
}

TEST(VideoStitcher, ASingleSharerIsForwardedUntouched) {
  // Nobody hands anything over, so nothing may be rewritten: a call with one
  // sharer has to be exactly what it was before the stitcher existed.
  VideoStitcher stitcher;
  Encoder ana("ana", 40000, 900000);

  for (int i = 0; i < 50; ++i) {
    const std::uint16_t expected_sequence = static_cast<std::uint16_t>(40000 + i);
    const std::uint32_t expected_timestamp =
        900000 + static_cast<std::uint32_t>(i) * kTicksPerFrame;
    const VideoStitcher::Rewritten out = ana.send(stitcher);
    EXPECT_EQ(out.sequence, expected_sequence);
    EXPECT_EQ(out.timestamp, expected_timestamp);
    EXPECT_FALSE(out.rebased);
  }
  EXPECT_EQ(stitcher.source(), "ana");
}

TEST(VideoStitcher, TheSecondSharerContinuesWhereTheFirstStopped) {
  VideoStitcher stitcher;
  Encoder ana("ana", 40000, 900000);
  Encoder bruno("bruno", 100, 5000);

  std::vector<VideoStitcher::Rewritten> sent;
  for (int i = 0; i < 30; ++i) {
    sent.push_back(ana.send(stitcher));
  }
  const VideoStitcher::Rewritten last_of_ana = sent.back();

  const VideoStitcher::Rewritten first_of_bruno = bruno.send(stitcher);
  EXPECT_TRUE(first_of_bruno.rebased);
  EXPECT_EQ(first_of_bruno.sequence, static_cast<std::uint16_t>(last_of_ana.sequence + 1));
  EXPECT_EQ(first_of_bruno.timestamp, last_of_ana.timestamp + kTicksPerFrame);
  sent.push_back(first_of_bruno);

  for (int i = 0; i < 30; ++i) {
    const VideoStitcher::Rewritten out = bruno.send(stitcher);
    EXPECT_FALSE(out.rebased) << "the series was restarted in the middle of a share";
    sent.push_back(out);
  }

  expect_one_stream(sent);
  EXPECT_EQ(stitcher.source(), "bruno");
}

TEST(VideoStitcher, LossIsPassedThroughRatherThanPaperedOver) {
  // A hole in the sequence numbers is how the viewer learns it missed
  // something, and how rtc::RtcpNackResponder is asked for it again.
  // Renumbering densely would hide every loss on the call.
  VideoStitcher stitcher;
  Encoder ana("ana", 1000, 0);

  const VideoStitcher::Rewritten before = ana.send(stitcher);
  ana.lose(4);
  const VideoStitcher::Rewritten after = ana.send(stitcher);

  EXPECT_EQ(static_cast<std::uint16_t>(after.sequence - before.sequence), 5U)
      << "the gap the loss left was filled in, so the viewer will never ask for those packets";
}

TEST(VideoStitcher, TheSameParticipantSharingAgainIsANewStream) {
  // Stopping and starting a share is a new encoder, and a new encoder picks a
  // new starting point. Under one SSRC and one user id that breaks in exactly
  // the way two different sharers do.
  VideoStitcher stitcher;
  Encoder first("ana", 40000, 900000);

  std::vector<VideoStitcher::Rewritten> sent;
  for (int i = 0; i < 20; ++i) {
    sent.push_back(first.send(stitcher));
  }

  Encoder again("ana", 7, 12);
  const VideoStitcher::Rewritten resumed = again.send(stitcher);
  EXPECT_TRUE(resumed.rebased);
  sent.push_back(resumed);
  for (int i = 0; i < 20; ++i) {
    sent.push_back(again.send(stitcher));
  }

  expect_one_stream(sent);
}

TEST(VideoStitcher, AnOutOfOrderPacketKeepsItsPlaceAndDoesNotRestartTheSeries) {
  // The viewer has to be able to put it back where it belongs, and a
  // retransmission has to answer the NACK that asked for it. Neither works if
  // a late packet is renumbered to the end or treated as a new stream.
  VideoStitcher stitcher;
  const std::string ana = "ana";

  const VideoStitcher::Rewritten first = stitcher.rewrite(ana, 500, 0);
  const VideoStitcher::Rewritten third = stitcher.rewrite(ana, 502, 2 * kTicksPerFrame);
  const VideoStitcher::Rewritten second = stitcher.rewrite(ana, 501, kTicksPerFrame);

  EXPECT_FALSE(second.rebased);
  EXPECT_EQ(static_cast<std::uint16_t>(second.sequence - first.sequence), 1U);
  EXPECT_EQ(static_cast<std::uint16_t>(third.sequence - second.sequence), 1U);

  // And the handover after it still starts past the furthest packet sent, not
  // past the late one.
  Encoder bruno("bruno", 9000, 77);
  const VideoStitcher::Rewritten handover = bruno.send(stitcher);
  EXPECT_EQ(handover.sequence, static_cast<std::uint16_t>(third.sequence + 1));
}

TEST(VideoStitcher, TheSequenceSpaceWrapsWithoutRestartingTheSeries) {
  // 65535 is followed by 0, and that is not a discontinuity.
  VideoStitcher stitcher;
  Encoder ana("ana", 65530, 0);

  std::vector<VideoStitcher::Rewritten> sent;
  for (int i = 0; i < 20; ++i) {
    const VideoStitcher::Rewritten out = ana.send(stitcher);
    EXPECT_FALSE(out.rebased) << "the wraparound was read as a new stream";
    sent.push_back(out);
  }

  // Which means the numbers wrapped rather than stopping.
  EXPECT_LT(sent.back().sequence, sent.front().sequence);
  expect_one_stream(sent);
}

TEST(VideoStitcher, AHandoverAcrossTheWraparoundStaysContinuous) {
  VideoStitcher stitcher;
  Encoder ana("ana", 65534, 100);

  std::vector<VideoStitcher::Rewritten> sent;
  for (int i = 0; i < 3; ++i) {
    sent.push_back(ana.send(stitcher));
  }
  ASSERT_EQ(sent.back().sequence, 0U) << "this test needs the handover to land just past the wrap";

  Encoder bruno("bruno", 30000, 999999);
  sent.push_back(bruno.send(stitcher));
  for (int i = 0; i < 10; ++i) {
    sent.push_back(bruno.send(stitcher));
  }

  expect_one_stream(sent);
}

TEST(VideoStitcher, EachViewerGetsItsOwnSeries) {
  // Two viewers joined at different moments and have been sent different
  // amounts, so the same handover lands in a different place for each.
  VideoStitcher early;
  VideoStitcher late;
  Encoder ana("ana", 40000, 900000);
  Encoder ana_for_late("ana", 40000, 900000);

  for (int i = 0; i < 30; ++i) {
    (void)ana.send(early);
  }
  for (int i = 0; i < 3; ++i) {
    (void)ana_for_late.send(late);
  }

  Encoder bruno("bruno", 100, 5000);
  Encoder bruno_for_late("bruno", 100, 5000);
  const VideoStitcher::Rewritten to_early = bruno.send(early);
  const VideoStitcher::Rewritten to_late = bruno_for_late.send(late);

  EXPECT_NE(to_early.sequence, to_late.sequence)
      << "one viewer's position in the stream was used for another's";
}

}  // namespace
