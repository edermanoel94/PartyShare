// Screen capture against the real system, section 7 of SPEC.md.
//
// Enumeration on its own proves very little: there are backends that list
// monitors and then never produce a pixel. What is asserted here is frames,
// their size, and the rate they arrive at.
//
// Skipped where there is no display server, which is the normal state of a CI
// runner. The pieces that can be tested without one, the frame sizing and the
// queue, are unit tests in tests/unit/test_video_pipeline.cpp instead.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "video/screen_capturer.hpp"

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

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
using dv::client::video::Window;
using dv::client::video::windows;

/// One BGRA pixel, as the frames carry them.
using Pixel = std::array<std::uint8_t, VideoFrame::kBytesPerPixel>;

/// Collects what the capturer produces, from the capture thread.
class Recorder {
 public:
  [[nodiscard]] ScreenCapturer::FrameSink sink() {
    return [this](VideoFrame frame) {
      const std::lock_guard<std::mutex> lock(mutex_);
      ++count_;
      last_size_ = Size{frame.width(), frame.height()};
      last_bytes_ = frame.byte_count();
      if (std::ranges::find(sizes_, last_size_) == sizes_.end()) {
        sizes_.push_back(last_size_);
      }
      // The pixel in the middle, which for a window is the middle of its
      // content and for a screen is whatever happens to be there.
      const std::size_t offset =
          static_cast<std::size_t>(frame.height() / 2) * static_cast<std::size_t>(frame.stride()) +
          static_cast<std::size_t>(frame.width() / 2) * VideoFrame::kBytesPerPixel;
      if (offset + last_center_.size() <= frame.byte_count()) {
        std::memcpy(last_center_.data(), frame.data() + offset, last_center_.size());
      }
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
  [[nodiscard]] Pixel last_center() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return last_center_;
  }
  /// Every distinct size a frame has arrived at, in order of first arrival.
  [[nodiscard]] std::vector<Size> sizes() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return sizes_;
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
  Pixel last_center_{};
  std::vector<Size> sizes_;
  dv::Error error_;
};

/// Waits until `condition` holds or `limit` has passed, and says which.
template <typename Condition>
[[nodiscard]] bool within(std::chrono::milliseconds limit, Condition condition) {
  const auto deadline = std::chrono::steady_clock::now() + limit;
  while (!condition()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      return false;
    }
    std::this_thread::sleep_for(20ms);
  }
  return true;
}

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

