#include "ui/components/toast.hpp"

#include <algorithm>

#include <QColor>
#include <QEasingCurve>
#include <QEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPoint>
#include <QPropertyAnimation>
#include <QRectF>
#include <QShowEvent>
#include <QTimer>
#include <QVariant>
#include <QVariantAnimation>

#include "ui/components/icons.hpp"
#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The card: how wide it may get before the sentence wraps, the padding round
/// the content, the icon, and the room left round the card for its shadow.
constexpr int kMaxTextWidth = 300;
constexpr int kPadding = 12;
constexpr int kIconSide = 18;
constexpr int kIconGap = 10;
constexpr int kShadowRoom = 12;

/// Between two toasts in the stack, on top of the shadow room each brings.
constexpr int kStackGap = -8;

/// How many are on screen at once. A fourth sends the oldest away: a column
/// of notifications down the side of a call is a second chat nobody asked
/// for.
constexpr int kMaxVisible = 3;

/// How far a toast travels as it arrives, in pixels, from above.
constexpr int kSlide = 14;

[[nodiscard]] QColor colour_of(ToastKind kind) {
  const theme::Colors& colours = theme::colors();
  switch (kind) {
    case ToastKind::Success:
      return colours.success;
    case ToastKind::Warning:
      return colours.warn;
    case ToastKind::Error:
      return colours.danger;
    case ToastKind::Info:
    case ToastKind::Moderation:
      // The colour of plain news. Being muted is bad news and being unmuted
      // is good, and the kind covers both; the shield is what says whose
      // doing it was.
      break;
  }
  return colours.info;
}

[[nodiscard]] Icon icon_of(ToastKind kind) {
  switch (kind) {
    case ToastKind::Success:
      return Icon::Check;
    case ToastKind::Warning:
      return Icon::Warning;
    case ToastKind::Error:
      return Icon::Error;
    case ToastKind::Moderation:
      return Icon::Shield;
    case ToastKind::Info:
      break;
  }
  return Icon::Info;
}

}  // namespace

int toast_lifetime_ms(ToastKind kind, const QString& text, bool over_picture) {
  // The time it takes to notice a notification and read it: a floor, so that
  // two words are not gone before the eye has arrived, and so much per
  // character on top. What everything that is merely news stays for.
  constexpr int kFloorMs = 2500;
  constexpr int kPerCharacterMs = 55;
  constexpr int kUntilDismissed = 0;
  const int reading = kFloorMs + (static_cast<int>(text.size()) * kPerCharacterMs);

  switch (kind) {
    case ToastKind::Moderation:
      // Waits to be dismissed, over a picture too. An administrator muting or
      // restricting somebody changes what a person in the room can do, and it
      // can happen while nobody is looking at this window: gone in four
      // seconds, it leaves somebody wondering why a voice stopped. It is the
      // one kind that is not allowed to pass unseen.
      return kUntilDismissed;
    case ToastKind::Error:
      // Waits to be dismissed on a page, because it is the answer to
      // something the person just did and they may have looked away while it
      // was being worked out. Not over a picture: nobody watching a film
      // reaches for the mouse to send a notification away, so one that waits
      // there sits on the film for the rest of it. Twice the reading time
      // instead - long enough for eyes that were on the picture to get to it.
      return over_picture ? reading * 2 : kUntilDismissed;
    case ToastKind::Info:
    case ToastKind::Success:
    case ToastKind::Warning:
      break;
  }
  return reading;
}

Toast::Toast(ToastKind kind, const QString& text, bool over_picture, QWidget* parent)
    : QWidget(parent),
      kind_(kind),
      text_(text),
      over_picture_(over_picture),
      fade_(new QVariantAnimation(this)),
      lifetime_(new QTimer(this)) {
  setFont(theme::font(theme::TextRole::Body));
  setCursor(Qt::PointingHandCursor);
  // Reachable by Tab and not by a click, which already dismisses it: the
  // focus is the keyboard's way to the same thing. See keyPressEvent.
  setFocusPolicy(Qt::TabFocus);
  setToolTip(QStringLiteral("Click to dismiss"));
  // For a screen reader this is the sentence itself.
  setAccessibleName(text);

  const QFontMetrics metrics(font());
  const QRect wrapped =
      metrics.boundingRect(QRect(0, 0, kMaxTextWidth, 1000), Qt::TextWordWrap, text_);
  const int content_height = std::max(kIconSide, wrapped.height());
  setFixedSize(
      kShadowRoom + kPadding + kIconSide + kIconGap + wrapped.width() + kPadding + kShadowRoom,
      kShadowRoom + kPadding + content_height + kPadding + kShadowRoom);

  fade_->setEasingCurve(QEasingCurve::OutCubic);
  connect(fade_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    presence_ = value.toReal();
    update();
  });
  connect(fade_, &QVariantAnimation::finished, this, [this] {
    if (leaving_) {
      emit dismissed(this);
      deleteLater();
    }
  });

  lifetime_->setSingleShot(true);
  connect(lifetime_, &QTimer::timeout, this, &Toast::dismiss);
}

void Toast::dismiss() {
  if (leaving_) {
    return;
  }
  leaving_ = true;
  lifetime_->stop();
  fade_->stop();
  const int duration = theme::motion::duration(theme::motion::kBaseMs);
  if (duration == 0) {
    emit dismissed(this);
    deleteLater();
    return;
  }
  // Leaving accelerates, where arriving slowed down: the two read as the
  // same movement run backwards.
  fade_->setEasingCurve(QEasingCurve::InCubic);
  fade_->setDuration(duration);
  fade_->setStartValue(presence_);
  fade_->setEndValue(0.0);
  fade_->start();
}

