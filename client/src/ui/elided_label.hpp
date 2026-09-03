#pragma once

#include <QLabel>
#include <QSize>
#include <QString>

class QEvent;
class QPaintEvent;

namespace dv::ui {

/// A one line label that gives way instead of overflowing.
///
/// A QLabel asks the layout for the whole width of its text and, when the
/// layout has less to give, paints the text anyway and lets the edge cut it
/// off mid-word: "sound shared (0" is what the status bar showed during a
/// share with sound, with nothing to say that anything was missing. This one
/// asks for the whole width too, so nothing changes while there is room; when
/// there is not, it ends in an ellipsis and offers the whole line as a tooltip.
class ElidedLabel final : public QLabel {
  Q_OBJECT

 public:
  explicit ElidedLabel(QWidget* parent = nullptr);

  /// Wide enough for the ellipsis alone, which is what lets the layout take
  /// the rest back.
  [[nodiscard]] QSize minimumSizeHint() const override;

 protected:
  bool event(QEvent* event) override;
  void paintEvent(QPaintEvent* event) override;

 private:
  /// The text as it fits right now.
  [[nodiscard]] QString shown() const;
};

}  // namespace dv::ui