TEST_F(ScreenCaptureTest, TheSystemWindowsCanBeListed) {
  const auto listed = windows();
  ASSERT_TRUE(listed.ok()) << listed.error().message;

  const auto screens = monitors();
  ASSERT_TRUE(screens.ok()) << screens.error().message;

  // Nothing says a machine has a window open, so the list may be empty. What
  // it must not hold is a nameless entry, or an id that a monitor also
  // answers to: start() tells the two apart by the id alone.
  for (const Window& window : listed.value()) {
    EXPECT_FALSE(window.id.empty());
    EXPECT_FALSE(window.title.empty()) << "a window with no title has nothing to show in a menu";
    for (const Monitor& monitor : screens.value()) {
      EXPECT_NE(window.id, monitor.id) << "a window and a monitor share the id " << window.id;
    }
  }
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

#if defined(_WIN32)

// --- one window --------------------------------------------------------------
//
// A window of this process's own is what gets captured, because it is the one
// window the test can be sure exists, can paint something known into, and can
// minimize and close on cue. It lives on a thread of its own that pumps
// messages: the system leaves a window that does not answer out of the list,
// and the capturer's start blocks the thread that calls it.

/// The colour the test window paints itself, and what the middle of a captured
/// frame has to come back as. Nothing else on a desktop is quite this blue.
constexpr Pixel kWindowColour{200, 120, 40, 255};  // BGRA

class TestWindow {
 public:
  explicit TestWindow(std::wstring title) : title_(std::move(title)) {
    std::promise<HWND> created;
    std::future<HWND> handle = created.get_future();
    thread_ = std::thread([this, &created] { run(created); });
    handle_ = handle.get();
  }

  ~TestWindow() { close(); }

  TestWindow(const TestWindow&) = delete;
  TestWindow& operator=(const TestWindow&) = delete;
  TestWindow(TestWindow&&) = delete;
  TestWindow& operator=(TestWindow&&) = delete;

  [[nodiscard]] bool ok() const { return handle_ != nullptr; }

  /// The title as the capturer will list it, which is UTF-8.
  [[nodiscard]] std::string title() const {
    std::string narrow(title_.size(), '\0');
    for (std::size_t i = 0; i < title_.size(); ++i) {
      // ASCII only, by construction of the title below.
      narrow[i] = static_cast<char>(title_[i]);
    }
    return narrow;
  }

  [[nodiscard]] bool minimized() const { return IsIconic(handle_) != 0; }

  // Asynchronous on purpose: the window's thread does the showing, and the
  // tests wait on `minimized()` rather than on the call returning.
  void minimize() { ShowWindowAsync(handle_, SW_MINIMIZE); }
  void restore() { ShowWindowAsync(handle_, SW_RESTORE); }

  /// Closes the window and waits for its thread to finish. Safe to call twice.
  void close() {
    if (!thread_.joinable()) {
      return;
    }
    if (handle_ != nullptr) {
      PostMessageW(handle_, WM_CLOSE, 0, 0);
    }
    thread_.join();
    handle_ = nullptr;
  }

 private:
  static LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wide, LPARAM low) {
    switch (message) {
      case WM_PAINT: {
        PAINTSTRUCT paint{};
        const HDC context = BeginPaint(window, &paint);
        const HBRUSH brush =
            CreateSolidBrush(RGB(kWindowColour[2], kWindowColour[1], kWindowColour[0]));
        FillRect(context, &paint.rcPaint, brush);
        DeleteObject(brush);
        EndPaint(window, &paint);
        return 0;
      }
      case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(window, message, wide, low);
    }
  }

  void run(std::promise<HWND>& created) {
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = procedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"PartyShareCaptureTestWindow";
    // Refused with ERROR_CLASS_ALREADY_EXISTS from the second window on, and
    // that is fine: the class is the same one.
    RegisterClassW(&window_class);

    const HWND window = CreateWindowExW(0, window_class.lpszClassName, title_.c_str(),
                                        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 480, 320,
                                        nullptr, nullptr, window_class.hInstance, nullptr);
    created.set_value(window);
    if (window == nullptr) {
      return;
    }

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
      TranslateMessage(&message);
      DispatchMessageW(&message);
    }
  }

  std::wstring title_;
  std::thread thread_;
  HWND handle_ = nullptr;
};

class WindowCaptureTest : public ScreenCaptureTest {
 protected:
  void SetUp() override {
    ScreenCaptureTest::SetUp();
    if (IsSkipped()) {
      return;
    }
    // The process id keeps two test runs side by side from finding each
    // other's window.
    window_ = std::make_unique<TestWindow>(L"PartyShare capture test " +
                                           std::to_wstring(GetCurrentProcessId()));
    ASSERT_TRUE(window_->ok()) << "could not create a window to capture";
  }

  void TearDown() override {
    // The capturer first: its thread may still be looking at the handle.
    ScreenCaptureTest::TearDown();
    window_.reset();
  }

  /// The test window's id as the capturer lists it, or empty when it never
  /// turned up. Polled, because a window just created can take a moment to
  /// be shown, and until it is shown it is not listed.
  [[nodiscard]] std::string window_id() const {
    std::string found;
    (void)within(3s, [&] {
      const auto listed = windows();
      if (!listed) {
        return false;
      }
      for (const Window& window : listed.value()) {
        if (window.title == window_->title()) {
          found = window.id;
          return true;
        }
      }
      return false;
    });
    return found;
  }

  void start_capturing(const std::string& id) {
    auto created =
        create_screen_capturer(ScreenCaptureOptions{}, recorder_.sink(), recorder_.errors());
    ASSERT_TRUE(created.ok()) << created.error().message;
    capturer_ = std::move(created).take();
    const auto started = capturer_->start(id);
    ASSERT_TRUE(started.ok()) << started.error().code << ": " << started.error().message;
    ASSERT_TRUE(within(10s, [&] { return recorder_.count() >= 3; }))
        << "no frames from the window: " << recorder_.error().message;
  }

  std::unique_ptr<TestWindow> window_;
};

TEST_F(WindowCaptureTest, TheWindowIsListed) {
  EXPECT_FALSE(window_id().empty()) << "the test's own window is not in the list";
}

