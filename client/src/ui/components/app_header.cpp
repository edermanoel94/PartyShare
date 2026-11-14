#include "ui/components/app_header.hpp"

#include <QHBoxLayout>
#include <QLabel>
#include <QPaintEvent>
#include <QPainter>
#include <QPen>
#include <QVBoxLayout>

#include "ui/theme.hpp"

namespace dv::ui {
namespace {

constexpr int kHeaderHeight = 64;

}  // namespace

AppHeader::AppHeader(QWidget* parent)
    : QWidget(parent),
      title_(new QLabel(this)),
      subtitle_(new QLabel(this)),
      beside_title_(new QHBoxLayout()),
      actions_(new QHBoxLayout()) {
  setFixedHeight(kHeaderHeight);

  theme::set_role(title_, theme::TextRole::Heading);
  theme::set_role(subtitle_, theme::TextRole::Caption);
  subtitle_->setVisible(false);

  auto* names = new QVBoxLayout();
  names->setContentsMargins(0, 0, 0, 0);
  names->setSpacing(0);
  names->addStretch();
  names->addWidget(title_);
  names->addWidget(subtitle_);
  names->addStretch();

  beside_title_->setContentsMargins(0, 0, 0, 0);
  beside_title_->setSpacing(theme::space::kSm);
  actions_->setContentsMargins(0, 0, 0, 0);
  actions_->setSpacing(theme::space::kSm);

  auto* row = new QHBoxLayout(this);
  row->setContentsMargins(theme::space::kXl, 0, theme::space::kXl, 0);
  row->setSpacing(theme::space::kMd);
  row->addLayout(names);
  row->addLayout(beside_title_);
  row->addStretch(1);
  row->addLayout(actions_);
}

void AppHeader::set_title(const QString& title) {
  title_->setText(title);
}

void AppHeader::set_subtitle(const QString& subtitle) {
  subtitle_->setText(subtitle);
  subtitle_->setVisible(!subtitle.isEmpty());
}

void AppHeader::paintEvent(QPaintEvent* /*event*/) {
  // A hairline under the bar and nothing else: the header is the top of the
  // page it belongs to, not a strip of a different colour laid over it.
  QPainter painter(this);
  painter.setPen(QPen(theme::colors().border, 1.0));
  painter.drawLine(0, height() - 1, width(), height() - 1);
}

}  // namespace dv::ui
