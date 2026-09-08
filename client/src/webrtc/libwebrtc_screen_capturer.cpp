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
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <libyuv/scale_argb.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <modules/desktop_capture/desktop_capturer.h>
#include <modules/desktop_capture/desktop_frame.h>

#if defined(WEBRTC_WIN)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <objbase.h>
#include <windows.h>
#endif

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

/// What a window's id starts with, so that `start` can tell it from a
/// monitor's without either side of the interface knowing how the other is
/// numbered. On Windows the number after it is the window handle, which is
/// what libwebrtc uses as a window's source id.
constexpr std::string_view kWindowPrefix = "window:";

[[nodiscard]] webrtc::DesktopCaptureOptions capture_options() {
  webrtc::DesktopCaptureOptions options = webrtc::DesktopCaptureOptions::CreateDefault();
#if defined(WEBRTC_WIN)
  // Desktop Duplication rather than the GDI fallback, as section 7 of SPEC.md
  // asks.
  options.set_allow_directx_capturer(true);
  // Windows Graphics Capture for windows, the same section's first choice.
  // The GDI window capturer paints a window black when its content is
  // hardware accelerated - a browser, a game, anything on Direct3D - and
  // cannot see one that another window is covering. Graphics Capture reads
  // the compositor's copy, which has both. Screens stay on Desktop
  // Duplication: it is what docs/11-benchmarks.md measured, and nothing about
  // capturing a window changes what a screen costs.
  options.set_allow_wgc_window_capturer(true);
#endif
  return options;
}

#if defined(WEBRTC_WIN)
/// COM, for the thread this is on.
///
/// Windows Graphics Capture is a WinRT API and refuses a thread that has not
/// joined an apartment - CO_E_NOTINITIALIZED from inside libwebrtc, which it
/// reports as the window not being capturable. Desktop Duplication never
/// needed this, which is why the capture thread got by without it until
/// windows came along.
///
/// Multithreaded rather than single-threaded: nothing here pumps messages,
/// and a single-threaded apartment that does not pump is one that eventually
/// hangs a cross-apartment call. A thread already in an apartment of either
/// kind - the interface's, which Qt joined at startup - is left as it is; it
/// is initialised, which is all Graphics Capture asks.
class ComScope {
 public:
  ComScope() : owned_(SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED))) {}

  ~ComScope() {
    if (owned_) {
      CoUninitialize();
    }
  }

  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
  ComScope(ComScope&&) = delete;
  ComScope& operator=(ComScope&&) = delete;

 private:
  bool owned_;
};
#endif

/// What `start` was handed, once its id has been read.
struct SourceRef {
  enum class Kind : std::uint8_t {
    /// An empty id: whichever monitor the system calls primary.
    PrimaryMonitor,
    Monitor,
    Window,
  };
  Kind kind = Kind::PrimaryMonitor;
  webrtc::DesktopCapturer::SourceId id = 0;
};

[[nodiscard]] Result<SourceRef> parse_source(const std::string& source_id) {
  if (source_id.empty()) {
    return SourceRef{};
  }
  const bool window = source_id.starts_with(kWindowPrefix);
  const std::string number = window ? source_id.substr(kWindowPrefix.size()) : source_id;
  try {
    return SourceRef{.kind = window ? SourceRef::Kind::Window : SourceRef::Kind::Monitor,
                     .id = static_cast<webrtc::DesktopCapturer::SourceId>(std::stoll(number))};
  } catch (const std::exception&) {
    return window ? Result<SourceRef>::failure("window_not_found",
                                               "not a window identifier: " + source_id)
                  : Result<SourceRef>::failure("monitor_not_found",
                                               "not a monitor identifier: " + source_id);
  }
}

[[nodiscard]] std::string window_id(webrtc::DesktopCapturer::SourceId id) {
  return std::string(kWindowPrefix) + std::to_string(id);
}

[[nodiscard]] std::string monitor_name(const webrtc::DesktopCapturer::Source& source,
                                       std::size_t index) {
  // X11 gives the output name, "DP-2". Windows and macOS give a title, and
  // sometimes nothing at all, which is what the number is for.
  return source.title.empty() ? "Monitor " + std::to_string(index + 1) : source.title;
}

/// One entry of `monitors()`, before the primary is settled.
[[nodiscard]] Monitor describe(const webrtc::DesktopCapturer::Source& source, std::size_t index) {
  Monitor monitor{
      .id = std::to_string(source.id), .name = monitor_name(source, index), .is_primary = false};
#if defined(WEBRTC_WIN)
  // On Windows a source id is the display's index in EnumDisplayDevices, for
  // the GDI capturer and the Desktop Duplication one alike - see GetScreenList
  // in modules/desktop_capture/win/screen_capture_utils.cc, which both draw
  // their lists from. So the same call answers which display is the primary
  // and, through its current mode, how big it is. Neither costs a frame.
  DISPLAY_DEVICEW device{};
  device.cb = sizeof(device);
  if (source.id >= 0 &&
      EnumDisplayDevicesW(nullptr, static_cast<DWORD>(source.id), &device, 0) != 0) {
    monitor.is_primary = (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0;
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode) != 0 &&
        mode.dmPelsWidth > 0 && mode.dmPelsHeight > 0) {
      monitor.name +=
          " (" + std::to_string(mode.dmPelsWidth) + "x" + std::to_string(mode.dmPelsHeight) + ")";
    }
  }
