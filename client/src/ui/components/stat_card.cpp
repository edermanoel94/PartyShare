#include "ui/components/stat_card.hpp"

#include <QColor>
#include <QLabel>
#include <QVBoxLayout>

#include "ui/theme.hpp"

namespace dv::ui {

Card::Card(QWidget* parent) : QFrame(parent), body_(new QVBoxLayout(this)) {
  setProperty("card", true);
  body_->setContentsMargins(theme::space::kLg, theme::space::kLg, theme::space::kLg,
                            theme::space::kLg);
  body_->setSpacing(theme::space::kSm);
}

StatCard::StatCard(const QString& label, QWidget* parent) : Card(parent) {
  auto* caption = new QLabel(label, this);
  theme::set_role(caption, theme::TextRole::Caption);

  value_ = new QLabel(QStringLiteral("–"), this);
  theme::set_role(value_, theme::TextRole::Display);

  status_ = new QLabel(QString{}, this);
  theme::set_role(status_, theme::TextRole::Caption);
  status_->setVisible(false);

  body()->setSpacing(theme::space::kXs);
  body()->addWidget(caption);
  body()->addWidget(value_);
  body()->addWidget(status_);
  // From the top down. A row of these shares one height, and a card with no
  // status line would otherwise centre its two lines while the card beside
  // it starts its three at the top - the same label at two heights.
  body()->addStretch();

  // The status line is coloured inline, and inline colours are as they were
  // when they were written.
  connect(theme::notifier(), &theme::Notifier::changed, this,
          [this] { set_status(status_text_, tone_); });
}

void StatCard::set_value(const QString& value) {
  value_->setText(value);
}

void StatCard::set_status(const QString& text, Tone tone) {
  tone_ = tone;
  status_text_ = text;
  status_->setVisible(!text.isEmpty());
  if (text.isEmpty()) {
    return;
  }
  const theme::Colors& colours = theme::colors();
  QColor colour = colours.muted;
  switch (tone) {
    case Tone::Good:
      colour = colours.success;
      break;
    case Tone::Fair:
      colour = colours.warn;
      break;
    case Tone::Poor:
      colour = colours.danger;
      break;
    case Tone::Neutral:
      break;
  }
  // A dot and the word, in the same colour. The dot is a character rather
  // than a painted circle so that it sits on the text's own baseline.
  status_->setText(QStringLiteral("<span style=\"color:%1\">● %2</span>")
                       .arg(colour.name(), text.toHtmlEscaped()));
}

}  // namespace dv::ui
