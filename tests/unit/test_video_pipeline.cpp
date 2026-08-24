// The parts of the screen share pipeline that have no platform in them.
//
// Frame sizing and the queue that sits between capture and encoding are pure
// logic, so they are tested here rather than in the media suite: no libwebrtc,
// no monitor, no sound card, and they run in milliseconds.

#include <cstdint>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "video/frame_queue.hpp"
#include "video/frame_size.hpp"
#include "video/screen_quality.hpp"
#include "video/video_frame.hpp"

namespace {

using dv::client::video::fit_within;
using dv::client::video::FrameQueue;
using dv::client::video::recommended_bitrate_kbps;
using dv::client::video::recommended_max_bitrate_kbps;
using dv::client::video::Size;
using dv::client::video::VideoFrame;

constexpr Size kTarget{1280, 720};

[[nodiscard]] VideoFrame make_frame(int width, int height, std::uint8_t fill = 0) {
  const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                     static_cast<std::size_t>(VideoFrame::kBytesPerPixel);
  std::vector<std::uint8_t> pixels(bytes, fill);
  return VideoFrame{Size{width, height}, std::move(pixels)};
}

TEST(FrameSizeTest, A1080pMonitorFitsExactlyInto720p) {
  EXPECT_EQ(fit_within({1920, 1080}, kTarget), (Size{1280, 720}));
}

TEST(FrameSizeTest, AnUltrawideKeepsItsShape) {
  // 3440x1440 is 21:9. Stretching it into 16:9 would make every window on the
  // shared screen taller than it is.
  const Size fitted = fit_within({3440, 1440}, kTarget);
  EXPECT_EQ(fitted.width, 1280);
  EXPECT_EQ(fitted.height, 534);
  EXPECT_LE(fitted.height, kTarget.height);
}

TEST(FrameSizeTest, ATallMonitorIsBoundedByItsHeight) {
  // A rotated monitor is taller than it is wide, so it is the height that has
  // to give way, not the width.
  const Size fitted = fit_within({1080, 1920}, kTarget);
  EXPECT_EQ(fitted.height, 720);
  EXPECT_LE(fitted.width, kTarget.width);
  EXPECT_EQ(fitted.width, 404);
}

TEST(FrameSizeTest, ASmallMonitorIsNotUpscaled) {
  EXPECT_EQ(fit_within({800, 600}, kTarget), (Size{800, 600}));
}

TEST(FrameSizeTest, EverySizeIsEvenInBothDimensions) {
  // I420 has one chroma sample per two by two block, so an odd side cannot be
  // represented and the receiver would have to crop.
  for (const Size source : {Size{1921, 1081}, Size{1365, 767}, Size{999, 333}, Size{3, 3}}) {
    const Size fitted = fit_within(source, kTarget);
    EXPECT_EQ(fitted.width % 2, 0) << source.width << "x" << source.height;
    EXPECT_EQ(fitted.height % 2, 0) << source.width << "x" << source.height;
    EXPECT_GE(fitted.width, 2);
    EXPECT_GE(fitted.height, 2);
  }
}

TEST(FrameSizeTest, AnEmptySourceProducesAnEmptySize) {
  EXPECT_TRUE(fit_within({0, 0}, kTarget).empty());
  EXPECT_TRUE(fit_within({1920, 0}, kTarget).empty());
}

TEST(ScreenQualityTest, EveryOfferedSizeIsEvenAndOrdered) {
  // Even for the same reason fit_within rounds down to even, and ordered
  // because the menu is read top to bottom and a list that jumps around reads
  // as a bug in the dialog.
  int previous = 0;
  for (const auto& row : dv::client::video::kScreenResolutions) {
    EXPECT_EQ(row.size.width % 2, 0) << row.label;
    EXPECT_EQ(row.size.height % 2, 0) << row.label;
    EXPECT_GT(row.size.height, previous) << row.label;
    previous = row.size.height;
  }
  EXPECT_FALSE(dv::client::video::kScreenFrameRates.empty());
}

TEST(ScreenQualityTest, TheDefaultQualityAsksForWhatTheSpecAllows) {
  // 1280x720 at 30 is section 5.2, and section 6 puts its ceiling at 3 Mbps.
  // Anything else here would mean the dialog complains about the defaults.
  EXPECT_EQ(recommended_max_bitrate_kbps({1280, 720}, 30), 3000);
}

TEST(ScreenQualityTest, MorePixelsAskForMore) {
  EXPECT_GT(recommended_max_bitrate_kbps({1920, 1080}, 30),
            recommended_max_bitrate_kbps({1280, 720}, 30));
}

TEST(ScreenQualityTest, DoublingTheRateCostsHalfAgainRatherThanDouble) {
  // A screen mostly does not change in 16 ms, so the second frame is nearly
  // free. Budgeting double for it would take bandwidth away from the picture.
  const int at30 = recommended_max_bitrate_kbps({1280, 720}, 30);
  const int at60 = recommended_max_bitrate_kbps({1280, 720}, 60);
  EXPECT_EQ(at60, at30 + at30 / 2);
}

TEST(ScreenQualityTest, ASlowerRateDoesNotLowerTheCeiling) {
  // It is a ceiling the encoder may use, not a target it will spend, and
  // lowering it would only take the headroom a keyframe needs.
  EXPECT_EQ(recommended_max_bitrate_kbps({1280, 720}, 15),
            recommended_max_bitrate_kbps({1280, 720}, 30));
}

TEST(ScreenQualityTest, TheRecommendationStopsWhereTheDialogStopsOffering) {
  // 1080p60 works out above 10 Mbps, which is not a number the settings dialog
  // can be set to. A hint asking for something unreachable is a hint that
  // cannot be acted on.
  const int wanted = recommended_max_bitrate_kbps({1920, 1080}, 60);
  EXPECT_EQ(wanted, dv::client::video::kMaxRecommendedBitrateKbps);
}

TEST(ScreenQualityTest, NonsenseFallsBackToTheSpecDefault) {
  EXPECT_EQ(recommended_max_bitrate_kbps({0, 0}, 30), dv::client::video::kBaseMaxBitrateKbps);
  EXPECT_EQ(recommended_max_bitrate_kbps({1280, 720}, 0), dv::client::video::kBaseMaxBitrateKbps);
}

TEST(ScreenQualityTest, TheAutomaticRangeIsTheSpecRangeAtTheDefaultQuality) {
  // Section 6 of SPEC.md puts 720p30 between 1.5 and 3 Mbps. Automatic mode
  // arriving anywhere else at the default quality would mean the mode and the
  // defaults disagree about the one case both of them name.
  const auto range = recommended_bitrate_kbps({1280, 720}, 30, 300);
  EXPECT_EQ(range.min_kbps, 1500);
  EXPECT_EQ(range.max_kbps, 3000);
}

TEST(ScreenQualityTest, TheAutomaticCeilingIsTheRecommendation) {
  for (const int fps : {30, 60}) {
    for (const Size size : {Size{1280, 720}, Size{1920, 1080}}) {
      EXPECT_EQ(recommended_bitrate_kbps(size, fps, 300).max_kbps,
                recommended_max_bitrate_kbps(size, fps))
          << size.width << "x" << size.height << " at " << fps;
    }
  }
}

TEST(ScreenQualityTest, TheAutomaticStartNeverSitsBelowTheFloor) {
  // dv::config::validate refuses a floor above the minimum, and this is the
  // one place that can produce that pair without anybody typing a number.
  const auto range = recommended_bitrate_kbps({1280, 720}, 30, 2000);
  EXPECT_GE(range.min_kbps, 2000);
  EXPECT_LE(range.min_kbps, range.max_kbps);
}

TEST(ScreenQualityTest, AFloorAboveTheRecommendationLiftsTheCeilingToMeetIt) {
  // Strange, and legal: a floor is read from the configuration while the
  // ceiling is computed. Returning a maximum under its minimum would be a
  // range no caller can use, so the ceiling gives way.
  const auto range = recommended_bitrate_kbps({1280, 720}, 30, 20000);
  EXPECT_GE(range.max_kbps, 20000);
  EXPECT_LE(range.min_kbps, range.max_kbps);
}

TEST(ScreenQualityTest, TheAutomaticRangeIsOrderedForEveryQualityOffered) {
  for (const dv::client::video::ScreenResolution& row : dv::client::video::kScreenResolutions) {
    for (const int fps : dv::client::video::kScreenFrameRates) {
      const auto range = recommended_bitrate_kbps(row.size, fps, 300);
      EXPECT_GE(range.min_kbps, 300) << row.label << " at " << fps;
      EXPECT_LE(range.min_kbps, range.max_kbps) << row.label << " at " << fps;
    }
  }
}

TEST(FrameQueueTest, FramesComeOutInTheOrderTheyWentIn) {
  FrameQueue queue{4};
  EXPECT_TRUE(queue.push(make_frame(2, 2, 1)));
  EXPECT_TRUE(queue.push(make_frame(2, 2, 2)));

  auto first = queue.pop();
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(*first->data(), 1);

  auto second = queue.pop();
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(*second->data(), 2);

  EXPECT_FALSE(queue.pop().has_value());
}

TEST(FrameQueueTest, TheOldestFrameIsTheOneDiscarded) {
  // The reason the queue exists. A screen share that falls behind has to show
  // the present late, not the past on time.
  FrameQueue queue{2};
  EXPECT_TRUE(queue.push(make_frame(2, 2, 1)));
  EXPECT_TRUE(queue.push(make_frame(2, 2, 2)));
  EXPECT_FALSE(queue.push(make_frame(2, 2, 3))) << "the queue was full and said nothing was lost";

  EXPECT_EQ(queue.size(), 2U);
  EXPECT_EQ(queue.dropped(), 1U);

  auto oldest = queue.pop();
  ASSERT_TRUE(oldest.has_value());
  EXPECT_EQ(*oldest->data(), 2) << "the frame discarded was not the oldest one";
}

TEST(FrameQueueTest, PoppingNeverBlocksOnAnEmptyQueue) {
  FrameQueue queue;
  EXPECT_FALSE(queue.pop().has_value());
  EXPECT_EQ(queue.size(), 0U);
  EXPECT_EQ(queue.dropped(), 0U);
}

TEST(FrameQueueTest, ACapacityOfZeroStillHoldsOneFrame) {
  // Asked for a queue that cannot hold anything, the sensible reading is the
  // smallest queue that works rather than one that discards everything.
  FrameQueue queue{0};
  EXPECT_EQ(queue.capacity(), 1U);
  EXPECT_TRUE(queue.push(make_frame(2, 2, 7)));
  EXPECT_EQ(queue.size(), 1U);
}

TEST(FrameQueueTest, ClearingLeavesTheDropCountAlone) {
  FrameQueue queue{1};
  EXPECT_TRUE(queue.push(make_frame(2, 2)));
  EXPECT_FALSE(queue.push(make_frame(2, 2)));
  queue.clear();

  EXPECT_EQ(queue.size(), 0U);
  EXPECT_EQ(queue.dropped(), 1U) << "clearing hid what had already been lost";
}

TEST(FrameQueueTest, ItSurvivesAProducerAndAConsumerAtOnce) {
  // The queue's whole job is to sit between two threads, so the case worth
  // testing is exactly that. Under a sanitizer this is also the data race
  // check.
  FrameQueue queue{2};
  constexpr int kFrames = 2000;

  std::thread producer([&] {
    for (int i = 0; i < kFrames; ++i) {
      (void)queue.push(make_frame(4, 4, static_cast<std::uint8_t>(i)));
    }
  });

  int consumed = 0;
  std::thread consumer([&] {
    for (int i = 0; i < kFrames; ++i) {
      if (queue.pop().has_value()) {
        ++consumed;
      }
    }
  });

  producer.join();
  consumer.join();

  // Nothing is asserted about how many got through, because that depends on
  // how the two threads interleave. What has to hold is that every frame is
  // accounted for: consumed, still queued, or counted as dropped.
  EXPECT_EQ(consumed + static_cast<int>(queue.size()) + static_cast<int>(queue.dropped()), kFrames);
}

TEST(VideoFrameTest, TakingThePixelsLeavesTheFrameEmpty) {
  // The buffer is handed back so the capturer can fill it again instead of
  // allocating three and a half megabytes thirty times a second.
  VideoFrame frame = make_frame(4, 4, 9);
  const std::size_t bytes = frame.byte_count();

  std::vector<std::uint8_t> pixels = frame.take_pixels();

  EXPECT_EQ(pixels.size(), bytes);
  EXPECT_TRUE(frame.empty());
  EXPECT_EQ(frame.width(), 0);
  EXPECT_EQ(frame.height(), 0);
}

TEST(VideoFrameTest, ItCarriesASizeRatherThanTwoLooseNumbers) {
  // Width and height are the same type and mean different things. Handing them
  // over as a Size is what stops the pair being swapped at a call site, which
  // produces a frame that decodes into diagonal stripes.
  const VideoFrame frame = make_frame(640, 480);
  EXPECT_EQ(frame.size(), (Size{640, 480}));
  EXPECT_EQ(frame.width(), 640);
  EXPECT_EQ(frame.height(), 480);
}

TEST(VideoFrameTest, StrideIsFourBytesPerPixel) {
  const VideoFrame frame = make_frame(1280, 720);
  EXPECT_EQ(frame.stride(), 1280 * 4);
  EXPECT_EQ(frame.byte_count(), static_cast<std::size_t>(1280) * 720 * 4);
}

}  // namespace
