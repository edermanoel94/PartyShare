#include "audio/audio_session.hpp"

namespace dv::client::audio {

#ifndef DV_WITH_LIBWEBRTC

// Build without libwebrtc. Everything above this interface still compiles and
// runs, which is what lets the tests and the CI matrix work without a 66 MB
// static library that has to be built from source.
//
// The counterpart lives in client/src/webrtc/libwebrtc_audio_session.cpp.

Result<std::unique_ptr<AudioSession>> create_audio_session(const AudioSessionOptions& /*options*/,
                                                           AudioSession::Callbacks /*callbacks*/) {
  return Result<std::unique_ptr<AudioSession>>::failure(
      "media_unavailable",
      "this client was built without libwebrtc, so it cannot send or receive audio. "
      "See docs/webrtc-toolchain.md.");
}

bool media_is_available() noexcept {
  return false;
}

Result<std::vector<AudioDevice>> input_devices() {
  return Result<std::vector<AudioDevice>>::failure(
      "media_unavailable", "this client was built without libwebrtc, so it has no audio devices");
}

Result<std::vector<AudioDevice>> output_devices() {
  return input_devices();
}

#endif  // DV_WITH_LIBWEBRTC

}  // namespace dv::client::audio
