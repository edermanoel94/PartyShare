#pragma once

#include <cstdint>

#include <QList>
#include <QMargins>
#include <QObject>
#include <QString>
#include <QWidget>

class QEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QShowEvent;
class QTimer;
class QVariantAnimation;

namespace dv::ui {

/// What a notification is about, which decides its icon and its colour - and
/// how long it stays: see toast_lifetime_ms.
enum class ToastKind : std::uint8_t {
  Info,
  Success,
  Warning,
  Error,
  /// Somebody with authority over the room did something to somebody in it:
  /// muted them, restricted them, lifted either. Its own kind, and not an
  /// Info, because it is the one piece of news that waits to be acknowledged.
  Moderation,
};

/// How long a notification stays on screen before it leaves by itself, in
/// milliseconds. Zero means it stays until it is dismissed.
///
/// `over_picture` says that what the notification will sit on is something
/// being watched - a shared screen - rather than a page. The caller knows
/// that; a notification does not.
///
/// Its own function because it is a decision about people and not about
/// drawing. The rules, and the reason for each, are where it is defined.
[[nodiscard]] int toast_lifetime_ms(ToastKind kind, const QString& text, bool over_picture);

/// One notification: a small raised card with an icon and a sentence, which
/// slides in, waits, and fades out. Clicking it sends it away at once, and so
/// does Esc, Space or Enter while it has the focus - some of these wait to be
/// dismissed, and a thing only a mouse can dismiss is a thing a keyboard is
/// stuck with.
///
/// Painted whole, text included, so that fading it is one opacity on one
/// painter rather than a graphics effect - see theme::paint_shadow for why
/// this program avoids those over a shared screen.
class Toast : public QWidget {
  Q_OBJECT

 public:
  Toast(ToastKind kind, const QString& text, bool over_picture, QWidget* parent);

  /// Fades out and deletes itself. Safe to call twice.
  void dismiss();

 signals:
  /// Gone from the screen and about to be deleted; the host closes the gap.
  void dismissed(dv::ui::Toast* toast);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mousePressEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void showEvent(QShowEvent* event) override;

 private:
  ToastKind kind_;
  QString text_;
  /// See toast_lifetime_ms.
  bool over_picture_;
  /// 0 invisible, 1 fully there.
  qreal presence_ = 0.0;
  bool leaving_ = false;
  QVariantAnimation* fade_ = nullptr;
  QTimer* lifetime_ = nullptr;
};

/// The notification area of a window: stacks toasts down from the top of
/// `container`, centred, and keeps them there as it is resized.
///
/// A QObject and not a widget. It owns no pixels of its own - a transparent
/// widget covering the top of the page would swallow clicks meant for what is
/// under it - only the toasts, which are children of the container.
class ToastHost : public QObject {
  Q_OBJECT

 public:
  explicit ToastHost(QWidget* container);

  /// How far in from the container's edges the stack keeps: the height of a
  /// header on top, say, so a notification never covers the page's title.
  void set_margins(const QMargins& margins);

  /// Puts a notification up. The same sentence already on screen is not said
  /// a second time beside itself, and a fourth sends the oldest away.
  ///
  /// `over_picture` is whether the container is showing something being
  /// watched right now, which changes how long some kinds stay: see
  /// toast_lifetime_ms.
  void show_toast(ToastKind kind, const QString& text, bool over_picture = false);

  /// Puts the notifications still up back on top of whatever the container
  /// is showing now, and has them drawn again. For a container that swaps
  /// what it shows - a stack of pages - to call when it does: a notification
  /// that waits to be dismissed outlives the page it was raised over, and the
  /// page that replaces it is drawn after it and over it.
  void bring_to_front();

  /// Sends every notification away at once, the ones that wait to be
  /// dismissed included. For when what they were about is over and whoever
  /// reads them next may not be who they were for: signing out.
  void dismiss_all();

 protected:
  bool eventFilter(QObject* watched, QEvent* event) override;

 private:
  /// Puts every toast where it belongs, newest at the bottom of the stack.
  void arrange(bool animated);

  QWidget* container_;
  QMargins margins_;
  QList<Toast*> toasts_;
};

}  // namespace dv::ui
