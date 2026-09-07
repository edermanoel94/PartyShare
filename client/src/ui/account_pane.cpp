#include "ui/account_pane.hpp"

#include <QCheckBox>
#include <QFont>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStyle>
#include <QVBoxLayout>
#include <QWidget>

namespace dv::ui {
namespace {

/// How wide the pane is. Fixed rather than shared with the table through a
/// splitter: what it holds is a name, four boxes and a few lines, none of
/// which read better wider, and every pixel it does not take is a column the
/// table can show.
constexpr int kPaneWidth = 300;

/// A one pixel rule between the pane's sections. A QFrame with a name the
/// stylesheet knows, so its colour follows the scheme like every other line.
[[nodiscard]] QFrame* make_rule(QWidget* parent) {
  auto* rule = new QFrame(parent);
  rule->setObjectName(QStringLiteral("rule"));
  rule->setFrameShape(QFrame::NoFrame);
  rule->setFixedHeight(1);
  return rule;
}

/// A small capitals heading over a section.
[[nodiscard]] QLabel* make_eyebrow(const QString& text, QWidget* parent) {
  auto* label = new QLabel(text.toUpper(), parent);
  label->setProperty("eyebrow", true);
  return label;
}

/// A button that is a line of text until the pointer is on it. The pane is
/// narrow and holds four of these; four bordered boxes would be a second
/// toolbar. `tone` is what the stylesheet colours them by.
[[nodiscard]] QPushButton* make_quiet(const QString& text, const QString& tone, QWidget* parent) {
  auto* button = new QPushButton(text, parent);
  button->setProperty("quiet", tone);
  button->setCursor(Qt::PointingHandCursor);
  return button;
}

/// Asks the stylesheet to look at a widget again after one of its properties
/// changed. Qt resolves the sheet when a widget is polished, and a property
/// set afterwards does nothing until this.
void repolish(QWidget* widget) {
  widget->style()->unpolish(widget);
  widget->style()->polish(widget);
}

}  // namespace

AccountPane::AccountPane(QWidget* parent) : QFrame(parent) {
  setObjectName(QStringLiteral("accountPane"));
  setFixedWidth(kPaneWidth);

  auto* outer = new QVBoxLayout(this);
  outer->setContentsMargins(14, 14, 14, 14);
  outer->setSpacing(0);

  empty_ = new QLabel(QStringLiteral("Select an account to see it here."), this);
  empty_->setProperty("hint", true);
  empty_->setWordWrap(true);
  empty_->setAlignment(Qt::AlignTop | Qt::AlignLeft);
  outer->addWidget(empty_);

  body_ = new QWidget(this);
  auto* column = new QVBoxLayout(body_);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(10);
  outer->addWidget(body_);
  outer->addStretch();

  // Who.
  name_ = new QLabel(body_);
  QFont big = name_->font();
  big.setPointSizeF(big.pointSizeF() * 1.25);
  big.setBold(true);
  name_->setFont(big);
  handle_ = new QLabel(body_);
  handle_->setProperty("hint", true);
  handle_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  role_ = new QLabel(body_);
  role_->setProperty("eyebrow", true);
  auto* who = new QVBoxLayout();
  who->setSpacing(2);
  who->addWidget(name_);
  who->addWidget(handle_);
  who->addWidget(role_);
  column->addLayout(who);

  auto* facts = new QFormLayout();
  facts->setContentsMargins(0, 0, 0, 0);
  facts->setHorizontalSpacing(10);
  facts->setVerticalSpacing(3);
  created_ = new QLabel(body_);
  status_ = new QLabel(body_);
  auto* created_key = new QLabel(QStringLiteral("Created"), body_);
  created_key->setProperty("hint", true);
  auto* status_key = new QLabel(QStringLiteral("Status"), body_);
  status_key->setProperty("hint", true);
  facts->addRow(created_key, created_);
  facts->addRow(status_key, status_);
  column->addLayout(facts);

  column->addWidget(make_rule(body_));

  // What can be done. Hidden as one on the administrator's own row.
  controls_ = new QWidget(body_);
  auto* controls = new QVBoxLayout(controls_);
  controls->setContentsMargins(0, 0, 0, 0);
  controls->setSpacing(10);

  controls->addWidget(make_eyebrow(QStringLiteral("Restrictions"), controls_));
  banned_ = new QCheckBox(QStringLiteral("Cannot sign in"), controls_);
  muted_ = new QCheckBox(QStringLiteral("Cannot use the microphone"), controls_);
  silenced_ = new QCheckBox(QStringLiteral("Cannot write in the chat"), controls_);
  blocked_ = new QCheckBox(QStringLiteral("Cannot share their screen"), controls_);
  auto* boxes = new QVBoxLayout();
  boxes->setSpacing(4);
  for (QCheckBox* box : {banned_, muted_, silenced_, blocked_}) {
    boxes->addWidget(box);
  }
  controls->addLayout(boxes);

  reason_ = new QLineEdit(controls_);
  controls->addWidget(reason_);
  apply_ = new QPushButton(QStringLiteral("Apply"), controls_);
  apply_->setProperty("accent", true);
  apply_->setToolTip(
      QStringLiteral("Sends all four boxes as they are. These stay with the account until "
                     "they are lifted, across rooms and across sign ins."));
  controls->addWidget(apply_);

  controls->addWidget(make_rule(controls_));

  message_ = make_quiet(QStringLiteral("Message..."), QStringLiteral("accent"), controls_);
  role_change_ = make_quiet(QString(), QStringLiteral("accent"), controls_);
  password_ = make_quiet(QStringLiteral("Reset password..."), QStringLiteral("accent"), controls_);
  remove_ = make_quiet(QStringLiteral("Delete..."), QStringLiteral("danger"), controls_);
  auto* actions = new QGridLayout();
  actions->setContentsMargins(0, 0, 0, 0);
  actions->setHorizontalSpacing(4);
  actions->setVerticalSpacing(2);
  actions->addWidget(message_, 0, 0);
  actions->addWidget(role_change_, 0, 1);
  actions->addWidget(password_, 1, 0);
  actions->addWidget(remove_, 1, 1);
  controls->addLayout(actions);
  column->addWidget(controls_);

  self_note_ = new QLabel(
      QStringLiteral("This is your own account. The server refuses every change to it from "
                     "here, so that nobody can lock themselves out."),
      body_);
  self_note_->setProperty("hint", true);
  self_note_->setWordWrap(true);
  column->addWidget(self_note_);

  column->addWidget(make_rule(body_));

  column->addWidget(make_eyebrow(QStringLiteral("Audit"), body_));
  audit_ = new QLabel(body_);
  audit_->setProperty("hint", true);
  audit_->setWordWrap(true);
  audit_->setTextInteractionFlags(Qt::TextSelectableByMouse);
  column->addWidget(audit_);

  connect(apply_, &QPushButton::clicked, this, &AccountPane::on_apply);
  // Return in the reason field is Apply: the reason is the last thing typed
  // before the decision, and reaching for the mouse after it is a step.
  connect(reason_, &QLineEdit::returnPressed, this, &AccountPane::on_apply);
  connect(message_, &QPushButton::clicked, this, &AccountPane::message_requested);
  connect(role_change_, &QPushButton::clicked, this, &AccountPane::role_change_requested);
  connect(password_, &QPushButton::clicked, this, &AccountPane::password_reset_requested);
  connect(remove_, &QPushButton::clicked, this, &AccountPane::delete_requested);

  show_nothing();
}

void AccountPane::show_account(const AccountView& account) {
  // The reason belongs to one decision about one account: kept while the same
  // account is refreshed under it, which the server does after every change
  // and every arrival, and dropped when another one is picked.
  if (id_ != account.id) {
    reason_->clear();
  }
  id_ = account.id;
  empty_->setVisible(false);
  body_->setVisible(true);

  const QString name = account.display_name.isEmpty() ? account.username : account.display_name;
  name_->setText(name);
  // The first eight characters of the identifier, which is a 32 character
  // hash and would run off the pane; the whole of it is in the tooltip and
  // can be selected from there. Eight is what the audit log and the server
  // log show, so the two can be matched by eye.
  handle_->setText(QStringLiteral("@%1 · %2…").arg(account.username, account.id.left(8)));
  handle_->setToolTip(account.id);
  role_->setText(account.admin ? QStringLiteral("ADMINISTRATOR") : QStringLiteral("USER"));
  // The accent for an administrator, like the table's role column: the
  // exception is what should catch the eye.
  role_->setProperty("accent", account.admin);
  repolish(role_);
  created_->setText(account.created.isEmpty() ? QStringLiteral("unknown") : account.created);
  status_->setText(account.online ? QStringLiteral("online now") : QStringLiteral("offline"));

  banned_->setChecked(account.restrictions.banned);
  muted_->setChecked(account.restrictions.muted);
  silenced_->setChecked(account.restrictions.silenced);
  blocked_->setChecked(account.restrictions.screen_share_blocked);
  reason_->setPlaceholderText(QStringLiteral("Reason, shown to %1").arg(name));
  role_change_->setText(account.admin ? QStringLiteral("Make a user")
                                      : QStringLiteral("Make an administrator"));

  controls_->setVisible(!account.is_me);
  self_note_->setVisible(account.is_me);
}

void AccountPane::show_audit(const QStringList& lines) {
  audit_->setText(lines.isEmpty() ? QStringLiteral("Nothing recorded about this account yet.")
                                  : lines.join(QLatin1Char('\n')));
}

void AccountPane::show_nothing() {
  id_.clear();
  empty_->setVisible(true);
  body_->setVisible(false);
}

void AccountPane::focus_restrictions() {
  if (!id_.isEmpty() && controls_->isVisible()) {
    banned_->setFocus(Qt::TabFocusReason);
  }
}

void AccountPane::on_apply() {
  if (id_.isEmpty()) {
    return;
  }
  protocol::RestrictUser change;
  change.user_id = id_.toStdString();
  change.banned = banned_->isChecked();
  change.muted = muted_->isChecked();
  change.silenced = silenced_->isChecked();
  change.screen_share_blocked = blocked_->isChecked();
  change.reason = reason_->text().toStdString();
  emit restrict_requested(change);
  // The confirmation is the new user list, which puts the boxes back to
  // whatever the server decided; the reason has been said and goes.
  reason_->clear();
}

}  // namespace dv::ui
