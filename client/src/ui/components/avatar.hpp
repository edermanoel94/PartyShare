#pragma once

#include <QColor>
#include <QString>
#include <QWidget>

class QFocusEvent;
class QKeyEvent;
class QMouseEvent;
class QPaintEvent;
class QPainter;
class QRectF;

namespace dv::ui {

/// Draws the round badge that stands for a person: the first letter of their
/// name on a colour worked out from the whole of it, so the same person is the
/// same colour in the room list, the participant list and the chat without
/// anybody having chosen one.
///
/// A free function as well as a widget, because half the places that want one
/// are item delegates, which paint and have no widgets.
void paint_avatar(QPainter& painter, const QRectF& box, const QString& name);

/// The colour pair behind `name`'s badge: what it is filled with and what is
/// written on it. Both follow the colour scheme.
struct AvatarColours {
  QColor ground;
  QColor ink;
};
[[nodiscard]] AvatarColours avatar_colours(const QString& name);

/// paint_avatar as a widget, for a layout.
class Avatar : public QWidget {
  Q_OBJECT

 public:
  explicit Avatar(int diameter, QWidget* parent = nullptr);

  void set_name(const QString& name);
  /// A ring round the badge: this is the person speaking.
  void set_ring(bool on);
  /// Makes the badge something to press: it takes the focus, shows a hand,
  /// and says `clicked` for a click, Space or Enter. Off, which is how it
  /// starts, it is a picture.
  void set_interactive(bool on);

 signals:
  void clicked();

 protected:
  void paintEvent(QPaintEvent* event) override;
  void mouseReleaseEvent(QMouseEvent* event) override;
  void keyPressEvent(QKeyEvent* event) override;
  void focusInEvent(QFocusEvent* event) override;

 private:
  QString name_;
  int diameter_;
  bool ring_ = false;
  bool interactive_ = false;
  /// The focus ring is for the keyboard. See IconButton, which has the same
  /// member for the same reason.
  bool focus_by_keyboard_ = false;
};

}  // namespace dv::ui
