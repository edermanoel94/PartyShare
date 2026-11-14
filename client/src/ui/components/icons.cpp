#include "ui/components/icons.hpp"

#include <algorithm>

#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QPointF>
#include <QRectF>

namespace dv::ui {
namespace {

/// The grid every icon is drawn on, and the stroke that reads as one weight
/// across all of them at the sizes the interface uses, 16 to 22 pixels.
constexpr qreal kGrid = 24.0;
constexpr qreal kStroke = 1.9;

void line(QPainterPath& path, qreal x1, qreal y1, qreal x2, qreal y2) {
  path.moveTo(x1, y1);
  path.lineTo(x2, y2);
}

void circle(QPainterPath& path, qreal x, qreal y, qreal radius) {
  path.addEllipse(QPointF(x, y), radius, radius);
}

/// The microphone, shared by the open and the struck-through one.
void microphone(QPainterPath& path) {
  path.addRoundedRect(QRectF(9, 3, 6, 12), 3, 3);
  // The cup: half a circle under the head, from its left edge round the
  // bottom to its right. Angles run counter-clockwise from three o'clock, and
  // y grows downwards, so 180 to 360 is the lower half.
  path.moveTo(5, 11);
  path.arcTo(QRectF(5, 4, 14, 14), 180, 180);
  line(path, 12, 18, 12, 21);
}

/// The strokes of `icon`, and into `dots` the few shapes that are filled
/// rather than outlined: an eye, the point of an exclamation mark.
void build(Icon icon, QPainterPath& path, QPainterPath& dots) {
  switch (icon) {
    case Icon::Rooms:
      path.addRoundedRect(QRectF(3, 3, 7, 7), 1.5, 1.5);
      path.addRoundedRect(QRectF(14, 3, 7, 7), 1.5, 1.5);
      path.addRoundedRect(QRectF(3, 14, 7, 7), 1.5, 1.5);
      path.addRoundedRect(QRectF(14, 14, 7, 7), 1.5, 1.5);
      break;
    case Icon::Shield:
      path.moveTo(12, 3);
      path.lineTo(20, 6);
      path.lineTo(20, 12);
      path.cubicTo(20, 16.5, 16.8, 20, 12, 21);
      path.cubicTo(7.2, 20, 4, 16.5, 4, 12);
      path.lineTo(4, 6);
      path.closeSubpath();
      break;
    case Icon::Settings:
      // Three sliders, each with its knob somewhere else along the track.
      line(path, 4, 6, 12.8, 6);
      line(path, 17.2, 6, 20, 6);
      circle(path, 15, 6, 2.2);
      line(path, 4, 12, 6.8, 12);
      line(path, 11.2, 12, 20, 12);
      circle(path, 9, 12, 2.2);
      line(path, 4, 18, 14.8, 18);
      line(path, 19.2, 18, 20, 18);
      circle(path, 17, 18, 2.2);
      break;
    case Icon::Microphone:
      microphone(path);
      break;
    case Icon::MicrophoneOff:
      microphone(path);
      line(path, 4, 4, 20, 20);
      break;
    case Icon::Screen:
      path.addRoundedRect(QRectF(3, 4, 18, 12), 2, 2);
      line(path, 8, 20, 16, 20);
      line(path, 12, 16, 12, 20);
      break;
    case Icon::Fullscreen:
      path.moveTo(4, 9);
      path.lineTo(4, 4);
      path.lineTo(9, 4);
      path.moveTo(15, 4);
      path.lineTo(20, 4);
      path.lineTo(20, 9);
      path.moveTo(4, 15);
      path.lineTo(4, 20);
      path.lineTo(9, 20);
      path.moveTo(15, 20);
      path.lineTo(20, 20);
      path.lineTo(20, 15);
      break;
    case Icon::ExitFullscreen:
      path.moveTo(9, 4);
      path.lineTo(9, 9);
      path.lineTo(4, 9);
      path.moveTo(15, 4);
      path.lineTo(15, 9);
      path.lineTo(20, 9);
      path.moveTo(9, 20);
      path.lineTo(9, 15);
      path.lineTo(4, 15);
      path.moveTo(15, 20);
      path.lineTo(15, 15);
      path.lineTo(20, 15);
      break;
    case Icon::Signal:
      line(path, 5, 20, 5, 16);
      line(path, 10, 20, 10, 12);
      line(path, 15, 20, 15, 8);
      line(path, 20, 20, 20, 4);
      break;
    case Icon::Leave:
      // A door frame open to the right, and an arrow going out through it.
      path.moveTo(9, 3);
      path.lineTo(5, 3);
      path.quadTo(3, 3, 3, 5);
      path.lineTo(3, 19);
      path.quadTo(3, 21, 5, 21);
      path.lineTo(9, 21);
      path.moveTo(16, 17);
      path.lineTo(21, 12);
      path.lineTo(16, 7);
      line(path, 21, 12, 9, 12);
      break;
    case Icon::Copy:
      path.addRoundedRect(QRectF(9, 9, 11, 11), 2, 2);
      path.moveTo(5, 15);
      path.lineTo(5, 6);
      path.quadTo(5, 4, 7, 4);
      path.lineTo(16, 4);
      break;
    case Icon::Plus:
      line(path, 12, 5, 12, 19);
      line(path, 5, 12, 19, 12);
      break;
    case Icon::Search:
      circle(path, 11, 11, 7);
      line(path, 20, 20, 16.5, 16.5);
      break;
    case Icon::Send:
      path.moveTo(4, 12);
      path.lineTo(20, 4);
      path.lineTo(14, 20);
      path.lineTo(11, 13);
      path.closeSubpath();
      break;
    case Icon::Smile:
      circle(path, 12, 12, 9);
      path.moveTo(8.5, 14.2);
      path.quadTo(12, 18.4, 15.5, 14.2);
      circle(dots, 9, 9.6, 1.1);
      circle(dots, 15, 9.6, 1.1);
      break;
    case Icon::Users:
      circle(path, 9, 8, 3.5);
      path.moveTo(2.5, 20);
      path.cubicTo(2.5, 15.6, 5.4, 13.5, 9, 13.5);
      path.cubicTo(12.6, 13.5, 15.5, 15.6, 15.5, 20);
      // The second person, mostly behind the first: the edge of a head and of
      // a shoulder.
      path.moveTo(16, 4.6);
      path.cubicTo(18.7, 5.4, 18.7, 10.6, 16, 11.4);
      path.moveTo(18, 14.4);
      path.cubicTo(20.2, 15.4, 21.5, 17.5, 21.5, 20);
      break;
    case Icon::Chat:
      path.moveTo(7, 4);
      path.lineTo(17, 4);
      path.quadTo(21, 4, 21, 8);
      path.lineTo(21, 13);
      path.quadTo(21, 17, 17, 17);
      path.lineTo(11, 17);
      path.lineTo(6, 21);
      path.lineTo(6.6, 17);
      path.quadTo(3, 16.6, 3, 13);
      path.lineTo(3, 8);
      path.quadTo(3, 4, 7, 4);
      break;
    case Icon::Check:
      path.moveTo(5, 12.5);
      path.lineTo(9.5, 17);
      path.lineTo(19, 7.5);
      break;
    case Icon::Info:
      circle(path, 12, 12, 9);
      line(path, 12, 11, 12, 16.5);
      circle(dots, 12, 7.8, 1.1);
      break;
    case Icon::Warning:
      path.moveTo(12, 3.5);
      path.lineTo(21.5, 20);
      path.lineTo(2.5, 20);
      path.closeSubpath();
      line(path, 12, 10, 12, 14.4);
      circle(dots, 12, 17.2, 1.0);
      break;
    case Icon::Error:
      circle(path, 12, 12, 9);
      line(path, 9, 9, 15, 15);
      line(path, 15, 9, 9, 15);
      break;
    case Icon::Close:
      line(path, 6, 6, 18, 18);
      line(path, 18, 6, 6, 18);
      break;
    case Icon::Key:
      circle(path, 8, 16, 4);
      line(path, 10.9, 13.1, 20, 4);
      line(path, 16.5, 7.5, 19, 10);
      line(path, 14, 10, 16, 12);
      break;
    case Icon::Volume:
      path.moveTo(11, 5);
      path.lineTo(6, 9);
      path.lineTo(3, 9);
      path.lineTo(3, 15);
      path.lineTo(6, 15);
      path.lineTo(11, 19);
      path.closeSubpath();
      path.moveTo(15.5, 8.5);
      path.quadTo(18.2, 12, 15.5, 15.5);
      path.moveTo(18.5, 5.5);
      path.quadTo(23, 12, 18.5, 18.5);
      break;
    case Icon::ArrowLeft:
      line(path, 20, 12, 4, 12);
      path.moveTo(10, 6);
      path.lineTo(4, 12);
      path.lineTo(10, 18);
      break;
  }
}

}  // namespace

void paint_icon(QPainter& painter, Icon icon, const QRectF& box, const QColor& colour) {
  QPainterPath path;
  QPainterPath dots;
  build(icon, path, dots);

  const qreal size = std::min(box.width(), box.height());
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.translate(box.center().x() - (size / 2), box.center().y() - (size / 2));
  painter.scale(size / kGrid, size / kGrid);

  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(colour, kStroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter.drawPath(path);
  if (!dots.isEmpty()) {
    painter.setPen(Qt::NoPen);
    painter.setBrush(colour);
    painter.drawPath(dots);
  }
  painter.restore();
}

QPixmap icon_pixmap(Icon icon, int size, const QColor& colour, qreal device_pixel_ratio) {
  const int pixels = std::max(1, static_cast<int>(size * device_pixel_ratio));
  QPixmap pixmap(pixels, pixels);
  pixmap.fill(Qt::transparent);
  pixmap.setDevicePixelRatio(device_pixel_ratio);
  QPainter painter(&pixmap);
  paint_icon(painter, icon, QRectF(0, 0, size, size), colour);
  return pixmap;
}

}  // namespace dv::ui
