#pragma once

#include <memory>

#include <QString>

class QSystemTrayIcon;
class QWidget;

namespace dv::ui {

/// Says that something happened in a room the person is not looking at.
///
/// Two things, and only the first of them is a notification:
///
///   1. QSystemTrayIcon::showMessage, the balloon in the notification area;
///   2. QApplication::alert, which lights the taskbar button.
///
/// Both are Qt's own, portable, and cost nothing to deploy. What that buys and
/// what it gives up is written down in docs/avisos-de-entrada-e-saida.md, because
/// the alternative was built and then deliberately removed and the next person
/// to wonder deserves the reasoning rather than the archaeology. The short
/// version: Qt's documentation warns that a balloon "may not appear at all",
/// so the taskbar flash is the half that can be relied on.
///
/// The two are raised together and neither is allowed to fail anything. A
/// notification that did not arrive is not worth interrupting a call over.
class Notifier {
 public:
  /// `window` is the one whose taskbar button flashes, and whose focus decides
  /// whether there is anything to say at all. It has to outlive the notifier.
  explicit Notifier(QWidget* window);
  ~Notifier();

  Notifier(const Notifier&) = delete;
  Notifier& operator=(const Notifier&) = delete;
  Notifier(Notifier&&) = delete;
  Notifier& operator=(Notifier&&) = delete;

  /// Whether the person is already looking at this window.
  ///
  /// Nothing needs to be raised when they are: a participant list that just
  /// grew a row said it, and a balloon over the top of a window somebody is
  /// reading is a notification about something they have already seen.
  [[nodiscard]] bool window_has_attention() const;

  /// Raises the notification, and says whether a balloon actually went up.
  ///
  /// Plain text, both arguments: a balloon takes nothing else.
  ///
  /// The return value exists because the balloon is not silent and cannot be
  /// made to be. On Windows the shell turns it into a toast and plays the
  /// system notification sound; QSystemTrayIcon::showMessage has no parameter
  /// to ask otherwise, where the native API this replaced took
  /// `<audio silent="true"/>`. So the caller uses this to decide whether to
  /// play its own chime as well, and one arrival makes one sound.
  ///
  /// False covers a desktop with no notification area, one whose tray shows
  /// icons and drops balloons, and this window having the focus - handled by
  /// the caller, since only it knows the difference between nothing to say and
  /// nowhere to say it. It is not a promise that a balloon that was raised was
  /// also seen: Qt's own documentation warns that one "may not appear at all",
  /// which is why the taskbar flash below happens either way.
  bool notify(const QString& title, const QString& body);

 private:
  QWidget* window_ = nullptr;
  /// Built once, at construction, and kept for the life of the window.
  ///
  /// It used to be built on the first balloon, back when the native path was
  /// there to be tried first and an icon in the notification area was a cost
  /// most machines would never pay. Now that the balloon is the only channel,
  /// the icon is what the balloon hangs off - and adding it and asking it to
  /// speak in the same breath is the kind of ordering that works on the
  /// machine it was written on.
  std::unique_ptr<QSystemTrayIcon> tray_;
};

}  // namespace dv::ui
