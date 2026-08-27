// Every platform that is not Windows.
//
// Linux and macOS can both do this - a PulseAudio or PipeWire monitor source
// there, ScreenCaptureKit's audio output here - but neither is written yet, and
// section 8 of docs/09-screen-audio.md says so out loud. Until
// then this answers honestly rather than silently sharing nothing.
//
// The same shape as client/src/video/screen_capturer.cpp and
// client/src/media/media_session.cpp: everything above the interface still
// compiles, links and runs.

#include "audio/audio_sources.hpp"
#include "audio/loopback_capturer.hpp"

#if !defined(_WIN32)

namespace dv::client::audio {

namespace {

constexpr const char* kUnavailable =
    "capturing what an application is playing is only implemented on Windows. "
    "See docs/09-screen-audio.md, section 8.";

}  // namespace

bool loopback_capture_is_available() noexcept {
  return false;
}

// Taken by value because the real implementation moves them, and a stub cannot
// change the signature.
// NOLINTBEGIN(performance-unnecessary-value-param)
Result<std::unique_ptr<LoopbackCapturer>> create_loopback_capturer(
    LoopbackCapturer::BlockSink /*blocks*/, LoopbackCapturer::ErrorSink /*errors*/) {
  // NOLINTEND(performance-unnecessary-value-param)
  return Result<std::unique_ptr<LoopbackCapturer>>::failure("capture_unavailable", kUnavailable);
}

Result<std::vector<AudioSource>> audio_sources() {
  return Result<std::vector<AudioSource>>::failure("capture_unavailable", kUnavailable);
}

}  // namespace dv::client::audio

#endif  // !defined(_WIN32)