void Toast::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setOpacity(presence_);

  const theme::Colors& colours = theme::colors();
  const QRectF card = QRectF(rect()).adjusted(kShadowRoom, kShadowRoom, -kShadowRoom, -kShadowRoom);
  theme::paint_shadow(painter, card, theme::kControlRadius);

  // The accent, and twice the weight, while the keyboard is on it.
  painter.setPen(hasFocus() ? QPen(colours.accent, 2.0) : QPen(colours.border_strong, 1.0));
  painter.setBrush(colours.surface_raised);
  painter.drawRoundedRect(card.adjusted(0.5, 0.5, -0.5, -0.5), theme::kControlRadius,
                          theme::kControlRadius);

  // The colour and the icon say the same thing twice, on purpose: somebody
  // who cannot tell the red from the green still has a cross and a tick.
  const QRectF icon_box(card.left() + kPadding, card.top() + kPadding, kIconSide, kIconSide);
  paint_icon(painter, icon_of(kind_), icon_box, colour_of(kind_));

  painter.setPen(colours.text);
  painter.setFont(font());
  const QRectF text_box(icon_box.right() + kIconGap, card.top() + kPadding,
                        card.right() - kPadding - icon_box.right() - kIconGap,
                        card.height() - (2 * kPadding));
  painter.drawText(text_box, Qt::TextWordWrap | Qt::AlignLeft | Qt::AlignVCenter, text_);
}

void Toast::mousePressEvent(QMouseEvent* event) {
  dismiss();
  event->accept();
}

void Toast::keyPressEvent(QKeyEvent* event) {
  const int key = event->key();
  if (key == Qt::Key_Escape || key == Qt::Key_Space || key == Qt::Key_Return ||
      key == Qt::Key_Enter) {
    dismiss();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

void Toast::showEvent(QShowEvent* event) {
  QWidget::showEvent(event);
  if (leaving_ || presence_ > 0.0) {
    return;
  }
  const int duration = theme::motion::duration(theme::motion::kBaseMs);
  if (duration == 0) {
    presence_ = 1.0;
  } else {
    fade_->setDuration(duration);
    fade_->setStartValue(0.0);
    fade_->setEndValue(1.0);
    fade_->start();
  }
  if (const int lifetime = toast_lifetime_ms(kind_, text_, over_picture_); lifetime > 0) {
    lifetime_->start(lifetime);
  }
}

ToastHost::ToastHost(QWidget* container) : QObject(container), container_(container) {
  // To follow the corner as the window is resized.
  container_->installEventFilter(this);
}

void ToastHost::set_margins(const QMargins& margins) {
  margins_ = margins;
  arrange(false);
}

void ToastHost::show_toast(ToastKind kind, const QString& text, bool over_picture) {
  if (text.isEmpty()) {
    return;
  }
  // The same sentence already on screen is not said again beside itself: a
  // refusal repeated by an impatient click would otherwise fill the stack
  // with one message three times.
  for (const Toast* shown : toasts_) {
    if (shown->accessibleName() == text) {
      return;
    }
  }

  auto* toast = new Toast(kind, text, over_picture, container_);
  connect(toast, &Toast::dismissed, this, [this](Toast* gone) {
    toasts_.removeAll(gone);
    arrange(true);
  });
  toasts_.append(toast);
  while (toasts_.size() > kMaxVisible) {
    // Out of the list now, so the stack closes up at once; the widget itself
    // goes when its fade has finished.
    Toast* oldest = toasts_.takeFirst();
    oldest->dismiss();
  }

  arrange(false);
  // Starting a little above where it will rest, and coming down into place:
  // the direction a thing arrives from when it is laid on top of a page.
  const QPoint rest = toast->pos();
  const int duration = theme::motion::duration(theme::motion::kBaseMs);
  toast->show();
  toast->raise();
  if (duration > 0) {
    auto* slide = new QPropertyAnimation(toast, "pos", toast);
    slide->setDuration(duration);
    slide->setEasingCurve(QEasingCurve::OutCubic);
    slide->setStartValue(rest - QPoint(0, kSlide));
    slide->setEndValue(rest);
    slide->start(QAbstractAnimation::DeleteWhenStopped);
  }
}

void ToastHost::bring_to_front() {
  for (Toast* toast : toasts_) {
    toast->raise();
    toast->update();
  }
}

void ToastHost::dismiss_all() {
  // A copy, and the list emptied first: each dismissal comes back through
  // the `dismissed` signal to take itself out of the list it is being read
  // from.
  const QList<Toast*> leaving = toasts_;
  toasts_.clear();
  for (Toast* toast : leaving) {
    toast->dismiss();
  }
}

bool ToastHost::eventFilter(QObject* watched, QEvent* event) {
  if (watched == container_ && event->type() == QEvent::Resize) {
    arrange(false);
  }
  return QObject::eventFilter(watched, event);
}

void ToastHost::arrange(bool animated) {
  int top = margins_.top();
  for (Toast* toast : toasts_) {
    // Centred across what is left between the side margins. The corner is
    // where a notification usually goes, and in this program the corner is
    // where the tabs of the room's side panel are: a notification there sat
    // on the two things most likely to be clicked next.
    const int span = container_->width() - margins_.left() - margins_.right();
    const QPoint target(margins_.left() + ((span - toast->width()) / 2), top);
    top += toast->height() + kStackGap;
    const int duration = animated ? theme::motion::duration(theme::motion::kBaseMs) : 0;
    if (duration == 0 || !toast->isVisible()) {
      toast->move(target);
      continue;
    }
    auto* shift = new QPropertyAnimation(toast, "pos", toast);
    shift->setDuration(duration);
    shift->setEasingCurve(QEasingCurve::OutCubic);
    shift->setEndValue(target);
    shift->start(QAbstractAnimation::DeleteWhenStopped);
  }
}

}  // namespace dv::ui
