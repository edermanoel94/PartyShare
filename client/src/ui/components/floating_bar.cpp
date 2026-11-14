#include "ui/components/floating_bar.hpp"

#include <QColor>
#include <QHBoxLayout>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>

#include "ui/theme.hpp"

namespace dv::ui {
namespace {

constexpr int kShadowRoom = 12;
constexpr int kPadding = 8;
/// Rounder than a card: the bar holds buttons whose own corners are twelve
/// pixels, and a surface has to be rounder than what it holds by about its
/// padding for the two curves to read as concentric.
constexpr int kRadius = 20;

}  // namespace

FloatingBar::FloatingBar(QWidget* parent) : QWidget(parent), row_(new QHBoxLayout(this)) {
  const int inset = kShadowRoom + kPadding;
  row_->setContentsMargins(inset, inset, inset, inset);
  row_->setSpacing(theme::space::kSm);
}

int FloatingBar::shadow_room() {
  return kShadowRoom;
}

void FloatingBar::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  const theme::Colors& colours = theme::colors();
  const QRectF surface =
      QRectF(rect()).adjusted(kShadowRoom, kShadowRoom, -kShadowRoom, -kShadowRoom);
  theme::paint_shadow(painter, surface, kRadius);

  // Very slightly see-through, so that what is under the bar is hinted at
  // rather than cut off: it floats over a picture somebody is trying to read.
  QColor ground = colours.surface_raised;
  ground.setAlphaF(0.96F);
  painter.setPen(QPen(colours.border_strong, 1.0));
  painter.setBrush(ground);
  painter.drawRoundedRect(surface.adjusted(0.5, 0.5, -0.5, -0.5), kRadius, kRadius);
}

}  // namespace dv::ui
