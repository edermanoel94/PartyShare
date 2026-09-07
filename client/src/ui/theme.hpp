#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace dv::ui::theme {

/// Every colour the interface is allowed to use.
///
/// One struct with two fillings, light and dark, rather than colours written
/// where they are drawn. The reason is the bug this replaced: the login error
/// was a literal #c62828 and the metrics line was palette(mid), so half the
/// interface followed the system's colour scheme and half of it did not, and
/// nobody could tell by reading any one file.
struct Colors {
  /// Behind everything.
  QColor window;
  /// Cards, group boxes, fields and lists: the paper the content sits on.
  QColor surface;
  QColor surface_hover;
  QColor surface_pressed;
  QColor border;
  /// The border of something under the pointer, which is how a control says it
  /// can be clicked before it is.
  QColor border_strong;

  QColor text;
  /// Secondary text. Timestamps, hints, the metrics line.
  QColor muted;

  /// The brand indigo of assets/partyshare.svg. The interface and the icon on
  /// the dock are the same program and have no business being different
  /// colours.
  QColor accent;
  QColor accent_hover;
  QColor accent_pressed;
  /// A wash of the accent, for a control that is on rather than one that is
  /// the thing to press.
  QColor accent_soft;
  /// Text and icons drawn on top of `accent`.
  QColor on_accent;

  QColor danger;
  QColor danger_soft;
  QColor warn;
  QColor warn_soft;
  QColor success;

  /// Behind a shared screen, and the text shown when there is not one. Nearly
  /// black in both schemes: a picture is judged against what surrounds it, and
  /// a light grey surround makes every screen share look washed out.
  QColor canvas;
  QColor canvas_text;
};

/// The colours in force, which follow the system's light or dark setting.
[[nodiscard]] const Colors& colors();

/// How round everything is, in pixels. Shared with the widgets that draw
/// themselves, so a corner painted by hand matches one drawn by the stylesheet.
inline constexpr int kCardRadius = 14;
inline constexpr int kControlRadius = 10;

/// Installs the palette and the stylesheet, and keeps them following the
/// system's colour scheme for as long as the application runs.
///
/// Called once, before the first window is built.
void apply(QApplication& application);

/// The stylesheet for `colors()`. Exposed for whoever wants to look at it.
[[nodiscard]] QString stylesheet();

}  // namespace dv::ui::theme
