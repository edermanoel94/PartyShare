#pragma once

#include <string>
#include <string_view>

namespace dv::client::audio {

/// Anything under this is a microphone that cannot carry the top of a voice,
/// and the person using it deserves to be told. 32 kHz keeps 16 kHz of audio,
/// which is everything Opus sends; 16 kHz keeps 8, which is a telephone.
inline constexpr int kNarrowMicrophoneHz = 32000;

/// The one sentence the settings dialog shows under the microphone when the
/// device delivers less than the voice needs, and nothing when it does not.
///
/// Pure, so that the words can be tested without a dialog. Two cases, because
/// they have two different fixes: a wired device on a low default format is
/// changed in the Windows sound settings, and a Bluetooth headset on the
/// hands-free profile cannot be changed at all - that profile is 8 or 16 kHz
/// by definition, and the only way up is a different connection.
///
/// A rate of zero means nothing has been captured yet, which is not a problem
/// to report. See docs/16-audio-plan.md, step 6.
[[nodiscard]] inline std::string microphone_rate_hint(int sample_rate_hz,
                                                      std::string_view device_name) {
  if (sample_rate_hz <= 0 || sample_rate_hz >= kNarrowMicrophoneHz) {
    return {};
  }
  const std::string rate = std::to_string(sample_rate_hz / 1000);
  const std::string top = std::to_string(sample_rate_hz / 2000);
  const std::string what =
      "This microphone runs at " + rate + " kHz, so voice above " + top + " kHz is lost. ";
  if (device_name.find("Hands-Free") != std::string_view::npos ||
      device_name.find("Hands Free") != std::string_view::npos) {
    return what +
           "That is the Bluetooth hands-free profile, which cannot go higher: a wired "
           "connection or a different device is the only way up.";
  }
  return what + "Change its default format in the Windows sound settings, to 24 bit, 48000 Hz.";
}

}  // namespace dv::client::audio
