// The libwebrtc side of video::ScreenCapturer.
//
// Above it there is the interface in client/src/video/screen_capturer.hpp, and
// nothing above that knows which platform API is producing the pixels. Section
// 7 of SPEC.md names one per system; libwebrtc's DesktopCapturer chooses, and
// this file is the seam.
//
// Built only when DV_BUILD_CLIENT_MEDIA is on. The stub that takes its place
// otherwise lives in client/src/video/screen_capturer.cpp.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <libyuv/scale_argb.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <modules/desktop_capture/desktop_capturer.h>
#include <modules/desktop_capture/desktop_frame.h>

#include <dv/logging/logger.hpp>

#include "video/screen_capturer.hpp"

namespace dv::client::video {
namespace {

using Clock = std::chrono::steady_clock;

/// How long to keep pumping before giving up on the first frame.
///
/// On X11 the answer comes back inside CaptureFrame. Behind the XDG portal it
/// does not: the system puts a dialog in front of the user and the first frame
/// only arrives once they have picked a screen and agreed. Fifteen seconds is
/// long enough for a person to read the dialog and short enough that a refusal
/// does not look like a hang.
constexpr auto kFirstFrameTimeout = std::chrono::seconds(15);

/// Consecutive failures tolerated before capture is declared over.
///
/// Individual failures are ordinary: nothing changed on screen, or the
/// compositor was busy. A run of them is not.
constexpr int kMaxConsecutiveFailures = 30;

[[nodiscard]] webrtc::DesktopCaptureOptions capture_options() {
  webrtc::DesktopCaptureOptions options = webrtc::DesktopCaptureOptions::CreateDefault();
#if defined(WEBRTC_WIN)
  // Desktop Duplication rather than the GDI fallback, as section 7 of SPEC.md
  // asks.
  options.set_allow_directx_capturer(true);
#endif
  return options;
}

[[nodiscard]] std::string monitor_name(const webrtc::DesktopCapturer::Source& source, Size size,
                                       std::size_t index) {
  std::string title = source.title;
  if (title.empty()) {
    title = "Monitor " + std::to_string(index + 1);
  }
  if (!size.empty()) {
    title += " (" + std::to_string(size.width) + "x" + std::to_string(size.height) + ")";
  }
  return title;
}

/// Turns a captured BGRA frame into one at the size we mean to send.
///
/// The scaling happens here rather than further down the pipeline so that the
/// queue and everything after it carry 720p frames instead of whatever the
/// monitor happens to be. On a 4K screen that is nine times less memory moved
/// per frame.
[[nodiscard]] VideoFrame scale_frame(const webrtc::DesktopFrame& source, Size target,
                                     std::vector<std::uint8_t> reuse) {
  const auto bytes = static_cast<std::size_t>(target.width) *
                     static_cast<std::size_t>(target.height) *
                     static_cast<std::size_t>(VideoFrame::kBytesPerPixel);
  reuse.resize(bytes);

  const int destination_stride = target.width * VideoFrame::kBytesPerPixel;
  libyuv::ARGBScale(source.data(), source.stride(), source.size().width(), source.size().height(),
                    reuse.data(), destination_stride, target.width, target.height,
                    libyuv::kFilterBox);

  return VideoFrame{target.width, target.height, std::move(reuse)};
}

class LibwebrtcScreenCapturer final : public ScreenCapturer,
                                      public webrtc::DesktopCapturer::Callback {
 public:
  LibwebrtcScreenCapturer(ScreenCaptureOptions options, FrameSink frames, ErrorSink errors)
      : options_(options), frames_(std::move(frames)), errors_(std::move(errors)) {}

  ~LibwebrtcScreenCapturer() override { stop(); }

  Result<std::monostate> start(const std::string& monitor_id) override {
    stop();

    webrtc::DesktopCapturer::SourceId source_id = 0;
    if (!monitor_id.empty()) {
      try {
        source_id = static_cast<webrtc::DesktopCapturer::SourceId>(std::stoll(monitor_id));
      } catch (const std::exception&) {
        return Result<std::monostate>::failure("monitor_not_found",
                                               "not a monitor identifier: " + monitor_id);
      }
    }

    // The capturer is created and driven on the capture thread. Several of the
    // platform backends bind themselves to the thread that created them, so
    // building one here and using it there would work on X11 and fail on
    // Windows.
    running_.store(true);
    std::promise<Result<std::monostate>> started;
    std::future<Result<std::monostate>> ready = started.get_future();
    thread_ = std::thread([this, source_id, use_default = monitor_id.empty(),
                           started = std::move(started)]() mutable {
      capture_loop(source_id, use_default, started);
    });

    Result<std::monostate> result = ready.get();
    if (!result) {
      running_.store(false);
      if (thread_.joinable()) {
        thread_.join();
      }
    }
    return result;
  }

  void stop() override {
    if (!running_.exchange(false)) {
      return;
    }
    // Called from a sink means called from the capture thread, and a thread
    // cannot join itself.
    if (thread_.joinable() && thread_.get_id() != std::this_thread::get_id()) {
      thread_.join();
    } else if (thread_.joinable()) {
      thread_.detach();
    }
  }

  [[nodiscard]] bool capturing() const override { return running_.load(); }

  [[nodiscard]] ScreenCaptureStats stats() const override {
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
  }

  // --- webrtc::DesktopCapturer::Callback -------------------------------------

  void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                       std::unique_ptr<webrtc::DesktopFrame> frame) override {
    captured_result_ = result;
    captured_frame_ = std::move(frame);
    captured_ = true;
  }

