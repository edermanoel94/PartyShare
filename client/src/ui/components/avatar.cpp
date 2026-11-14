#include "ui/components/avatar.hpp"

#include <array>

#include <QColor>
#include <QFocusEvent>
#include <QFont>
#include <QHash>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>

#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The hues a badge can be. Eight, spread round the wheel and all at the same
/// saturation and lightness, so that no person's badge is louder than
/// another's and two neighbours in a list are rarely the same.
constexpr std::array<int, 8> kHues = {242, 168, 24, 330, 205, 95, 280, 45};

/// The room the ring takes round the badge, when there is one.
constexpr int kRingMargin = 3;

}  // namespace

AvatarColours avatar_colours(const QString& name) {
  const int hue = kHues.at(qHash(name) % kHues.size());
  // A dark tint with light writing in the dark scheme and a pale one with
  // dark writing in the light scheme: the badge sits on a surface in both,
  // and a saturated disc on either would be the loudest thing in the list.
  if (theme::is_dark()) {
    return {.ground = QColor::fromHsl(hue, 90, 84), .ink = QColor::fromHsl(hue, 150, 222)};
  }
  return {.ground = QColor::fromHsl(hue, 140, 226), .ink = QColor::fromHsl(hue, 140, 82)};
}

void paint_avatar(QPainter& painter, const QRectF& box, const QString& name) {
  const AvatarColours colours = avatar_colours(name);
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  painter.setBrush(colours.ground);
  painter.drawEllipse(box);

  // One letter. A name can begin with a character outside the basic plane,
  // which is two QChars, and half of one is a question mark in a box.
  const QString trimmed = name.trimmed();
  QString initial = trimmed.left(trimmed.size() > 1 && trimmed.at(0).isHighSurrogate() ? 2 : 1);
  initial = initial.toUpper();

  QFont font = painter.font();
  font.setPixelSize(static_cast<int>(box.height() * 0.44));
  font.setWeight(QFont::DemiBold);
  painter.setFont(font);
  painter.setPen(colours.ink);
  painter.drawText(box, Qt::AlignCenter, initial);
  painter.restore();
}

Avatar::Avatar(int diameter, QWidget* parent) : QWidget(parent), diameter_(diameter) {
  const int side = diameter_ + (2 * kRingMargin);
  setFixedSize(side, side);
}

void Avatar::set_name(const QString& name) {
  // Nothing to do, and nothing done, when it is the name already shown. The
  // room hands every badge its name again several times a second - the list
  // of participants carries the levels - and a repaint for each would be a
  // row of badges redrawn all call long to stay exactly as they were.
  if (name_ == name) {
    return;
  }
  name_ = name;
  setToolTip(name);
  update();
}

void Avatar::set_ring(bool on) {
  if (ring_ == on) {
    return;
  }
  ring_ = on;
  update();
}

void Avatar::set_interactive(bool on) {
  interactive_ = on;
  setFocusPolicy(on ? Qt::StrongFocus : Qt::NoFocus);
  setCursor(on ? Qt::PointingHandCursor : Qt::ArrowCursor);
}

void Avatar::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  const QRectF badge(kRingMargin, kRingMargin, diameter_, diameter_);
  paint_avatar(painter, badge, name_);
  // One ring, and the focus has it when both apply: where the keyboard is
  // matters more to the person at it than who is talking.
  const bool focused = interactive_ && hasFocus() && focus_by_keyboard_;
  if (ring_ || focused) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(focused ? theme::colors().accent : theme::colors().success, 2.0));
    painter.drawEllipse(badge.adjusted(-2, -2, 2, 2));
  }
}

void Avatar::focusInEvent(QFocusEvent* event) {
  const Qt::FocusReason reason = event->reason();
  // A window or a menu handing the focus back leaves the answer as it was.
  if (reason != Qt::ActiveWindowFocusReason && reason != Qt::PopupFocusReason) {
    focus_by_keyboard_ = reason == Qt::TabFocusReason || reason == Qt::BacktabFocusReason ||
                         reason == Qt::ShortcutFocusReason;
  }
  QWidget::focusInEvent(event);
}

void Avatar::mouseReleaseEvent(QMouseEvent* event) {
  if (interactive_ && event->button() == Qt::LeftButton && rect().contains(event->pos())) {
    emit clicked();
    event->accept();
    return;
  }
  QWidget::mouseReleaseEvent(event);
}

void Avatar::keyPressEvent(QKeyEvent* event) {
  const int key = event->key();
  if (interactive_ && (key == Qt::Key_Space || key == Qt::Key_Return || key == Qt::Key_Enter)) {
    emit clicked();
    event->accept();
    return;
  }
  QWidget::keyPressEvent(event);
}

}  // namespace dv::ui
