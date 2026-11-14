// The Windows half of theme::platform_set_title_bar.
//
// The stub that takes its place on every other platform lives in
// client/src/ui/theme_stub.cpp.

#include <QWidget>

#include "ui/theme.hpp"

// The Windows headers define min and max as macros unless told not to, and
// the rest of this program uses std::min.
#ifndef NOMINMAX
#define NOMINMAX
#endif
// In this order and kept in it by hand: dwmapi.h uses types windows.h
// declares and does not include it. Sorted alphabetically, which is what the
// formatter does to a block of includes, it is the first of the two and the
// file does not compile.
// clang-format off
#include <windows.h>
#include <dwmapi.h>
// clang-format on

namespace dv::ui::theme {

void platform_set_title_bar(QWidget* window, bool dark) {
  // Only a window the system already knows about. winId() on a widget that
  // has no native window yet creates one, and creating it here - from inside
  // the event that is in the middle of showing it - is not this function's
  // business.
  if (window == nullptr || !window->testAttribute(Qt::WA_WState_Created) ||
      window->internalWinId() == 0) {
    return;
  }

  // DWMWA_USE_IMMERSIVE_DARK_MODE. Twenty since Windows 10 20H1, which is
  // older than anything Qt 6 runs on; an older system answers with an error
  // and keeps its title bar, which is the right outcome and is why the result
  // is not looked at.
  //
  // Qt sets this itself, from the desktop's scheme. This is for the case that
  // does not cover: somebody who chose Dark in Settings on a light desktop,
  // and would otherwise get dark colours under a white title bar.
  constexpr DWORD kUseImmersiveDarkMode = 20;
  const BOOL value = dark ? TRUE : FALSE;
  // The handle is an integer in Qt's API and a pointer in the system's.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast,performance-no-int-to-ptr)
  auto* const handle = reinterpret_cast<HWND>(window->internalWinId());
  (void)DwmSetWindowAttribute(handle, kUseImmersiveDarkMode, &value, sizeof(value));
}

}  // namespace dv::ui::theme
