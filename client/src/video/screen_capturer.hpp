#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <dv/core/result.hpp>

#include "video/frame_size.hpp"
#include "video/video_frame.hpp"

namespace dv::client::video {

/// A screen the system can capture.
struct Monitor {
  /// Opaque, and only meaningful to the capturer that produced it. Monitor
  /// identifiers are not stable across reboots on any platform, so this is
  /// remembered for the length of a session and no longer.
  std::string id;
  /// What to put in front of the user, for example "DP-2" or "Monitor 1".
  ///
  /// There is no size field on purpose. No backend reports one in its source
  /// list, and on most systems the only way to find out is to capture a frame
  /// from each monitor, which on Wayland means a permission prompt per monitor
  /// just to fill in a menu label. Windows is the exception - the display
  /// settings answer for free - and there the size rides inside the name,
  /// "Monitor 1 (2560x1440)", because two monitors of the same make need
  /// something to tell them apart by.
  std::string name;
  /// The one the system calls primary, where it says - Windows does - and the
  /// first one listed where it does not. Exactly one monitor carries this.
  bool is_primary = false;

  friend bool operator==(const Monitor&, const Monitor&) = default;
};

/// A window the system can capture - one program's window rather than the
/// whole screen it sits on.
struct Window {
  /// Opaque, only meaningful to the capturer that produced it, and only for
  /// as long as the window is open: closing the program and opening it again
  /// gives a different id. Never collides with a monitor's, so the two can be
  /// told apart by `start` without the caller knowing how either is made.
  std::string id;
  /// The title bar's text, "report.docx - Word", which is how the user knows
  /// which one they mean. Never empty: the system does not list untitled
  /// windows, and a menu could not show them anyway.
  std::string title;

  friend bool operator==(const Window&, const Window&) = default;
};

/// Section 5.2 of SPEC.md: 1280x720 at 30 FPS.
struct ScreenCaptureOptions {
  Size max_size{.width = 1280, .height = 720};
  int max_fps = 30;
};

/// What the capture is doing, for the log and for the tests.
struct ScreenCaptureStats {
  std::uint64_t frames_captured = 0;
  /// Frames the platform refused to produce. A few are normal: nothing changed
  /// on screen, or the compositor was busy.
  std::uint64_t frames_failed = 0;
  /// Frames thrown away because the encoder was behind. See FrameQueue.
  std::uint64_t frames_dropped = 0;
  /// Measured over the last second, so it can be compared against max_fps.
  double fps = 0;
};

/// Captures one monitor or one window, and hands the frames over one at a
/// time.
///
/// This exists so that nothing above it knows which platform it is on. Section
/// 7 of SPEC.md names a different API per system, Windows Graphics Capture,
/// ScreenCaptureKit, PipeWire and X11, and libwebrtc's DesktopCapturer already
/// chooses between them. What this interface adds is the boundary: the code
/// that decides what to send, when to stop and what to draw never sees any of
/// that, and can be built and tested without libwebrtc at all.
///
/// Wayland is the reason `start` is asynchronous about failure. There the
/// system asks the user for consent through a portal, and the answer arrives
/// later, so a capturer that only reported failure by return value could not
/// express "the user said no".
///
/// Frames arrive on the capturer's own thread, never on the caller's.
class ScreenCapturer {
 public:
  /// Takes ownership of the frame. Called on the capture thread, so it must not
  /// block: whatever it does, a slow sink costs frames.
  using FrameSink = std::function<void(VideoFrame frame)>;

  /// Reports that capture ended on its own, with the reason. Permission denied
  /// and a monitor being unplugged both arrive this way.
  using ErrorSink = std::function<void(Error error)>;

  ScreenCapturer() = default;
  virtual ~ScreenCapturer() = default;

  ScreenCapturer(const ScreenCapturer&) = delete;
  ScreenCapturer& operator=(const ScreenCapturer&) = delete;
  ScreenCapturer(ScreenCapturer&&) = delete;
  ScreenCapturer& operator=(ScreenCapturer&&) = delete;

  /// Starts capturing `source_id`, a monitor as `monitors()` listed it or a
  /// window as `windows()` did. An empty id means the primary monitor, the
  /// one `monitors()` lists first - one monitor, never the whole desktop with
  /// every screen side by side, which is what the platform's own default is.
  ///
  /// Fails with `monitor_not_found` when the id is neither a window's nor one
  /// the system reports as a monitor, and with `capture_unavailable` when the
  /// platform refuses to start at all. A window has two refusals of its own:
  /// `window_not_found` once it has closed, and `window_minimized` while it is
  /// minimized, because a minimized window has no pixels to capture and the
  /// system will not start on one. A refusal that only arrives later, such as
  /// a user declining the Wayland portal, comes through the error sink
  /// instead.
  ///
  /// A window that is minimized *during* a share pauses it rather than ending
  /// it: no frames leave until it is restored, the last one sent stays on
  /// everybody else's screen, and nothing is reported. Closing the window ends
  /// the share, with `window_closed` through the error sink.
  [[nodiscard]] virtual Result<std::monostate> start(const std::string& source_id) = 0;

  /// Stops capturing and joins the capture thread. Safe to call when not
  /// running, and safe to call from inside a sink.
  virtual void stop() = 0;

  [[nodiscard]] virtual bool capturing() const = 0;

  [[nodiscard]] virtual ScreenCaptureStats stats() const = 0;
};

/// The monitors the system reports, primary first.
///
/// Fails with `capture_unavailable` in a build without libwebrtc, and on a
/// system where no screen capturer can be created.
[[nodiscard]] Result<std::vector<Monitor>> monitors();

/// The windows the system would let us capture, front-most first, which is
/// the order it lists them in. A minimized window is still a window and is
/// still listed, even though `start` refuses it until it is restored: the
/// list is what the user picks from, and a program they minimized a minute ago
/// is one they may well want.
///
/// This client's own windows are in the list too, and sharing one is allowed.
/// Filtering them out would need the system to tell whose each window is,
/// which it only does on some platforms, and a list that differs by platform
/// is worse than one entry nobody picks.
///
/// Fails with `capture_unavailable` in a build without libwebrtc, and on a
/// system where no window capturer can be created.
[[nodiscard]] Result<std::vector<Window>> windows();

/// True when this build can capture a screen at all.
[[nodiscard]] bool screen_capture_is_available() noexcept;

[[nodiscard]] Result<std::unique_ptr<ScreenCapturer>> create_screen_capturer(
    const ScreenCaptureOptions& options, ScreenCapturer::FrameSink frames,
    ScreenCapturer::ErrorSink errors);

}  // namespace dv::client::video
