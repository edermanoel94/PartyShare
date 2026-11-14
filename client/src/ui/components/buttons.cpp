#include "ui/components/buttons.hpp"

#include <QColor>
#include <QEvent>
#include <QIcon>
#include <QSize>

#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The icon's side in a button, in logical pixels: the x-height of the label
/// and a little, so the two read as one line.
constexpr int kIconSide = 16;

/// The height every button of this family stands at, so a row of them - and a
/// field beside them - shares a top and a bottom.
constexpr int kButtonHeight = 38;

}  // namespace

Button::Button(const QString& text, QWidget* parent) : QPushButton(text, parent) {
  setMinimumHeight(kButtonHeight);
  setCursor(Qt::PointingHandCursor);
  setIconSize(QSize(kIconSide, kIconSide));
  // An icon is a picture of the colours as they were when it was rendered.
  connect(theme::notifier(), &theme::Notifier::changed, this, [this] { refresh_icon(); });
}

void Button::set_leading_icon(Icon icon) {
  icon_ = icon;
  refresh_icon();
}

void Button::set_ink(Ink ink) {
  ink_ = ink;
  refresh_icon();
}

void Button::changeEvent(QEvent* event) {
  // Moving to a screen with a different scale factor: the pixmap was rendered
  // for the old one and would be stretched or shrunk onto the new.
  if (event->type() == QEvent::DevicePixelRatioChange) {
    refresh_icon();
  }
  QPushButton::changeEvent(event);
}

void Button::refresh_icon() {
  if (!icon_.has_value()) {
    return;
  }
  const Icon drawn = icon_.value_or(Icon::Plus);
  const theme::Colors& colours = theme::colors();
  QColor ink = colours.text;
  switch (ink_) {
    case Ink::OnAccent:
      ink = colours.on_accent;
      break;
    case Ink::Accent:
      ink = colours.accent_text;
      break;
    case Ink::Danger:
      ink = colours.danger;
      break;
    case Ink::Text:
      break;
  }
  const qreal ratio = devicePixelRatioF();
  QIcon icon;
  icon.addPixmap(icon_pixmap(drawn, kIconSide, ink, ratio), QIcon::Normal);
  // Qt would otherwise make the disabled one itself, by greying the normal
  // pixmap - which turns white-on-accent into a pale smudge.
  icon.addPixmap(icon_pixmap(drawn, kIconSide, colours.muted, ratio), QIcon::Disabled);
  setIcon(icon);
}

PrimaryButton::PrimaryButton(const QString& text, QWidget* parent) : Button(text, parent) {
  setProperty("accent", true);
  set_ink(Ink::OnAccent);
}

SecondaryButton::SecondaryButton(const QString& text, QWidget* parent) : Button(text, parent) {}

DangerButton::DangerButton(const QString& text, QWidget* parent) : Button(text, parent) {
  setProperty("danger", true);
  set_ink(Ink::Danger);
}

QuietButton::QuietButton(const QString& text, QWidget* parent) : Button(text, parent) {
  setProperty("quiet", QStringLiteral("accent"));
  set_ink(Ink::Accent);
}

PrimaryButton::PrimaryButton(QWidget* parent) : PrimaryButton(QString{}, parent) {}

SecondaryButton::SecondaryButton(QWidget* parent) : SecondaryButton(QString{}, parent) {}

DangerButton::DangerButton(QWidget* parent) : DangerButton(QString{}, parent) {}

QuietButton::QuietButton(QWidget* parent) : QuietButton(QString{}, parent) {}

}  // namespace dv::ui
