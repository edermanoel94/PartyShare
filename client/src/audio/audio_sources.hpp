#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <dv/core/result.hpp>

namespace dv::client::audio {

/// An application the machine is currently able to hear.
struct AudioSource {
  /// What the loopback capture targets. Not stable across runs of the
  /// application, so it is remembered for the length of a share and no longer -
  /// the same rule video::Monitor lives by.
  std::uint32_t process_id = 0;
  /// What to put in front of the user, for example "chrome" or "spotify".
  std::string name;
  /// True while the session is actually rendering. An application that has a
  /// stream open but is paused shows up with this false, which is worth
  /// distinguishing: it is choosable, but it is not what the user is hearing.
  bool playing = false;

  friend bool operator==(const AudioSource&, const AudioSource&) = default;
};

/// The applications with an audio session on the default playback device, the
/// ones actually rendering first.
///
/// This is a menu, not a promise: an application can appear here and stop
/// playing before the user picks it, and the reverse. Nothing is opened and no
/// permission is asked, so it is cheap enough to call every time a dialog is
/// shown.
///
/// This process is never in the list. Capturing our own output is the one thing
/// the feature must never do - see docs/09-screen-audio.md, section
/// 2 - and leaving it out of the menu is the cheapest place to enforce that.
///
/// Note on browsers: Chrome and Edge render from a child process rather than
/// from the window you can see, so what appears here is that child. Targeting
/// it is correct - it is where the sound is - and the capture includes the
/// process tree below it anyway.
///
/// Fails with `capture_unavailable` on a platform or a build that cannot
/// enumerate them.
[[nodiscard]] Result<std::vector<AudioSource>> audio_sources();

}  // namespace dv::client::audio
