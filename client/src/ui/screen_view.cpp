#include "ui/screen_view.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include <QMetaObject>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRect>
#include <QRectF>
#include <QRegion>
#include <QResizeEvent>
#include <QSize>

#include "ui/theme.hpp"

namespace dv::ui {

ScreenView::ScreenView(QWidget* parent)
    : QWidget(parent), placeholder_(QStringLiteral("nobody is sharing a screen")) {
  setMinimumSize(320, 180);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  // Painted edge to edge, so Qt does not have to clear it first.
  setAttribute(Qt::WA_OpaquePaintEvent);
}

void ScreenView::submit(const client::video::VideoFrame& frame) {
  if (frame.empty()) {
    return;
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);

    // BGRA in memory is exactly what Format_ARGB32 is on a little endian
    // machine, so this is a copy and not a conversion. The copy is needed: the
    // frame belongs to the media layer and is gone when this returns.
    //
    // Into a buffer that is already here, rather than through QImage::copy,
    // which allocates a new one every time. At 1280x720 that was three and a
    // half megabytes claimed and released thirty times a second, for a picture
    // whose size changes about once a call.
    if (pending_.width() != frame.width() || pending_.height() != frame.height() ||
        pending_.format() != QImage::Format_ARGB32) {
      pending_ = QImage(frame.width(), frame.height(), QImage::Format_ARGB32);
    }
    if (pending_.isNull()) {
      // QImage answers a null image rather than throwing when it cannot get
      // the memory. Writing into it would be a copy to a null pointer, and
      // dropping the frame is what the rest of this class already does with
      // every frame the interface thread is too busy to collect.
      return;
    }

    const auto stride = static_cast<qsizetype>(frame.stride());
    if (pending_.bytesPerLine() == stride &&
        static_cast<std::size_t>(pending_.sizeInBytes()) == frame.byte_count()) {
      std::memcpy(pending_.bits(), frame.data(), frame.byte_count());
    } else {
      // Only reachable if Qt ever pads a row of four byte pixels, which it
      // does not today. Row by row is the answer that stays correct if it
      // starts to.
      const auto row = static_cast<std::size_t>(std::min(stride, pending_.bytesPerLine()));
      for (int y = 0; y < frame.height(); ++y) {
        std::memcpy(pending_.scanLine(y),
                    frame.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(stride)),
                    row);
      }
    }
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
  where_ = QRect();
  update();
}

void ScreenView::set_placeholder(QString text) {
  placeholder_ = std::move(text);
  update();
}

void ScreenView::take_pending_frame() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    // Swapped and not moved. What was on screen becomes the pending buffer,
    // which is the one the media thread fills next, so the two images are
    // passed back and forth for the length of the call instead of one of them
    // being allocated per frame.
    current_.swap(pending_);
  }
  delivery_pending_.store(false);

  // The whole widget only when the picture moved. Thirty times a second, the
  // usual answer is that it did not, and repainting the letterbox around an
  // unchanged rectangle is the same pixels in the same colour.
  if (place_frame()) {
    update();
    return;
  }
  update(where_);
}

bool ScreenView::place_frame() {
  QRect placed;
  if (!current_.isNull()) {
    // Fitted rather than stretched, and centred in what is left. A shared
    // screen that does not match the widget's shape has to keep its own.
    QSize target = current_.size();
    target.scale(size(), Qt::KeepAspectRatio);
    placed =
        QRect(QPoint((width() - target.width()) / 2, (height() - target.height()) / 2), target);
  }
  if (placed == where_) {
    return false;
  }
  where_ = placed;
  return true;
}

void ScreenView::rebuild_card() {
  QPainterPath card;
  card.addRoundedRect(QRectF(rect()), theme::kCardRadius, theme::kCardRadius);
  QPainterPath box;
  box.addRect(QRectF(rect()));
  outside_ = box.subtracted(card);
  card_for_ = size();
}

void ScreenView::resizeEvent(QResizeEvent* event) {
  QWidget::resizeEvent(event);
  rebuild_card();
  place_frame();
}

void ScreenView::paintEvent(QPaintEvent* event) {
  if (card_for_ != size()) {
    rebuild_card();
  }

  QPainter painter(this);
  const QRect damaged = event->rect();
  painter.setClipRect(damaged);

  if (current_.isNull()) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    // WA_OpaquePaintEvent promises every pixel is painted, so what falls
    // outside the rounded rectangle has to be filled with the window colour by
    // hand rather than left to Qt to clear.
    painter.fillRect(damaged, palette().window());
    QPainterPath card;
    card.addRoundedRect(QRectF(rect()), theme::kCardRadius, theme::kCardRadius);
    painter.fillPath(card, palette().dark());
    painter.setPen(palette().color(QPalette::BrightText));
    painter.drawText(rect(), Qt::AlignCenter, placeholder_);
    return;
  }

  // Whatever the picture does not cover, and no more. Usually nothing at all:
  // a frame arriving into an unchanged layout asks only for the picture's own
  // rectangle, and this region comes out empty.
  const QRegion surround = QRegion(damaged).subtracted(QRegion(where_));
  for (const QRect& piece : surround) {
    painter.fillRect(piece, palette().dark());
  }

  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.drawImage(where_, current_);

  // The corners the card is missing, cut back out of the picture that was
  // allowed to be drawn over them. Antialiased, so the curve blends into the
  // picture rather than stepping down it.
  if (!outside_.isEmpty()) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillPath(outside_, palette().window());
  }
}

}  // namespace dv::ui
