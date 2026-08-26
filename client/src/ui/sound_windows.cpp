// The Windows half of ui::play_wav.
//
// The stub that takes its place on every other platform lives in
// client/src/ui/sound_stub.cpp.
//
// PlaySound and not the audio stack the rest of the client already has. That
// stack is libwebrtc's, it is optional at build time - DV_BUILD_CLIENT_MEDIA
// is off by default - and it belongs to the call: mixing a chime into the
// stream that carries voices would send it to the room instead of playing it
// here. A cue about the room is not part of the room.
//
// One voice, and that is on purpose rather than a limitation worked around:
// PlaySound stops whatever it was playing when it is called again, so a burst
// of arrivals is one sound rather than a pile of them.

#include "ui/sound.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstddef>

#include <windows.h>
// After windows.h, which declares what it needs. clang-format sorts this block
// alphabetically and that happens to be the right order; keep it that way.
#include <mmsystem.h>

namespace dv::ui {

bool play_wav(const void* wav, std::size_t size) {
  if (wav == nullptr || size == 0) {
    return false;
  }

  // SND_MEMORY, because the sound is compiled into the executable rather than
  // installed beside it, and a resource written to a temporary file so that it
  // could be named in a path is a temporary file to clean up.
  //
  // SND_NODEFAULT matters more than it looks: without it, a buffer PlaySound
  // cannot parse is answered with the machine's default beep, which would turn
  // a broken asset into a sound that seems to work.
  //
  // The cast is what the API wants. PlaySound takes a path, an alias or a
  // buffer through the same parameter and picks by flag.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  return ::PlaySoundW(static_cast<LPCWSTR>(wav), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT) !=
         FALSE;
}

}  // namespace dv::ui