#endif
  return monitor;
}

/// The monitors `capturer` can see, primary first.
///
/// Where the system does not say which one is primary, the first it lists is
/// taken to be. Either way exactly one entry says so, and it is the front.
[[nodiscard]] Result<std::vector<Monitor>> list_monitors(webrtc::DesktopCapturer& capturer) {
  webrtc::DesktopCapturer::SourceList sources;
  if (!capturer.GetSourceList(&sources)) {
    return Result<std::vector<Monitor>>::failure("capture_unavailable",
                                                 "the system would not list its monitors");
  }

  std::vector<Monitor> found;
  found.reserve(sources.size());
  for (std::size_t index = 0; index < sources.size(); ++index) {
    found.push_back(describe(sources[index], index));
  }

  const auto primary =
      std::ranges::find_if(found, [](const Monitor& monitor) { return monitor.is_primary; });
  if (primary == found.end()) {
    if (!found.empty()) {
      found.front().is_primary = true;
    }
  } else {
    // To the front, with the others kept in the order the system gave them.
    std::rotate(found.begin(), primary, std::next(primary));
  }
  return found;
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

  return VideoFrame{target, std::move(reuse)};
}

class LibwebrtcScreenCapturer final : public ScreenCapturer,
                                      public webrtc::DesktopCapturer::Callback {
 public:
  LibwebrtcScreenCapturer(ScreenCaptureOptions options, FrameSink frames, ErrorSink errors)
      : options_(options), frames_(std::move(frames)), errors_(std::move(errors)) {}

  ~LibwebrtcScreenCapturer() override { stop(); }

  Result<std::monostate> start(const std::string& source_id) override {
    stop();

    const Result<SourceRef> parsed = parse_source(source_id);
    if (!parsed) {
      return Result<std::monostate>::failure(parsed.error());
    }

    // The capturer is created and driven on the capture thread. Several of the
    // platform backends bind themselves to the thread that created them, so
    // building one here and using it there would work on X11 and fail on
    // Windows.
    running_.store(true);
    std::promise<Result<std::monostate>> started;
    std::future<Result<std::monostate>> ready = started.get_future();
    thread_ = std::thread([this, source = parsed.value(), started = std::move(started)]() mutable {
      capture_loop(source, started);
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
    running_.store(false);
    // The thread is what decides whether there is anything to do, not the
    // flag. A capture that ended on its own - the window closed, the monitor
    // unplugged, the portal refused - has already cleared the flag in fail(),
    // and a stop that took the flag's word for it would leave the thread
    // joinable for the destructor to trip over, which std::thread answers
    // with std::terminate.
    if (!thread_.joinable()) {
      return;
    }
    // Called from a sink means called from the capture thread, and a thread
    // cannot join itself.
    if (thread_.get_id() == std::this_thread::get_id()) {
      thread_.detach();
    } else {
      thread_.join();
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
  void capture_loop(SourceRef source, std::promise<Result<std::monostate>>& started) {
#if defined(WEBRTC_WIN)
    const ComScope com;
#endif
    const bool window = source.kind == SourceRef::Kind::Window;
    std::unique_ptr<webrtc::DesktopCapturer> capturer =
        window ? webrtc::DesktopCapturer::CreateWindowCapturer(capture_options())
               : webrtc::DesktopCapturer::CreateScreenCapturer(capture_options());
    if (capturer == nullptr) {
      started.set_value(Result<std::monostate>::failure(
          "capture_unavailable", window ? "this system has no window capturer"
                                        : "this system has no screen capturer, is a display "
                                          "attached?"));
      return;
    }

#if defined(WEBRTC_WIN)
    // On Windows a window's source id is its handle, for Graphics Capture and
    // GDI alike, and the handle answers the two questions the user needs
    // answered apart. SelectSource below refuses a closed window and a
    // minimized one with the same false, and "that window no longer exists"
    // is the wrong thing to tell somebody who only minimized it.
    //
    // The cast is the one libwebrtc itself makes in both directions - see
    // GetWindowList in modules/desktop_capture/win/window_capture_utils.cc -
    // and there is no other way back from the number it hands out.
    // NOLINTNEXTLINE(performance-no-int-to-ptr)
    const HWND handle = window ? reinterpret_cast<HWND>(source.id) : nullptr;
    if (window && IsWindow(handle) == 0) {
      started.set_value(Result<std::monostate>::failure(
          "window_not_found", "window " + std::to_string(source.id) + " has closed"));
      return;
    }
    if (window && IsIconic(handle) != 0) {
      started.set_value(Result<std::monostate>::failure(
          "window_minimized", "window " + std::to_string(source.id) + " is minimized"));
      return;
    }
#endif

    if (source.kind == SourceRef::Kind::PrimaryMonitor) {
      // An empty id promises the primary monitor, and libwebrtc's own default
      // is not that. A capturer never told SelectSource keeps
      // kFullDesktopScreenId and duplicates the whole desktop: on two monitors
      // that is both of them side by side, fitted into 720p as a strip half
      // the height, and it is what everybody with a second screen was sending
      // until this chose for them. Listed here, on the capture thread and from
      // this capturer, because the ids belong to the capturer that made them.
      if (const auto listed = list_monitors(*capturer); listed && !listed.value().empty()) {
        // Written by describe() from a number, so it reads back as one.
        source.id =
            static_cast<webrtc::DesktopCapturer::SourceId>(std::stoll(listed.value().front().id));
        source.kind = SourceRef::Kind::Monitor;
      } else {
        DV_LOG_WARN("Screen capture: could not tell which monitor is primary, sharing them all");
      }
    }

    if (source.kind != SourceRef::Kind::PrimaryMonitor && !capturer->SelectSource(source.id)) {
      started.set_value(
          window ? Result<std::monostate>::failure(
                       "window_not_found", "the system refused window " + std::to_string(source.id))
                 : Result<std::monostate>::failure(
                       "monitor_not_found",
                       "the system refused monitor " + std::to_string(source.id)));
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
#if defined(WEBRTC_WIN)
      if (window && IsWindow(handle) == 0) {
        // Gone, and the handle is the one that says so in time. Graphics
        // Capture would say it through the item's Closed event, but that
        // event is delivered by a dispatcher queue bound to this thread, and
        // this thread pumps no messages - so it would arrive never, and the
        // capturer would hand back the last frame it has, forever, as a
        // success. The GDI capturer answers at the next frame. Asking the
        // window itself answers now, for both.
        fail("window_closed", "the shared window was closed");
        return;
      }
      if (window && IsIconic(handle) != 0) {
        // Paused. A minimized window has no pixels, and what the backends do
        // about that differs: Graphics Capture hands back the last frame it
        // has, again and again, and GDI a 1x1 black one. Neither is worth
        // encoding, so neither is asked for. Not a failure either, so the
        // counter below stays where it is and the share outlives a minute in
        // the taskbar; everybody else keeps the last frame that was sent. The
        // first-frame deadline moves too: a share minimized the moment it
        // started is waiting on the user, not on the system.
        first_frame_deadline = Clock::now() + kFirstFrameTimeout;
        {
          // Says zero, because zero is what is being sent. A rate left at
          // its last value would have the metrics report a pause as thirty
          // frames a second of nothing.
          const std::lock_guard<std::mutex> lock(stats_mutex_);
          stats_.fps = 0;
        }
        window_frames = 0;
        window_started = Clock::now();
        std::this_thread::sleep_for(interval);
        next_frame_at = Clock::now();
        continue;
      }
#endif
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
          if (window) {
#if defined(WEBRTC_WIN)
            if (IsWindow(handle) == 0) {
              fail("window_closed", "the shared window was closed");
              return;
            }
#endif
            fail("window_capture_failed", "the system stopped producing frames from the window");
            return;
          }
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

  return list_monitors(*capturer);
}

Result<std::vector<Window>> windows() {
#if defined(WEBRTC_WIN)
  // Listing needs no apartment, but choosing the backend does: whether
  // Graphics Capture is available is a WinRT question, and a thread that
  // cannot ask it gets the GDI list, which includes tool windows that
  // Graphics Capture cannot capture. The same list from every thread, then.
  const ComScope com;
#endif
  std::unique_ptr<webrtc::DesktopCapturer> capturer =
      webrtc::DesktopCapturer::CreateWindowCapturer(capture_options());
  if (capturer == nullptr) {
    return Result<std::vector<Window>>::failure("capture_unavailable",
                                                "this system has no window capturer");
  }

  webrtc::DesktopCapturer::SourceList sources;
  if (!capturer->GetSourceList(&sources)) {
    return Result<std::vector<Window>>::failure("capture_unavailable",
                                                "the system would not list its windows");
  }

  std::vector<Window> found;
  found.reserve(sources.size());
  for (const webrtc::DesktopCapturer::Source& source : sources) {
    // The backends already leave untitled windows out. Kept here as well
    // because the header promises it, and a promise that rests on somebody
    // else's filter is one release away from being broken.
    if (source.title.empty()) {
      continue;
    }
    found.push_back(Window{.id = window_id(source.id), .title = source.title});
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
