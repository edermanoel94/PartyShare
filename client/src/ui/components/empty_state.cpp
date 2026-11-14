#include "ui/components/empty_state.hpp"

#include <QHideEvent>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPushButton>
#include <QRectF>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

#include "ui/components/buttons.hpp"
#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The disc the icon sits in, and the icon inside it.
constexpr int kDisc = 56;
constexpr int kDiscIcon = 24;

/// The loading ring: its diameter, the weight of its stroke, how often it is
/// redrawn and how far it turns each time. Thirty frames a second is smooth
/// for a ring this size and half the wake-ups of sixty.
constexpr int kRing = 28;
constexpr qreal kRingStroke = 3.0;
constexpr int kRingFrameMs = 33;
constexpr int kRingStep = 12;

}  // namespace

EmptyState::EmptyState(Icon icon, const QString& title, const QString& hint, QWidget* parent)
    : QWidget(parent),
      icon_(icon),
      title_(new QLabel(title, this)),
      hint_(new QLabel(hint, this)),
      column_(new QVBoxLayout(this)) {
  title_->setAlignment(Qt::AlignCenter);
  theme::set_role(title_, theme::TextRole::Title);

  hint_->setAlignment(Qt::AlignCenter);
  hint_->setWordWrap(true);
  hint_->setProperty("hint", true);

  column_->setContentsMargins(theme::space::kXl, theme::space::kXl, theme::space::kXl,
                              theme::space::kXl);
  column_->setSpacing(theme::space::kSm);
  column_->addStretch();
  // The disc is painted, in the room this leaves above the title.
  column_->addSpacing(kDisc + theme::space::kMd);
  column_->addWidget(title_);
  column_->addWidget(hint_);
  column_->addStretch();
}

void EmptyState::set_title(const QString& title) {
  title_->setText(title);
}

void EmptyState::set_hint(const QString& hint) {
  hint_->setText(hint);
}

QPushButton* EmptyState::add_action(const QString& text) {
  auto* button = new PrimaryButton(text, this);
  // Before the closing stretch, centred, at its own width rather than the
  // column's: a button as wide as the page is a banner.
  column_->insertSpacing(column_->count() - 1, theme::space::kSm);
  column_->insertWidget(column_->count() - 1, button, 0, Qt::AlignHCenter);
  return button;
}

void EmptyState::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  const theme::Colors& colours = theme::colors();
  // Directly above the title, wherever the layout has put it.
  const QRectF disc((width() - kDisc) / 2.0, title_->geometry().top() - theme::space::kMd - kDisc,
                    kDisc, kDisc);
  painter.setPen(Qt::NoPen);
  painter.setBrush(colours.accent_soft);
  painter.drawEllipse(disc);
  paint_icon(painter, icon_,
             QRectF(disc.center().x() - (kDiscIcon / 2.0), disc.center().y() - (kDiscIcon / 2.0),
                    kDiscIcon, kDiscIcon),
             colours.accent_text);
}

LoadingState::LoadingState(const QString& text, QWidget* parent)
    : QWidget(parent), text_(new QLabel(text, this)), timer_(new QTimer(this)) {
  text_->setAlignment(Qt::AlignCenter);
  text_->setProperty("hint", true);

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(theme::space::kXl, theme::space::kXl, theme::space::kXl,
                             theme::space::kXl);
  column->addStretch();
  column->addSpacing(kRing + theme::space::kMd);
  column->addWidget(text_);
  column->addStretch();

  timer_->setInterval(kRingFrameMs);
  connect(timer_, &QTimer::timeout, this, [this] {
    angle_ = (angle_ + kRingStep) % 360;
    update();
  });
}

void LoadingState::set_text(const QString& text) {
  text_->setText(text);
}

void LoadingState::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);
  const theme::Colors& colours = theme::colors();
  const QRectF ring((width() - kRing) / 2.0, text_->geometry().top() - theme::space::kMd - kRing,
                    kRing, kRing);
  painter.setBrush(Qt::NoBrush);
  painter.setPen(QPen(colours.surface_pressed, kRingStroke));
  painter.drawEllipse(ring);
  // A quarter of the ring in the accent, turning. Clockwise, which in Qt's
  // angles - counter-clockwise from three o'clock, in sixteenths of a degree -
  // is a start that goes down as the angle goes up.
  painter.setPen(QPen(colours.accent, kRingStroke, Qt::SolidLine, Qt::RoundCap));
  painter.drawArc(ring, -angle_ * 16, 90 * 16);
}

void LoadingState::showEvent(QShowEvent* event) {
  // Only while there is somebody to turn for, and only if things move at all.
  if (theme::motion::enabled()) {
    timer_->start();
  }
  QWidget::showEvent(event);
}

void LoadingState::hideEvent(QHideEvent* event) {
  timer_->stop();
  QWidget::hideEvent(event);
}

}  // namespace dv::ui
