#include "ui/notifier.hpp"

#include <memory>

#include <QApplication>
#include <QIcon>
#include <QString>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QWidget>

namespace dv::ui {
namespace {

/// How long the balloon asks to stay up.
///
/// A hint and nothing more. Qt's documentation is explicit that Windows
/// usually ignores it while the application has focus, and the notification
/// area has its own idea of the right duration on every platform.
constexpr int kBalloonMilliseconds = 6000;

/// What the tray icon shows.
///
/// The application's, when there is one - main() installs it from the
/// resources, and it is the same drawing as the one on the taskbar. The
/// style's information icon is the answer for a build that somehow has none
/// rather than a reason to leave a blank square in the notification area.
[[nodiscard]] QIcon tray_icon(const QWidget* window) {
  QIcon icon = window != nullptr ? window->windowIcon() : QIcon();
  if (icon.isNull()) {
    icon = QApplication::windowIcon();
  }
  if (icon.isNull()) {
    icon = QApplication::style()->standardIcon(QStyle::SP_MessageBoxInformation);
  }
  return icon;
}

}  // namespace

Notifier::Notifier(QWidget* window) : window_(window) {
  // A machine with no notification area at all - a bare X11 session, a locked
  // down desktop - gets no tray icon and no balloon, and the taskbar flash in
  // notify() carries what is left on its own.
  if (!QSystemTrayIcon::isSystemTrayAvailable()) {
    return;
  }
  tray_ = std::make_unique<QSystemTrayIcon>(tray_icon(window_));
  tray_->setToolTip(QStringLiteral("PartyShare"));
  tray_->show();
}

// Out of line so that the header can forward declare QSystemTrayIcon: a
// unique_ptr's deleter has to see the complete type, and it is instantiated
// wherever the destructor is.
Notifier::~Notifier() = default;

bool Notifier::window_has_attention() const {
  if (window_ == nullptr) {
    return false;
  }
  // isActiveWindow is already false for a minimised window and for one behind
  // another application, which is the whole of the question. isVisible is the
  // case it does not cover on its own: a window hidden outright.
  return window_->isVisible() && window_->isActiveWindow();
}

bool Notifier::notify(const QString& title, const QString& body) {
  if (window_ != nullptr) {
    // Zero means until the window is touched, rather than for a fixed time.
    // Somebody who stepped away should still find the button lit when they get
    // back, which is the only part of this that survives being away for ten
    // minutes.
    QApplication::alert(window_, 0);
  }

  // supportsMessages and not only a null check: the tray can exist on a
  // desktop whose implementation shows icons and drops balloons, and Qt
  // answers that question separately from whether there is a tray at all.
  if (!tray_ || !QSystemTrayIcon::supportsMessages()) {
    return false;
  }
  tray_->showMessage(title, body, QSystemTrayIcon::Information, kBalloonMilliseconds);
  return true;
}

}  // namespace dv::ui