TEST_F(WindowCaptureTest, CapturingAWindowProducesItsPixels) {
  const std::string id = window_id();
  ASSERT_FALSE(id.empty());
  start_capturing(id);
  EXPECT_TRUE(capturer_->capturing());

  // Inside the ceiling, like a monitor. A 480x320 window on a 100% display is
  // sent as it is; on a scaled display it is bigger, and still inside.
  const Size size = recorder_.last_size();
  EXPECT_GT(size.width, 0);
  EXPECT_GT(size.height, 0);
  EXPECT_LE(size.width, 1280);
  EXPECT_LE(size.height, 720);
  EXPECT_EQ(size.width % 2, 0);
  EXPECT_EQ(size.height % 2, 0);

  // The proof that it is the window and not the screen it sits on: the middle
  // of the frame is the colour the window paints, whatever the display's
  // scale did to the size. A margin of a few steps, for a compositor that
  // rounds.
  const Pixel centre = recorder_.last_center();
  for (std::size_t channel = 0; channel < 3; ++channel) {
    EXPECT_NEAR(static_cast<int>(centre[channel]), static_cast<int>(kWindowColour[channel]), 8)
        << "channel " << channel << " of the middle pixel is not the window's colour";
  }
}

TEST_F(WindowCaptureTest, MinimizingTheWindowPausesTheShare) {
  const std::string id = window_id();
  ASSERT_FALSE(id.empty());
  start_capturing(id);

  window_->minimize();
  ASSERT_TRUE(within(3s, [&] { return window_->minimized(); }));
  // A frame captured just before the window went down may still be on its
  // way. After that, nothing.
  std::this_thread::sleep_for(300ms);
  const std::uint64_t while_minimized = recorder_.count();
  std::this_thread::sleep_for(1s);
  EXPECT_EQ(recorder_.count(), while_minimized) << "frames kept coming from a minimized window";
  EXPECT_TRUE(capturer_->capturing()) << "minimizing ended the share instead of pausing it";
  EXPECT_TRUE(recorder_.error().code.empty()) << recorder_.error().message;

  window_->restore();
  ASSERT_TRUE(within(3s, [&] { return !window_->minimized(); }));
  EXPECT_TRUE(within(5s, [&] { return recorder_.count() > while_minimized; }))
      << "restoring the window did not resume the frames: " << recorder_.error().message;

  // And they come back at the window's size. A frame of some other size on
  // the way out of the taskbar would make the encoder start over for a
  // picture nobody sees.
  std::this_thread::sleep_for(1s);
  const std::vector<Size> sizes = recorder_.sizes();
  EXPECT_EQ(sizes.size(), 1U) << "frames arrived at " << sizes.size() << " different sizes";
}

TEST_F(WindowCaptureTest, ClosingTheWindowEndsTheShare) {
  const std::string id = window_id();
  ASSERT_FALSE(id.empty());
  start_capturing(id);

  window_->close();
  ASSERT_TRUE(within(10s, [&] { return !recorder_.error().code.empty(); }))
      << "the share outlived the window";
  EXPECT_EQ(recorder_.error().code, "window_closed") << recorder_.error().message;
  EXPECT_FALSE(capturer_->capturing());
}

TEST_F(WindowCaptureTest, AMinimizedWindowIsRefused) {
  const std::string id = window_id();
  ASSERT_FALSE(id.empty());
  window_->minimize();
  ASSERT_TRUE(within(3s, [&] { return window_->minimized(); }));

  auto created =
      create_screen_capturer(ScreenCaptureOptions{}, recorder_.sink(), recorder_.errors());
  ASSERT_TRUE(created.ok()) << created.error().message;
  capturer_ = std::move(created).take();

  const auto started = capturer_->start(id);
  ASSERT_FALSE(started.ok());
  EXPECT_EQ(started.error().code, "window_minimized") << started.error().message;
  EXPECT_FALSE(capturer_->capturing());
}

TEST_F(WindowCaptureTest, AClosedWindowIsRefused) {
  const std::string id = window_id();
  ASSERT_FALSE(id.empty());
  window_->close();

  auto created =
      create_screen_capturer(ScreenCaptureOptions{}, recorder_.sink(), recorder_.errors());
  ASSERT_TRUE(created.ok()) << created.error().message;
  capturer_ = std::move(created).take();

  const auto started = capturer_->start(id);
  ASSERT_FALSE(started.ok());
  EXPECT_EQ(started.error().code, "window_not_found") << started.error().message;
  EXPECT_FALSE(capturer_->capturing());
}

#endif  // defined(_WIN32)

}  // namespace