 private:
  void capture_loop(webrtc::DesktopCapturer::SourceId source_id, bool use_default,
                    std::promise<Result<std::monostate>>& started) {
    std::unique_ptr<webrtc::DesktopCapturer> capturer =
        webrtc::DesktopCapturer::CreateScreenCapturer(capture_options());
    if (capturer == nullptr) {
      started.set_value(Result<std::monostate>::failure(
          "capture_unavailable", "this system has no screen capturer, is a display attached?"));
      return;
    }

    if (!use_default && !capturer->SelectSource(source_id)) {
      started.set_value(Result<std::monostate>::failure(
          "monitor_not_found", "the system refused monitor " + std::to_string(source_id)));
      return;
    }

    capturer->Start(this);
    started.set_value(std::monostate{});

    const auto interval = std::chrono::microseconds(1'000'000 / std::max(1, options_.max_fps));
    auto next_frame_at = Clock::now();
    auto first_frame_deadline = Clock::now() + kFirstFrameTimeout;
    bool seen_a_frame = false;
    int consecutive_failures = 0;
    std::vector<std::uint8_t> reuse;
    auto window_started = Clock::now();
    std::uint64_t window_frames = 0;

    while (running_.load()) {
      captured_ = false;
      captured_frame_.reset();
      capturer->CaptureFrame();

      if (captured_ && captured_result_ == webrtc::DesktopCapturer::Result::SUCCESS &&
          captured_frame_ != nullptr && !captured_frame_->size().is_empty()) {
        seen_a_frame = true;
        consecutive_failures = 0;

        const Size source_size{captured_frame_->size().width(), captured_frame_->size().height()};
        const Size target = fit_within(source_size, options_.max_size);
        VideoFrame frame = scale_frame(*captured_frame_, target, std::move(reuse));
        reuse.clear();

        ++window_frames;
        {
          const std::lock_guard<std::mutex> lock(stats_mutex_);
          ++stats_.frames_captured;
        }
        if (frames_) {
          frames_(std::move(frame));
        }
      } else if (captured_) {
        ++consecutive_failures;
        {
          const std::lock_guard<std::mutex> lock(stats_mutex_);
          ++stats_.frames_failed;
        }
        if (captured_result_ == webrtc::DesktopCapturer::Result::ERROR_PERMANENT ||
            consecutive_failures >= kMaxConsecutiveFailures) {
          fail("capture_failed", "the system stopped producing frames");
          return;
        }
      }

      if (!seen_a_frame && Clock::now() > first_frame_deadline) {
        // The portal path ends here when the user declines, or simply never
        // answers the dialog.
        fail("capture_denied", "no frame arrived, was the screen share permission refused?");
        return;
      }

      // The frame rate cap of section 5.2 of SPEC.md. Measured from the target
      // rather than from now, so a slow frame is followed by a quick one
      // instead of the rate drifting downwards.
      next_frame_at += interval;
      const auto now = Clock::now();
      if (next_frame_at > now) {
        std::this_thread::sleep_for(next_frame_at - now);
      } else {
        // So far behind that catching up would mean a burst. Give up the lost
        // time instead.
        next_frame_at = now;
      }

      if (const auto elapsed = Clock::now() - window_started; elapsed >= std::chrono::seconds(1)) {
        const double seconds = std::chrono::duration<double>(elapsed).count();
        const std::lock_guard<std::mutex> lock(stats_mutex_);
        stats_.fps = static_cast<double>(window_frames) / seconds;
        window_frames = 0;
        window_started = Clock::now();
      }
    }
  }

