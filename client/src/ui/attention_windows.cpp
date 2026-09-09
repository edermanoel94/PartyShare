// The Windows half of ui::flash_window.
//
// The stub that takes its place on every other platform lives in
// client/src/ui/attention_stub.cpp.
//
// FlashWindowEx and not QApplication::alert, which on Windows wraps the same
// call with a different request. alert asks for the taskbar button alone
// (FLASHW_TRAY), which is the right amount of noise for "somebody joined the
// room". A nudge is somebody asking for you by name, and this asks for the
// title bar as well and for the flashing to go on until the window comes to
// the front (FLASHW_TIMERNOFG) - which is the difference between a light that
// went on and a light that stays on until you look.

#include "ui/attention.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <cstdint>

#include <windows.h>

namespace dv::ui {

bool flash_window(std::uintptr_t window) {
  if (window == 0) {
    return false;
  }

  FLASHWINFO info{};
  info.cbSize = sizeof(info);
  // WId is the HWND on Windows, carried across as an integer so that the
  // header names no Qt type. The check this silences is about arithmetic on
  // integers that used to be pointers; this one was a pointer a moment ago and
  // is going back to being one.
  // NOLINTNEXTLINE(performance-no-int-to-ptr)
  info.hwnd = reinterpret_cast<HWND>(window);
  info.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG;
  // Both zero: no fixed count and the system's own blink rate. With
  // FLASHW_TIMERNOFG the count is not what stops it; the window coming to the
  // foreground is.
  info.uCount = 0;
  info.dwTimeout = 0;

  // The return value is whether the window was already active before the call,
  // which says nothing about whether the request was taken. It was: the only
  // way this call fails is a handle that is not a window, and the one passed
  // in is this process's own.
  (void)::FlashWindowEx(&info);
  return true;
}

}  // namespace dv::ui
