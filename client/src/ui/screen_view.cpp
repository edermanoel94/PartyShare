#include "ui/screen_view.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include <QBrush>
#include <QColor>
#include <QLabel>
#include <QMetaObject>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPalette>
#include <QRect>
#include <QRectF>
#include <QRegion>
#include <QResizeEvent>
#include <QSize>
#include <QTimer>

#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// How long a notice stays over the picture. Three seconds is long enough to
/// read one short sentence twice and short enough that it is gone before it
/// is in the way of what the person entered full screen to look at.
constexpr int kNoticeMs = 3000;

/// How far down from the top the notice sits. Off the very edge, where a
/// picture's own title bar usually is, and well above the middle, where the
/// thing being watched usually is.
constexpr int kNoticeTop = 24;

}  // namespace

ScreenView::ScreenView(QWidget* parent)
    : QWidget(parent),
      notice_(new QLabel(this)),
      notice_timer_(new QTimer(this)),
      placeholder_(QStringLiteral("nobody is sharing a screen")) {
  setMinimumSize(320, 180);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
  // Painted edge to edge, so Qt does not have to clear it first.
  setAttribute(Qt::WA_OpaquePaintEvent);

  // Styled here and not in the theme sheet, because it is the one label in the
  // program that sits on a picture rather than on a window: whatever the
  // scheme, the ground under it is somebody's screen and the only colours that
  // read on that are white on a dark translucent pill.
  notice_->setAttribute(Qt::WA_TransparentForMouseEvents);
  notice_->setAlignment(Qt::AlignCenter);
  notice_->setStyleSheet(
      QStringLiteral("QLabel { background: rgba(0, 0, 0, 170); color: white; "
                     "border-radius: 8px; padding: 8px 16px; }"));
  notice_->hide();

  notice_timer_->setSingleShot(true);
  notice_timer_->setInterval(kNoticeMs);
  connect(notice_timer_, &QTimer::timeout, notice_, &QWidget::hide);
}

void ScreenView::submit(const client::video::VideoFrame& frame) {
  if (frame.empty()) {
    return;
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);

    // Taken before the copy, so that a frame the decoder delivers after the
    // share ended costs nothing at all rather than three and a half megabytes
    // of memcpy on its way to being thrown away.
    if (!receiving_) {
      return;
    }

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

void ScreenView::set_receiving(bool receiving) {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    receiving_ = receiving;
    if (receiving) {
      // What is on screen is left alone. A share that is starting has its own
      // first frame on the way, and blanking the panel to wait for it is a
      // flash of placeholder that says nothing.
      return;
    }
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

void ScreenView::set_edge_to_edge(bool on) {
  if (edge_to_edge_ == on) {
    return;
  }
  edge_to_edge_ = on;
  // The whole widget: the surround changes colour and the corners come or go,
  // and neither of those is inside the picture's rectangle.
  update();
}

void ScreenView::show_notice(const QString& text) {
  notice_->setText(text);
  notice_->adjustSize();
  place_notice();
  notice_->show();
  // Over the picture, not under it. A child is drawn above its parent already;
  // this is for the day something else is parented here too.
  notice_->raise();
  notice_timer_->start();
}

void ScreenView::place_notice() {
  notice_->move((width() - notice_->width()) / 2, kNoticeTop);
}

void ScreenView::mouseDoubleClickEvent(QMouseEvent* event) {
  if (event->button() == Qt::LeftButton) {
    emit activated();
  }
  QWidget::mouseDoubleClickEvent(event);
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
  // Entering full screen is a resize, and it is exactly when the notice is up.
  if (notice_->isVisible()) {
    place_notice();
  }
}

void ScreenView::paintEvent(QPaintEvent* event) {
  if (card_for_ != size()) {
    rebuild_card();
  }

  QPainter painter(this);
  const QRect damaged = event->rect();
  painter.setClipRect(damaged);

  // Black rather than the canvas colour when the picture is the whole screen.
  // The two are a few steps apart, and on a screen that is otherwise showing
  // nothing else the difference is a grey frame around somebody's desktop.
  const QBrush ground = edge_to_edge_ ? QBrush(Qt::black) : palette().dark();

  if (current_.isNull()) {
    if (edge_to_edge_) {
      painter.fillRect(damaged, ground);
    } else {
      painter.setRenderHint(QPainter::Antialiasing, true);
      // WA_OpaquePaintEvent promises every pixel is painted, so what falls
      // outside the rounded rectangle has to be filled with the window colour
      // by hand rather than left to Qt to clear.
      painter.fillRect(damaged, palette().window());
      QPainterPath card;
      card.addRoundedRect(QRectF(rect()), theme::kCardRadius, theme::kCardRadius);
      painter.fillPath(card, ground);
    }
    painter.setPen(palette().color(QPalette::BrightText));
    painter.drawText(rect(), Qt::AlignCenter, placeholder_);
    return;
  }

  // Whatever the picture does not cover, and no more. Usually nothing at all:
  // a frame arriving into an unchanged layout asks only for the picture's own
  // rectangle, and this region comes out empty.
  const QRegion surround = QRegion(damaged).subtracted(QRegion(where_));
  for (const QRect& piece : surround) {
    painter.fillRect(piece, ground);
  }

  painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
  painter.drawImage(where_, current_);

  // The corners the card is missing, cut back out of the picture that was
  // allowed to be drawn over them. Antialiased, so the curve blends into the
  // picture rather than stepping down it. Not when the picture is the whole
  // screen: a screen has no card to be the corners of.
  if (!edge_to_edge_ && !outside_.isEmpty()) {
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.fillPath(outside_, palette().window());
  }
}

}  // namespace dv::ui
