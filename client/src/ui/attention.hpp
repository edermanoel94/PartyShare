#pragma once

#include <cstdint>

namespace dv::ui {

/// Lights the program up the way the operating system does when a window
/// wants somebody: on Windows the taskbar button and the title bar flash until
/// the window is brought to the front. Returns whether the platform did it,
/// and the caller falls back to QApplication::alert when it did not.
///
/// This is the nudge's half of the operating system, and it is native on
/// purpose where the arrival balloon is not. docs/10-join-leave-alerts.md
/// records why the native toast was taken out: it needed a Start menu shortcut
/// and five libraries to say "somebody joined", which a balloon says as well.
/// Flashing a window needs one call that has been in every Windows since 98,
/// and it says something the balloon cannot: not "something happened" but
/// "somebody wants you, and this will not stop until you look".
///
/// `window` is the platform's own handle for the top-level window -
/// QWidget::winId - and is an integer here so that this header, like
/// ui/sound.hpp, names no Qt type and the platform halves need none.
[[nodiscard]] bool flash_window(std::uintptr_t window);

}  // namespace dv::ui
