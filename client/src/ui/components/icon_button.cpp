#include "ui/components/icon_button.hpp"

#include <algorithm>

#include <QColor>
#include <QEasingCurve>
#include <QEnterEvent>
#include <QEvent>
#include <QFocusEvent>
#include <QFontMetrics>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QVariant>
#include <QVariantAnimation>

#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The icon's side as a share of the button's height: large enough to read,
/// with enough around it that the button is a target and not a glyph.
constexpr qreal kIconShare = 0.46;
/// Between the icon and the label, and from either to the button's edge.
constexpr int kLabelGap = 8;
constexpr int kLabelPadding = 14;

/// What a variant looks like in one state.
struct Paint {
  QColor rest;
  QColor hover;
  QColor ink;
  QColor ink_hover;
};

[[nodiscard]] Paint paint_for(IconButton::Variant variant, bool checked,
                              IconButton::CheckedTone tone) {
  const theme::Colors& colours = theme::colors();
  if (checked) {
    const bool warn = tone == IconButton::CheckedTone::Warn;
    const QColor wash = warn ? colours.warn_soft : colours.accent_soft;
    const QColor ink = warn ? colours.warn : colours.accent_text;
    return {.rest = wash, .hover = theme::mix(wash, ink, 0.14), .ink = ink, .ink_hover = ink};
  }
  switch (variant) {
    case IconButton::Variant::Tonal:
      return {.rest = colours.surface_pressed,
              .hover = theme::mix(colours.surface_pressed, colours.border_strong, 0.6),
              .ink = colours.text,
              .ink_hover = colours.text};
    case IconButton::Variant::Accent:
      return {.rest = colours.accent,
              .hover = colours.accent_hover,
              .ink = colours.on_accent,
              .ink_hover = colours.on_accent};
    case IconButton::Variant::Danger: {
      const QColor ink = theme::ink_on(colours.danger);
      return {.rest = colours.danger,
              .hover = theme::mix(colours.danger, ink, 0.12),
              .ink = ink,
              .ink_hover = ink};
    }
    case IconButton::Variant::Ghost:
      break;
  }
  QColor clear = colours.surface_hover;
  clear.setAlpha(0);
  return {.rest = clear,
          .hover = colours.surface_hover,
          .ink = colours.muted,
          .ink_hover = colours.text};
}

}  // namespace

IconButton::IconButton(Icon icon, QWidget* parent)
    : QAbstractButton(parent), icon_(icon), hover_animation_(new QVariantAnimation(this)) {
  setCursor(Qt::PointingHandCursor);
  // Reachable by Tab, like every other button: an icon is not an excuse for a
  // control only a mouse can find.
  setFocusPolicy(Qt::StrongFocus);
  setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
  setFont(theme::font(theme::TextRole::Label));

  hover_animation_->setEasingCurve(QEasingCurve::OutCubic);
  connect(hover_animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) {
    hover_ = value.toReal();
    update();
  });
  // The checked state changes the whole drawing, and nothing else asks for a
  // repaint when it is set from code rather than by a click.
  connect(this, &QAbstractButton::toggled, this, [this] { update(); });
}

void IconButton::set_icon(Icon icon) {
  icon_ = icon;
  update();
}

void IconButton::set_checked_icon(Icon icon) {
  checked_icon_ = icon;
  update();
}

void IconButton::set_variant(Variant variant) {
  variant_ = variant;
  update();
}

void IconButton::set_checked_tone(CheckedTone tone) {
  checked_tone_ = tone;
  update();
}

void IconButton::set_label(const QString& label) {
  label_ = label;
  // What a screen reader says. With no label it is whatever was set by hand,
  // and the tooltip stands in for it; see the constructor's callers.
  setAccessibleName(label);
  updateGeometry();
  update();
}

void IconButton::set_extent(int extent) {
  extent_ = extent;
  updateGeometry();
  update();
}

void IconButton::set_corner_radius(int radius) {
  radius_ = radius;
  update();
}

QSize IconButton::sizeHint() const {
  if (label_.isEmpty()) {
    return {extent_, extent_};
  }
  const int icon_side = static_cast<int>(extent_ * kIconShare);
  const int text = QFontMetrics(font()).horizontalAdvance(label_);
  return {kLabelPadding + icon_side + kLabelGap + text + kLabelPadding + 2, extent_};
}

