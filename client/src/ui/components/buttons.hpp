#pragma once

#include <cstdint>
#include <optional>

#include <QPushButton>
#include <QString>

#include "ui/components/icons.hpp"

class QEvent;

namespace dv::ui {

/// A QPushButton that can carry one of the painted icons in front of its
/// text, in a colour that follows the theme and the button's state.
///
/// The base of the four below, and nothing on its own: what a button looks
/// like is decided by the stylesheet from a property each subclass sets, so
/// that a button made in Qt Designer by setting the same property - `accent`,
/// `danger`, `quiet` - is the same button as one made here.
class Button : public QPushButton {
  Q_OBJECT

 public:
  /// What the icon is drawn in. It cannot be read back from the stylesheet,
  /// so each kind of button says which of the theme's colours its text wears.
  enum class Ink : std::uint8_t { Text, OnAccent, Accent, Danger };

  explicit Button(const QString& text, QWidget* parent = nullptr);

  void set_leading_icon(Icon icon);

 protected:
  void set_ink(Ink ink);
  void changeEvent(QEvent* event) override;

 private:
  /// Renders the icon again, in the colours in force now.
  void refresh_icon();

  std::optional<Icon> icon_;
  Ink ink_ = Ink::Text;
};

/// The one thing to press on the screen it is on. One per screen: two of
/// these side by side is two things claiming to be the answer.
class PrimaryButton : public Button {
  Q_OBJECT

 public:
  explicit PrimaryButton(const QString& text, QWidget* parent = nullptr);
  /// With no text, and a parent that has to be given: the constructor Qt
  /// Designer calls for a widget promoted to this class.
  explicit PrimaryButton(QWidget* parent);
};

/// Everything else that is a button: bordered, on the surface colour.
class SecondaryButton : public Button {
  Q_OBJECT

 public:
  explicit SecondaryButton(const QString& text, QWidget* parent = nullptr);
  /// With no text, and a parent that has to be given: the constructor Qt
  /// Designer calls for a widget promoted to this class.
  explicit SecondaryButton(QWidget* parent);
};

/// Leaving, deleting, anything not undone by pressing something else.
class DangerButton : public Button {
  Q_OBJECT

 public:
  explicit DangerButton(const QString& text, QWidget* parent = nullptr);
  /// With no text, and a parent that has to be given: the constructor Qt
  /// Designer calls for a widget promoted to this class.
  explicit DangerButton(QWidget* parent);
};

/// A line of accent coloured text until the pointer is on it. For an action
/// that belongs to a row or a card and would be a toolbar if every one of its
/// kind had a border.
class QuietButton : public Button {
  Q_OBJECT

 public:
  explicit QuietButton(const QString& text, QWidget* parent = nullptr);
  /// With no text, and a parent that has to be given: the constructor Qt
  /// Designer calls for a widget promoted to this class.
  explicit QuietButton(QWidget* parent);
};

}  // namespace dv::ui
