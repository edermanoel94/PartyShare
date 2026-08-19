// Screen capture against the real system, section 7 of SPEC.md.
//
// Enumeration on its own proves very little: there are backends that list
// monitors and then never produce a pixel. What is asserted here is frames,
// their size, and the rate they arrive at.
//
// Skipped where there is no display server, which is the normal state of a CI
// runner. The pieces that can be tested without one, the frame sizing and the
// queue, are unit tests in tests/unit/test_video_pipeline.cpp instead.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "video/screen_capturer.hpp"

namespace {

using namespace std::chrono_literals;
using dv::client::video::create_screen_capturer;
using dv::client::video::fit_within;
using dv::client::video::Monitor;
using dv::client::video::monitors;
using dv::client::video::screen_capture_is_available;
using dv::client::video::ScreenCaptureOptions;
using dv::client::video::ScreenCapturer;
using dv::client::video::Size;
using dv::client::video::VideoFrame;

/// Collects what the capturer produces, from the capture thread.
class Recorder {
 public:
  [[nodiscard]] ScreenCapturer::FrameSink sink() {
    return [this](VideoFrame frame) {
      const std::lock_guard<std::mutex> lock(mutex_);
      ++count_;
      last_size_ = Size{frame.width(), frame.height()};
      last_bytes_ = frame.byte_count();
    };
  }

  [[nodiscard]] ScreenCapturer::ErrorSink errors() {
    return [this](dv::Error error) {
      const std::lock_guard<std::mutex> lock(mutex_);
      error_ = std::move(error);
    };
  }

  [[nodiscard]] std::uint64_t count() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return count_;
  }
  [[nodiscard]] Size last_size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_size_;
  }
  [[nodiscard]] std::size_t last_bytes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_bytes_;
  }
  [[nodiscard]] dv::Error error() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return error_;
  }

 private:
  mutable std::mutex mutex_;
  std::uint64_t count_ = 0;
  Size last_size_;
  std::size_t last_bytes_ = 0;
  dv::Error error_;
};

class ScreenCaptureTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (!screen_capture_is_available()) {
      GTEST_SKIP() << "no display server attached, so there is no screen to capture";
    }
  }

  // Owned by the fixture so it outlives the capturer, which calls back from
  // its own thread right up until it is joined.
  Recorder recorder_;
  std::unique_ptr<ScreenCapturer> capturer_;

  void TearDown() override {
    if (capturer_) {
      capturer_->stop();
      capturer_.reset();
    }
  }
};

TEST_F(ScreenCaptureTest, TheSystemMonitorsCanBeListed) {
  const auto listed = monitors();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  ASSERT_FALSE(listed.value().empty()) << "a display server is attached but reports no monitor";

  for (const Monitor& monitor : listed.value()) {
    EXPECT_FALSE(monitor.id.empty());
    EXPECT_FALSE(monitor.name.empty()) << "a monitor with no name has nothing to show in a menu";
  }
  EXPECT_TRUE(listed.value().front().is_primary);
}

TEST_F(ScreenCaptureTest, CapturingTheDefaultMonitorProducesFrames) {
  auto created =
      create_screen_capturer(ScreenCaptureOptions{}, recorder_.sink(), recorder_.errors());
  ASSERT_TRUE(created.ok()) << created.error().message;
  capturer_ = std::move(created).take();

  const auto started = capturer_->start("");
  ASSERT_TRUE(started.ok()) << started.error().message;
  EXPECT_TRUE(capturer_->capturing());

  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (recorder_.count() < 5 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(50ms);
  }

  ASSERT_GE(recorder_.count(), 5U) << "no frames from the capturer: " << recorder_.error().message;

  // Section 5.2 of SPEC.md: 1280x720. Whatever the monitor is, what leaves the
  // capturer fits inside that box.
  const Size size = recorder_.last_size();
  EXPECT_GT(size.width, 0);
  EXPECT_GT(size.height, 0);
  EXPECT_LE(size.width, 1280);
  EXPECT_LE(size.height, 720);
  EXPECT_EQ(size.width % 2, 0);
  EXPECT_EQ(size.height % 2, 0);

  // And the buffer really holds that many pixels, rather than a size field
  // that disagrees with its contents.
  EXPECT_EQ(recorder_.last_bytes(), static_cast<std::size_t>(size.width) *
                                        static_cast<std::size_t>(size.height) *
                                        VideoFrame::kBytesPerPixel);
}

