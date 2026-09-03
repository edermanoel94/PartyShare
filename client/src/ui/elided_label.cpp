#include "ui/elided_label.hpp"

#include <algorithm>

#include <QCursor>
#include <QEvent>
#include <QFontMetrics>
#include <QMargins>
#include <QPainter>
#include <QStyle>
#include <QToolTip>

namespace dv::ui {

ElidedLabel::ElidedLabel(QWidget* parent) : QLabel(parent) {}

QSize ElidedLabel::minimumSizeHint() const {
  const QSize whole = QLabel::minimumSizeHint();
  const QMargins margins = contentsMargins();
  const int ellipsis = fontMetrics().horizontalAdvance(QStringLiteral("…"));
  return {std::min(whole.width(), ellipsis + margins.left() + margins.right()), whole.height()};
}

QString ElidedLabel::shown() const {
  return fontMetrics().elidedText(text(), Qt::ElideRight, contentsRect().width());
}

bool ElidedLabel::event(QEvent* event) {
  // The tooltip exists only while something is hidden. A label that repeats
  // itself on hover when it already fits is noise.
  if (event->type() == QEvent::ToolTip) {
    const QString whole = text();
    if (shown() != whole) {
      QToolTip::showText(QCursor::pos(), whole, this);
    } else {
      QToolTip::hideText();
    }
    return true;
  }
  return QLabel::event(event);
}

void ElidedLabel::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  // Through the style, so that the colour the stylesheet gave this label (the
  // muted ink of the status bar) is the colour the text is drawn in.
  style()->drawItemText(&painter, contentsRect(), static_cast<int>(alignment()), palette(),
                        isEnabled(), shown(), foregroundRole());
}

}  // namespace dv::ui
