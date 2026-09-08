// The non-Windows half of ui::flash_window.
//
// It says no, and the caller falls back to QApplication::alert, which is Qt's
// portable version of the same idea: it bounces the Dock icon on macOS and
// asks the window manager for attention on X11. That is a fair cue and the
// honest state of things until somebody writes the platform halves -
// NSApplication requestUserAttention: with NSCriticalRequest, which bounces
// until the application is activated, and _NET_WM_STATE_DEMANDS_ATTENTION or
// its Wayland equivalent, xdg_activation_v1. The shake and the sound that go
// with a nudge are not in here: the shake is Qt moving its own window and the
// sound is ui::play_wav, each with its own platform story.

#include <cstdint>

#include "ui/attention.hpp"

namespace dv::ui {

bool flash_window(std::uintptr_t /*window*/) {
  return false;
}

}  // namespace dv::ui
