// The non-Windows half of ui::play_wav.
//
// It says no, and the caller falls back to QApplication::beep. That is a worse
// cue than the chime - it is one sound for two events, so it cannot say which
// of arriving and leaving happened - and it is the honest state of things
// until somebody writes the ALSA and CoreAudio halves. See
// client/src/ui/sound.hpp for why Qt Multimedia is not the answer.

#include <cstddef>

#include "ui/sound.hpp"

namespace dv::ui {

bool play_wav(const void* /*wav*/, std::size_t /*size*/) {
  return false;
}

}  // namespace dv::ui
