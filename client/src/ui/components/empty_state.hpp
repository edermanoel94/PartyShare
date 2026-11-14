#pragma once

#include <QString>
#include <QWidget>

#include "ui/components/icons.hpp"

class QHideEvent;
class QLabel;
class QPaintEvent;
class QPushButton;
class QShowEvent;
class QTimer;
class QVBoxLayout;

namespace dv::ui {

/// What stands where a list would be when the list is empty: a picture, a
/// sentence saying that there is nothing, and one saying what to do about it.
///
/// A table with no rows is indistinguishable from a table that failed to
/// load. This is how the empty case says which one it is.
class EmptyState : public QWidget {
  Q_OBJECT

 public:
  EmptyState(Icon icon, const QString& title, const QString& hint, QWidget* parent = nullptr);

  void set_title(const QString& title);
  void set_hint(const QString& hint);
  /// Puts the one thing to do about it under the sentence, and hands the
  /// button back to be connected. One per empty state.
  QPushButton* add_action(const QString& text);

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  Icon icon_;
  QLabel* title_ = nullptr;
  QLabel* hint_ = nullptr;
  QVBoxLayout* column_ = nullptr;
};

/// What stands where the content will be while it is on its way: a ring going
/// round and a sentence saying what is being waited for.
///
/// The ring only turns while the widget is on screen, and stands still when
/// animations are off - the sentence is then the whole of the message, which
/// is why it is not optional.
class LoadingState : public QWidget {
  Q_OBJECT

 public:
  explicit LoadingState(const QString& text, QWidget* parent = nullptr);

  void set_text(const QString& text);

 protected:
  void paintEvent(QPaintEvent* event) override;
  void showEvent(QShowEvent* event) override;
  void hideEvent(QHideEvent* event) override;

 private:
  QLabel* text_ = nullptr;
  QTimer* timer_ = nullptr;
  /// Where the ring has got to, in degrees.
  int angle_ = 0;
};

}  // namespace dv::ui
