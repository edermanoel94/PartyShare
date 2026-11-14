#include "ui/components/app_dialog.hpp"

#include <QEasingCurve>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QShowEvent>
#include <QVBoxLayout>

#include "ui/components/buttons.hpp"
#include "ui/theme.hpp"

namespace dv::ui {

AppDialog::AppDialog(const QString& title, QWidget* parent)
    : QDialog(parent),
      description_(new QLabel(this)),
      body_(new QVBoxLayout()),
      buttons_(new QHBoxLayout()) {
  setWindowTitle(title);
  setModal(true);

  auto* heading = new QLabel(title, this);
  theme::set_role(heading, theme::TextRole::Heading);

  description_->setWordWrap(true);
  description_->setProperty("hint", true);
  description_->setVisible(false);

  body_->setContentsMargins(0, 0, 0, 0);
  body_->setSpacing(theme::space::kMd);
  buttons_->setContentsMargins(0, 0, 0, 0);
  buttons_->setSpacing(theme::space::kSm);
  buttons_->addStretch();

  auto* column = new QVBoxLayout(this);
  column->setContentsMargins(theme::space::kXl, theme::space::kXl, theme::space::kXl,
                             theme::space::kXl);
  column->setSpacing(theme::space::kSm);
  column->addWidget(heading);
  column->addWidget(description_);
  column->addSpacing(theme::space::kSm);
  column->addLayout(body_, 1);
  column->addSpacing(theme::space::kMd);
  column->addLayout(buttons_);
}

void AppDialog::set_description(const QString& text) {
  description_->setText(text);
  description_->setVisible(!text.isEmpty());
}

QPushButton* AppDialog::add_primary(const QString& text) {
  auto* button = new PrimaryButton(text, this);
  button->setDefault(true);
  buttons_->addWidget(button);
  connect(button, &QPushButton::clicked, this, &QDialog::accept);
  return button;
}

QPushButton* AppDialog::add_secondary(const QString& text) {
  auto* button = new SecondaryButton(text, this);
  // Before the primary one if it is already there: the answer stays last,
  // where the eye and the pointer finish.
  const bool primary_placed = buttons_->count() > 1 && !has_secondary_;
  buttons_->insertWidget(primary_placed ? buttons_->count() - 1 : buttons_->count(), button);
  if (!has_secondary_) {
    connect(button, &QPushButton::clicked, this, &QDialog::reject);
  }
  has_secondary_ = true;
  return button;
}

void AppDialog::showEvent(QShowEvent* event) {
  QDialog::showEvent(event);
  const int duration = theme::motion::duration(theme::motion::kFastMs);
  if (duration == 0) {
    return;
  }
  // The window's own opacity, which the compositor applies: no graphics
  // effect, and nothing of the dialog is rendered twice.
  auto* fade = new QPropertyAnimation(this, "windowOpacity", this);
  fade->setDuration(duration);
  fade->setStartValue(0.0);
  fade->setEndValue(1.0);
  fade->setEasingCurve(QEasingCurve::OutCubic);
  fade->start(QAbstractAnimation::DeleteWhenStopped);
}

AppForm::AppForm(QWidget* parent)
    : QWidget(parent), form_(new QFormLayout(this)), error_(new QLabel(this)) {
  form_->setContentsMargins(0, 0, 0, 0);
  form_->setHorizontalSpacing(theme::space::kLg);
  form_->setVerticalSpacing(theme::space::kMd);
  form_->setLabelAlignment(Qt::AlignLeft | Qt::AlignVCenter);
  // The fields take the width; a form whose fields stay at their size hint
  // is a ragged right edge.
  form_->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);

  error_->setWordWrap(true);
  error_->setProperty("error", true);
  error_->setVisible(false);
}

void AppForm::add_row(const QString& label, QWidget* field) {
  form_->addRow(label, field);
}

void AppForm::add_wide(QWidget* widget) {
  form_->addRow(widget);
}

QLabel* AppForm::add_hint(const QString& text) {
  auto* hint = new QLabel(text, this);
  hint->setWordWrap(true);
  hint->setProperty("hint", true);
  form_->addRow(hint);
  return hint;
}

void AppForm::show_error(const QString& text) {
  // Added the first time there is something to say, so it is always the last
  // row whatever was added after the form was built.
  if (form_->indexOf(error_) < 0) {
    form_->addRow(error_);
  }
  error_->setText(text);
  error_->setVisible(!text.isEmpty());
}

}  // namespace dv::ui
