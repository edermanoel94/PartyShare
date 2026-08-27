#include "video/screen_capturer.hpp"

namespace dv::client::video {

#ifndef DV_WITH_LIBWEBRTC

// Build without libwebrtc. Everything above this interface still compiles and
// runs, exactly as it does for audio in media/media_session.cpp.
//
// The counterpart lives in client/src/webrtc/libwebrtc_screen_capturer.cpp.

namespace {

constexpr const char* kUnavailable =
    "this client was built without libwebrtc, so it cannot capture a screen. "
    "See docs/07-webrtc-toolchain.md.";

}  // namespace

Result<std::vector<Monitor>> monitors() {
  return Result<std::vector<Monitor>>::failure("capture_unavailable", kUnavailable);
}

bool screen_capture_is_available() noexcept {
  return false;
}

// Same as create_media_session: the sinks are taken by value because the real
// implementation moves them, and this stub cannot change the signature.
// NOLINTBEGIN(performance-unnecessary-value-param)
Result<std::unique_ptr<ScreenCapturer>> create_screen_capturer(
    const ScreenCaptureOptions& /*options*/, ScreenCapturer::FrameSink /*frames*/,
    ScreenCapturer::ErrorSink /*errors*/) {
  // NOLINTEND(performance-unnecessary-value-param)
  return Result<std::unique_ptr<ScreenCapturer>>::failure("capture_unavailable", kUnavailable);
}

#endif  // DV_WITH_LIBWEBRTC

}  // namespace dv::client::video