QSize IconButton::minimumSizeHint() const {
  return sizeHint();
}

void IconButton::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const theme::Colors& colours = theme::colors();
  const Paint paint = paint_for(variant_, isChecked(), checked_tone_);
  // Pressed is the hover colour and a little further, so the button answers
  // the click before whatever the click does has had time to.
  const qreal travel = isDown() ? 1.0 : hover_;
  QColor ground = theme::mix(paint.rest, paint.hover, travel);
  QColor ink = theme::mix(paint.ink, paint.ink_hover, travel);
  if (isDown()) {
    ground = theme::mix(ground, ink, 0.08);
  }
  if (!isEnabled()) {
    // Faded rather than recoloured: a disabled Leave is still recognisably
    // the Leave button, which a uniform grey would not be.
    painter.setOpacity(0.42);
  }

  const QRectF box = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
  painter.setPen(Qt::NoPen);
  painter.setBrush(ground);
  painter.drawRoundedRect(box, radius_, radius_);

  const qreal icon_side = extent_ * kIconShare;
  const Icon drawn =
      isChecked() && checked_icon_.has_value() ? checked_icon_.value_or(icon_) : icon_;
  if (label_.isEmpty()) {
    paint_icon(painter, drawn,
               QRectF(box.center().x() - (icon_side / 2), box.center().y() - (icon_side / 2),
                      icon_side, icon_side),
               ink);
  } else {
    const QRectF icon_box(kLabelPadding, box.center().y() - (icon_side / 2), icon_side, icon_side);
    paint_icon(painter, drawn, icon_box, ink);
    painter.setPen(ink);
    painter.setFont(font());
    const QRectF text_box(icon_box.right() + kLabelGap, 0,
                          width() - icon_box.right() - kLabelGap - kLabelPadding, height());
    painter.drawText(text_box, Qt::AlignVCenter | Qt::AlignLeft, label_);
  }

  if (hasFocus() && focus_by_keyboard_ && isEnabled()) {
    // Inside the box, so the ring is not clipped by the widget's own edge,
    // and in a colour that shows against the fill it sits on: the accent
    // disappears on an accent button.
    const bool filled = variant_ == Variant::Accent || variant_ == Variant::Danger;
    painter.setOpacity(1.0);
    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(filled && !isChecked() ? colours.text : colours.accent, 2.0));
    painter.drawRoundedRect(box.adjusted(1, 1, -1, -1), radius_ - 1, radius_ - 1);
  }
}

void IconButton::enterEvent(QEnterEvent* event) {
  animate_hover(true);
  QAbstractButton::enterEvent(event);
}

void IconButton::leaveEvent(QEvent* event) {
  animate_hover(false);
  QAbstractButton::leaveEvent(event);
}

void IconButton::changeEvent(QEvent* event) {
  if (event->type() == QEvent::EnabledChange) {
    // A button disabled under the pointer never gets its leave event, and
    // would come back enabled still wearing the hover.
    if (!isEnabled()) {
      hover_animation_->stop();
      hover_ = 0.0;
    }
    update();
  }
  QAbstractButton::changeEvent(event);
}

void IconButton::focusInEvent(QFocusEvent* event) {
  switch (event->reason()) {
    case Qt::TabFocusReason:
    case Qt::BacktabFocusReason:
    case Qt::ShortcutFocusReason:
      focus_by_keyboard_ = true;
      break;
    case Qt::ActiveWindowFocusReason:
    case Qt::PopupFocusReason:
      // The window coming back to the front, or a menu closing: the focus is
      // returning to where it already was, by whichever way it first got
      // there. What was true before is still true.
      break;
    default:
      focus_by_keyboard_ = false;
      break;
  }
  QAbstractButton::focusInEvent(event);
}

void IconButton::animate_hover(bool hovered) {
  const qreal target = hovered && isEnabled() ? 1.0 : 0.0;
  hover_animation_->stop();
  const int duration = theme::motion::duration(theme::motion::kFastMs);
  if (duration == 0) {
    hover_ = target;
    update();
    return;
  }
  hover_animation_->setDuration(duration);
  hover_animation_->setStartValue(hover_);
  hover_animation_->setEndValue(target);
  hover_animation_->start();
}

}  // namespace dv::ui