TEST_F(ScreenCaptureTest, TheFrameRateStaysUnderTheCap) {
  ScreenCaptureOptions options;
  options.max_fps = 10;

  auto created = create_screen_capturer(options, recorder_.sink(), recorder_.errors());
  ASSERT_TRUE(created.ok()) << created.error().message;
  capturer_ = std::move(created).take();
  ASSERT_TRUE(capturer_->start("").ok());

  // Wait for the first frame before timing, so the portal negotiation and the
  // first allocation do not count against the rate.
  const auto ready = std::chrono::steady_clock::now() + 10s;
  while (recorder_.count() == 0 && std::chrono::steady_clock::now() < ready) {
    std::this_thread::sleep_for(20ms);
  }
  ASSERT_GT(recorder_.count(), 0U) << recorder_.error().message;

  const std::uint64_t before = recorder_.count();
  const auto started_at = std::chrono::steady_clock::now();
  std::this_thread::sleep_for(2s);
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
  const double measured = static_cast<double>(recorder_.count() - before) / seconds;

  // A cap that is not enforced is the failure this guards against: without the
  // sleep in the capture loop this runs at whatever the machine can manage,
  // which is hundreds of frames a second on an idle screen.
  EXPECT_LE(measured, options.max_fps + 2) << "captured at " << measured << " fps";
  EXPECT_GE(measured, 1.0) << "captured at " << measured << " fps, the loop is stalling";
}

TEST_F(ScreenCaptureTest, StoppingEndsTheFrames) {
  auto created =
      create_screen_capturer(ScreenCaptureOptions{}, recorder_.sink(), recorder_.errors());
  ASSERT_TRUE(created.ok()) << created.error().message;
  capturer_ = std::move(created).take();
  ASSERT_TRUE(capturer_->start("").ok());

  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (recorder_.count() == 0 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
  }
  ASSERT_GT(recorder_.count(), 0U) << recorder_.error().message;

  capturer_->stop();
  EXPECT_FALSE(capturer_->capturing());

  const std::uint64_t after_stop = recorder_.count();
  std::this_thread::sleep_for(500ms);
  EXPECT_EQ(recorder_.count(), after_stop) << "frames kept arriving after stop returned";
}

TEST_F(ScreenCaptureTest, StartingTwiceRestartsRatherThanRunningTwo) {
  auto created =
      create_screen_capturer(ScreenCaptureOptions{}, recorder_.sink(), recorder_.errors());
  ASSERT_TRUE(created.ok()) << created.error().message;
  capturer_ = std::move(created).take();

  ASSERT_TRUE(capturer_->start("").ok());
  ASSERT_TRUE(capturer_->start("").ok()) << "the second start was refused";
  EXPECT_TRUE(capturer_->capturing());

  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (recorder_.count() < 3 && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(20ms);
  }
  EXPECT_GE(recorder_.count(), 3U) << "restarting left the capture dead";
}

TEST_F(ScreenCaptureTest, AMonitorThatIsNotAMonitorIsRefused) {
  auto created =
      create_screen_capturer(ScreenCaptureOptions{}, recorder_.sink(), recorder_.errors());
  ASSERT_TRUE(created.ok()) << created.error().message;
  capturer_ = std::move(created).take();

  const auto started = capturer_->start("not-a-number");
  ASSERT_FALSE(started.ok());
  EXPECT_EQ(started.error().code, "monitor_not_found");
  EXPECT_FALSE(capturer_->capturing());
}

TEST_F(ScreenCaptureTest, EveryListedMonitorCanBeCaptured) {
  const auto listed = monitors();
  ASSERT_TRUE(listed.ok()) << listed.error().message;
  ASSERT_FALSE(listed.value().empty());

  for (const Monitor& monitor : listed.value()) {
    Recorder recorder;
    auto created =
        create_screen_capturer(ScreenCaptureOptions{}, recorder.sink(), recorder.errors());
    ASSERT_TRUE(created.ok()) << created.error().message;
    std::unique_ptr<ScreenCapturer> capturer = std::move(created).take();

    const auto started = capturer->start(monitor.id);
    EXPECT_TRUE(started.ok()) << monitor.name << ": " << started.error().message;

    const auto deadline = std::chrono::steady_clock::now() + 10s;
    while (recorder.count() == 0 && std::chrono::steady_clock::now() < deadline) {
      std::this_thread::sleep_for(20ms);
    }
    EXPECT_GT(recorder.count(), 0U)
        << monitor.name << " is listed but produced nothing: " << recorder.error().message;

    capturer->stop();
  }
}

}  // namespace
