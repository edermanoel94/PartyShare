#pragma once

#include <cstdint>
#include <optional>

#include <QAbstractButton>
#include <QSize>
#include <QString>

#include "ui/components/icons.hpp"

class QEnterEvent;
class QEvent;
class QFocusEvent;
class QPaintEvent;
class QVariantAnimation;

namespace dv::ui {

/// A button that is an icon, with or without a word beside it: the items of
/// the navigation rail, the call controls, the small actions in a header.
///
/// Drawn by hand rather than by the stylesheet, for two things a stylesheet
/// cannot do. The icon has to change colour with the button's state - a
/// QPushButton's icon is a picture, and stays the colour it was rendered in
/// when the background under it turns red. And the background eases between
/// its resting and hovered colours rather than switching, which is most of
/// what makes a row of these feel like one surface instead of six boxes.
///
/// Knows nothing about what it is for: no session, no page, no room.
class IconButton : public QAbstractButton {
  Q_OBJECT

 public:
  /// How much the button asks to be looked at.
  enum class Variant : std::uint8_t {
    /// Nothing behind the icon until the pointer is on it. Navigation, and
    /// anything that sits in a row of its own kind.
    Ghost,
    /// A quiet filled shape. The call controls.
    Tonal,
    /// The one thing to press.
    Accent,
    /// Leaving, deleting.
    Danger,
  };

  /// What a checkable button wears while it is on. Sharing is a good state
  /// and wears the accent; a muted microphone is a warning and wears amber -
  /// the same rule the stylesheet's `toggle` property follows.
  enum class CheckedTone : std::uint8_t { Accent, Warn };

  explicit IconButton(Icon icon, QWidget* parent = nullptr);

  void set_icon(Icon icon);
  /// A different drawing while the button is checked: the microphone, struck
  /// through. Without one the same icon is drawn in both states.
  void set_checked_icon(Icon icon);
  void set_variant(Variant variant);
  void set_checked_tone(CheckedTone tone);
  /// A word beside the icon. Empty, which is how it starts, is an icon alone -
  /// and then the tooltip is the only name the button has, so give it one.
  void set_label(const QString& label);
  /// The height, and the width too while there is no label.
  void set_extent(int extent);
  void set_corner_radius(int radius);

  [[nodiscard]] QSize sizeHint() const override;
  [[nodiscard]] QSize minimumSizeHint() const override;

 protected:
  void paintEvent(QPaintEvent* event) override;
  void enterEvent(QEnterEvent* event) override;
  void leaveEvent(QEvent* event) override;
  void changeEvent(QEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;

 private:
  /// Eases `hover_` towards 1 or back to 0.
  void animate_hover(bool hovered);

  Icon icon_;
  std::optional<Icon> checked_icon_;
  Variant variant_ = Variant::Ghost;
  CheckedTone checked_tone_ = CheckedTone::Accent;
  QString label_;
  int extent_ = 40;
  int radius_ = 10;
  /// Whether the focus it holds was brought by the keyboard. The ring is
  /// drawn only then: it is there to say where the keyboard is, and a button
  /// that has just been clicked with a mouse does not need telling where the
  /// mouse was - it only stays ringed, as though it were still the point.
  bool focus_by_keyboard_ = false;
  /// 0 at rest, 1 with the pointer on it, and somewhere between while moving.
  qreal hover_ = 0.0;
  QVariantAnimation* hover_animation_ = nullptr;
};

}  // namespace dv::ui
