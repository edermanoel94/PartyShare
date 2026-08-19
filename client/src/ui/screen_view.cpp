#include "ui/screen_view.hpp"

#include <utility>

#include <QMetaObject>
#include <QPainter>
#include <QRect>
#include <QSize>

namespace dv::ui {

ScreenView::ScreenView(QWidget* parent)
    : QWidget(parent), placeholder_(QStringLiteral("ninguém está compartilhando a tela")) {
  setMinimumSize(320, 180);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  // Painted edge to edge, so Qt does not have to clear it first.
  setAttribute(Qt::WA_OpaquePaintEvent);
}

void ScreenView::submit(const client::video::VideoFrame& frame) {
  if (frame.empty()) {
    return;
  }

  // BGRA in memory is exactly what Format_ARGB32 is on a little endian
  // machine, so this is a copy and not a conversion. The copy is needed: the
  // frame belongs to the media layer and is gone when this returns.
  const QImage wrapped(frame.data(), frame.width(), frame.height(), frame.stride(),
                       QImage::Format_ARGB32);

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_ = wrapped.copy();
  }

  // Only one invocation is in flight at a time. Whatever frame is pending when
  // the interface thread gets round to it is the one that gets drawn, and the
  // ones in between are simply skipped.
  if (!delivery_pending_.exchange(true)) {
    QMetaObject::invokeMethod(this, "take_pending_frame", Qt::QueuedConnection);
  }
}

void ScreenView::clear() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_ = QImage();
    current_ = QImage();
  }
  update();
}

void ScreenView::set_placeholder(QString text) {
  placeholder_ = std::move(text);
  update();
}

void ScreenView::take_pending_frame() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    current_ = std::move(pending_);
    pending_ = QImage();
  }
  delivery_pending_.store(false);
  update();
}

void ScreenView::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.fillRect(rect(), palette().dark());

  if (current_.isNull()) {
    painter.setPen(palette().color(QPalette::BrightText));
    painter.drawText(rect(), Qt::AlignCenter, placeholder_);
    return;
  }

  // Fitted rather than stretched, and centred in what is left. A shared screen
  // that does not match the widget's shape has to keep its own.
  QSize target = current_.size();
  target.scale(size(), Qt::KeepAspectRatio);
  const QRect where(QPoint((width() - target.width()) / 2, (height() - target.height()) / 2),
                    target);

  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.drawImage(where, current_);
}

}  // namespace dv::ui
