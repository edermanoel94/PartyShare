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
#include "video/video_frame.hpp"

namespace {

using dv::client::video::fit_within;
using dv::client::video::FrameQueue;
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
