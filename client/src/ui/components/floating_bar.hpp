#pragma once

#include <QWidget>

class QHBoxLayout;
class QPaintEvent;

namespace dv::ui {

/// A row of controls that floats over something else rather than taking a
/// strip of the layout from it: a raised, rounded surface with a shadow under
/// it. The call controls over a shared screen are one.
///
/// It is a child of what it floats over and is placed by hand - move() it -
/// because the whole point of it is not to be in that widget's layout. Put
/// the controls in `row()`.
class FloatingBar : public QWidget {
  Q_OBJECT

 public:
  explicit FloatingBar(QWidget* parent = nullptr);

  [[nodiscard]] QHBoxLayout* row() const { return row_; }

  /// How far the surface stands in from the widget's own edge, which is the
  /// room its shadow is painted in. Whoever places the bar subtracts this to
  /// line the visible surface up with something.
  [[nodiscard]] static int shadow_room();

 protected:
  void paintEvent(QPaintEvent* event) override;

 private:
  QHBoxLayout* row_ = nullptr;
};

}  // namespace dv::ui
