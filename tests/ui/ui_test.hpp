#pragma once

#include <algorithm>
#include <cmath>

#include <QColor>
#include <QCoreApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPointF>
#include <QWidget>

#include "ui/theme.hpp"

/// What the interface tests share: a way to press things without Qt Test,
/// which the build does not otherwise need, and the contrast arithmetic.
namespace dv::ui::testing {

/// Runs what was posted, deferred deletions included. A widget that says it
/// is gone with deleteLater is still a child of its parent until this.
inline void settle() {
  QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
  QCoreApplication::processEvents();
}

/// A left click in the middle of `widget`, delivered the way the window
/// system would deliver it.
inline void click(QWidget* widget) {
  const QPointF middle = QPointF(widget->rect().center());
  const QPointF global = QPointF(widget->mapToGlobal(widget->rect().center()));
  QMouseEvent press(QEvent::MouseButtonPress, middle, global, Qt::LeftButton, Qt::LeftButton,
                    Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &press);
  QMouseEvent release(QEvent::MouseButtonRelease, middle, global, Qt::LeftButton, Qt::NoButton,
                      Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &release);
}

/// One key, down and up.
inline void press_key(QWidget* widget, Qt::Key key) {
  QKeyEvent down(QEvent::KeyPress, key, Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &down);
  QKeyEvent up(QEvent::KeyRelease, key, Qt::NoModifier);
  QCoreApplication::sendEvent(widget, &up);
}

/// WCAG relative luminance.
[[nodiscard]] inline double luminance(const QColor& colour) {
  const auto linear = [](double channel) {
    return channel <= 0.03928 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
  };
  return (0.2126 * linear(colour.redF())) + (0.7152 * linear(colour.greenF())) +
         (0.0722 * linear(colour.blueF()));
}

/// WCAG contrast ratio of two colours, whichever way round they are given.
[[nodiscard]] inline double contrast(const QColor& first, const QColor& second) {
  const double a = luminance(first);
  const double b = luminance(second);
  return (std::max(a, b) + 0.05) / (std::min(a, b) + 0.05);
}

/// Puts the theme back the way main() left it when a test that changed it
/// goes out of scope, however it leaves.
class ThemeGuard {
 public:
  ThemeGuard() = default;
  ~ThemeGuard() {
    theme::set_mode(theme::Mode::Dark);
    theme::motion::set_enabled(false);
  }
  ThemeGuard(const ThemeGuard&) = delete;
  ThemeGuard& operator=(const ThemeGuard&) = delete;
  ThemeGuard(ThemeGuard&&) = delete;
  ThemeGuard& operator=(ThemeGuard&&) = delete;
};

}  // namespace dv::ui::testing
