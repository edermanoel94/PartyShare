#pragma once

#include <cstddef>

namespace dv::ui {

/// Plays a WAV held in memory and returns immediately, without waiting for it
/// to finish. Returns whether the platform took it.
///
/// The one place in this client where Qt's own answer was turned down.
/// QSoundEffect is what Qt offers, it lives in Qt Multimedia, and Qt
/// Multimedia on Windows plays through the FFmpeg backend - so two chimes of a
/// fifth of a second each would put a module and its codec plugins into the
/// build and into the installer. The Qt installed on the build machine does
/// not even carry the module.
///
/// The platform underneath is five lines instead. See
/// client/src/ui/sound_windows.cpp, and docs/10-join-leave-alerts.md for
/// how this sits next to the notification, which is Qt's all the way through.
///
/// `wav` must stay alive and unchanged until the sound has finished playing.
/// The call is asynchronous and does not copy: on Windows it hands the buffer
/// straight to PlaySound, which reads from it on its own thread. Callers keep
/// their buffers for the life of the process, which is the only lifetime that
/// is obviously long enough.
[[nodiscard]] bool play_wav(const void* wav, std::size_t size);

}  // namespace dv::ui
