#include "ui/components/app_sidebar.hpp"

#include <QIcon>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QRect>
#include <QSignalBlocker>
#include <QVBoxLayout>

#include "ui/components/icon_button.hpp"
#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The rail: its width, the side of one of its buttons, and the program's
/// mark at the top with the room it stands in.
constexpr int kRailWidth = 68;
constexpr int kItem = 44;
constexpr int kItemRadius = 12;
constexpr int kMark = 36;
constexpr int kMarkTop = 16;

}  // namespace

AppSidebar::AppSidebar(QWidget* parent)
    : QWidget(parent), places_(new QVBoxLayout()), bottom_(new QVBoxLayout()) {
  setFixedWidth(kRailWidth);

  auto* column = new QVBoxLayout(this);
  // The mark is painted into the top margin.
  column->setContentsMargins(0, kMarkTop + kMark + theme::space::kLg, 0, theme::space::kLg);
  column->setSpacing(0);
  for (QVBoxLayout* group : {places_, bottom_}) {
    group->setContentsMargins(0, 0, 0, 0);
    group->setSpacing(theme::space::kSm);
    group->setAlignment(Qt::AlignHCenter);
  }
  column->addLayout(places_);
  column->addStretch();
  column->addLayout(bottom_);
}

IconButton* AppSidebar::add_destination(int id, Icon icon, const QString& name) {
  auto* button = new IconButton(icon, this);
  button->set_extent(kItem);
  button->set_corner_radius(kItemRadius);
  button->setCheckable(true);
  button->setToolTip(name);
  button->setAccessibleName(name);
  places_->addWidget(button, 0, Qt::AlignHCenter);
  destinations_.insert(id, button);

  // `clicked`, which already toggled the button. It is put back at once: the
  // rail shows where the window is, and that has not changed until the
  // listener says so through set_current. Without this a destination refused
  // by its listener would stay lit over a page that never came.
  connect(button, &IconButton::clicked, this, [this, button, id] {
    {
      const QSignalBlocker quiet(button);
      button->setChecked(!button->isChecked());
    }
    button->update();
    emit destination_chosen(id);
  });
  return button;
}

IconButton* AppSidebar::add_action(Icon icon, const QString& name) {
  auto* button = new IconButton(icon, this);
  button->set_extent(kItem);
  button->set_corner_radius(kItemRadius);
  button->setToolTip(name);
  button->setAccessibleName(name);
  bottom_->addWidget(button, 0, Qt::AlignHCenter);
  return button;
}

void AppSidebar::add_footer(QWidget* widget) {
  bottom_->addSpacing(theme::space::kXs);
  bottom_->addWidget(widget, 0, Qt::AlignHCenter);
}

void AppSidebar::set_current(int id) {
  for (auto it = destinations_.cbegin(); it != destinations_.cend(); ++it) {
    const QSignalBlocker quiet(it.value());
    it.value()->setChecked(it.key() == id);
    it.value()->update();
  }
}

void AppSidebar::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  const theme::Colors& colours = theme::colors();
  painter.fillRect(rect(), colours.chrome);
  painter.setPen(QPen(colours.border, 1.0));
  painter.drawLine(width() - 1, 0, width() - 1, height());

  // The program's own icon, from the resources main() registers it from, so
  // the mark in the window is the mark on the taskbar.
  const QRect mark((width() - kMark) / 2, kMarkTop, kMark, kMark);
  const QIcon icon(QStringLiteral(":/partyshare-48.png"));
  icon.paint(&painter, mark);
}

}  // namespace dv::ui