  void fail(std::string code, std::string message) {
    running_.store(false);
    DV_LOG_ERROR("Screen capture: {}", message);
    if (errors_) {
      errors_(Error{std::move(code), std::move(message)});
    }
  }

  ScreenCaptureOptions options_;
  FrameSink frames_;
  ErrorSink errors_;

  std::thread thread_;
  std::atomic<bool> running_{false};

  // Touched only on the capture thread, between CaptureFrame and the callback
  // it makes on that same thread.
  bool captured_ = false;
  webrtc::DesktopCapturer::Result captured_result_ =
      webrtc::DesktopCapturer::Result::ERROR_TEMPORARY;
  std::unique_ptr<webrtc::DesktopFrame> captured_frame_;

  mutable std::mutex stats_mutex_;
  ScreenCaptureStats stats_;
};

}  // namespace

Result<std::vector<Monitor>> monitors() {
  std::unique_ptr<webrtc::DesktopCapturer> capturer =
      webrtc::DesktopCapturer::CreateScreenCapturer(capture_options());
  if (capturer == nullptr) {
    return Result<std::vector<Monitor>>::failure(
        "capture_unavailable", "this system has no screen capturer, is a display attached?");
  }

  webrtc::DesktopCapturer::SourceList sources;
  if (!capturer->GetSourceList(&sources)) {
    return Result<std::vector<Monitor>>::failure("capture_unavailable",
                                                 "the system would not list its monitors");
  }

  std::vector<Monitor> found;
  found.reserve(sources.size());
  for (std::size_t index = 0; index < sources.size(); ++index) {
    const webrtc::DesktopCapturer::Source& source = sources[index];
    // The size is not part of the source list on any backend, and asking for it
    // means capturing a frame. The name carries what the platform gave us; the
    // real size arrives with the first frame.
    const Size size{};
    found.push_back(
        Monitor{std::to_string(source.id), monitor_name(source, size, index), size, index == 0});
  }
  return found;
}

bool screen_capture_is_available() noexcept {
  return webrtc::DesktopCapturer::CreateScreenCapturer(capture_options()) != nullptr;
}

Result<std::unique_ptr<ScreenCapturer>> create_screen_capturer(const ScreenCaptureOptions& options,
                                                               ScreenCapturer::FrameSink frames,
                                                               ScreenCapturer::ErrorSink errors) {
  return std::unique_ptr<ScreenCapturer>{
      new LibwebrtcScreenCapturer(options, std::move(frames), std::move(errors))};
}

}  // namespace dv::client::video
