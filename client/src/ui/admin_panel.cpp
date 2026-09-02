#include "ui/admin_panel.hpp"

#include <dv/models/room.hpp>

#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

#include "ui/table.hpp"

namespace dv::ui {

AdminPanel::AdminPanel(client::app::CallSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
  auto* outer = new QVBoxLayout(this);

  auto* header = new QHBoxLayout();
  auto* title = new QLabel(QStringLiteral("Admin"), this);
  QFont bold = title->font();
  bold.setBold(true);
  title->setFont(bold);
  auto* back = new QPushButton(QStringLiteral("Back"), this);
  header->addWidget(title);
  header->addStretch();
  header->addWidget(back);
  outer->addLayout(header);

  tabs_ = new QTabWidget(this);
  tabs_->addTab(build_users_tab(), QStringLiteral("Users"));
  tabs_->addTab(build_rooms_tab(), QStringLiteral("Rooms"));
  tabs_->addTab(build_audit_tab(), QStringLiteral("Audit"));
  outer->addWidget(tabs_, 1);

  connect(back, &QPushButton::clicked, this, &AdminPanel::closed);
  connect(tabs_, &QTabWidget::currentChanged, this, &AdminPanel::on_tab_changed);
}

QWidget* AdminPanel::build_users_tab() {
  auto* page = new QWidget(this);
  auto* column = new QVBoxLayout(page);

  users_ = make_table(
      {QStringLiteral("Username"), QStringLiteral("Name"), QStringLiteral("Role"),
       QStringLiteral("Created"), QStringLiteral("Online"), QStringLiteral("Restricted")},
      page);
  column->addWidget(users_, 1);

  auto* controls = new QHBoxLayout();
  create_user_ = new QPushButton(QStringLiteral("New account"), page);
  create_user_->setProperty("accent", true);
  send_notice_ = new QPushButton(QStringLiteral("Message"), page);
  change_role_ = new QPushButton(QStringLiteral("Change role"), page);
  reset_password_ = new QPushButton(QStringLiteral("Reset password"), page);
  restrict_user_ = new QPushButton(QStringLiteral("Restrictions"), page);
  delete_user_ = new QPushButton(QStringLiteral("Delete"), page);
  delete_user_->setProperty("danger", true);
  controls->addWidget(create_user_);
  // Next to the account controls and not among the restrictions, which is
  // where it looks like it belongs. Telling somebody something is the one
  // thing on this row that takes nothing away from them, and an administrator
  // reaching for it after a warning rather than before one is the order this
  // feature exists to make possible.
  controls->addWidget(send_notice_);
  controls->addWidget(change_role_);
  controls->addWidget(reset_password_);
  controls->addWidget(restrict_user_);
  controls->addStretch();
  controls->addWidget(delete_user_);
  column->addLayout(controls);

  connect(create_user_, &QPushButton::clicked, this, &AdminPanel::on_create_user);
  connect(send_notice_, &QPushButton::clicked, this, &AdminPanel::on_send_notice);
  connect(change_role_, &QPushButton::clicked, this, &AdminPanel::on_change_role);
  connect(reset_password_, &QPushButton::clicked, this, &AdminPanel::on_reset_password);
  connect(restrict_user_, &QPushButton::clicked, this, &AdminPanel::on_restrict_user);
  connect(delete_user_, &QPushButton::clicked, this, &AdminPanel::on_delete_user);
  return page;
}

QWidget* AdminPanel::build_rooms_tab() {
  auto* page = new QWidget(this);
  auto* column = new QVBoxLayout(page);

  // No "Persistent" column: it read "yes" on every row from the moment every
  // room started outliving its participants, and a column with one value in it
  // is width spent on nothing.
  rooms_ =
      make_table({QStringLiteral("Room"), QStringLiteral("Name"), QStringLiteral("People")}, page);
  // The slack goes to Name, for the reason it does on the home page: a name is
  // as long as somebody made it, and People is "3/10" at its widest.
  rooms_->horizontalHeader()->setStretchLastSection(false);
  rooms_->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
  rooms_->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
  column->addWidget(rooms_, 1);

  auto* controls = new QHBoxLayout();
  create_room_ = new QPushButton(QStringLiteral("New room"), page);
  create_room_->setProperty("accent", true);
  close_room_ = new QPushButton(QStringLiteral("Close room"), page);
  close_room_->setProperty("danger", true);
  controls->addWidget(create_room_);
  controls->addStretch();
  controls->addWidget(close_room_);
  column->addLayout(controls);

  connect(create_room_, &QPushButton::clicked, this, &AdminPanel::on_create_room);
  connect(close_room_, &QPushButton::clicked, this, &AdminPanel::on_close_room);
  return page;
}

QWidget* AdminPanel::build_audit_tab() {
  auto* page = new QWidget(this);
  auto* column = new QVBoxLayout(page);

  audit_ = make_table({QStringLiteral("When"), QStringLiteral("Who"), QStringLiteral("Action"),
                       QStringLiteral("Target"), QStringLiteral("Room"), QStringLiteral("Detail")},
                      page);
  column->addWidget(audit_, 1);

  auto* note = new QLabel(
      // "And their answers": acknowledge_notice is written by the person who
      // read a notice, not by an administrator, and a footer that said only
      // administrators appear here would be contradicted by the row above it.
      QStringLiteral("Newest first. Administrative actions and their answers are recorded here."),
      page);
  note->setProperty("hint", true);
  column->addWidget(note);
  return page;
}

void AdminPanel::refresh() {
  // Three requests, but at most one complaint. A local failure here means the
  // socket is down, so the other two would fail identically, and three stacked
  // modal dialogs for one dropped connection is an obstacle rather than a
  // report.
  if (!send(session_.list_users())) {
    return;
  }
  if (!send(session_.list_rooms())) {
    return;
  }
  (void)send(session_.list_audit());
}

void AdminPanel::on_tab_changed(int /*index*/) {
  // The panel stays open while other people are doing things, so what it shows
  // goes stale on its own. Asking again when a tab is brought forward costs
  // one message and is the moment somebody is about to read it.
  refresh();
}

bool AdminPanel::send(const Result<std::monostate>& request) {
  if (!request) {
    emit failed(QString::fromStdString(request.error().code),
                QString::fromStdString(request.error().message));
    return false;
  }
  return true;
}

void AdminPanel::apply_users(const QStringList& rows) {
  // Read here and not out of the table afterwards, because the table keeps
  // only what it draws: ui::fill stops at the last column and the flags field
  // is past it. The row is the one MainWindow's on_user_list builds.
  accounts_.clear();
  accounts_.reserve(static_cast<int>(rows.size()));
  for (const QString& row : rows) {
    const QStringList fields = row.split(QLatin1Char('\t'));
    const QString id = fields.value(0);
    if (id.isEmpty()) {
      continue;
    }
    // One character per flag, in the order Restrictions declares them. Bounds
    // checked rather than indexed blindly: a row from an older client, or one
    // that lost its tail somewhere, should leave the boxes as they were and
    // not read off the end of a string.
    const QString flags = fields.value(7);
    const auto flag = [&flags](qsizetype at) {
      return flags.size() > at && flags.at(at) == QLatin1Char('1');
    };
    accounts_.insert(id, Account{
                             .username = fields.value(1),
                             .display_name = fields.value(2),
                             .role = models::role_from_string(fields.value(3).toStdString()),
                             .restrictions = models::Restrictions{.banned = flag(0),
                                                                  .muted = flag(1),
                                                                  .silenced = flag(2),
                                                                  .screen_share_blocked = flag(3)},
                         });
  }
  fill(users_, rows);
}

void AdminPanel::apply_rooms(const QStringList& rows) {
  fill(rooms_, rows);
}

void AdminPanel::apply_audit(const QStringList& rows) {
  fill(audit_, rows);
}

void AdminPanel::on_create_user() {
  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("New account"));
  auto* form = new QFormLayout(&dialog);

  auto* username = new QLineEdit(&dialog);
  auto* password = new QLineEdit(&dialog);
  password->setEchoMode(QLineEdit::Password);
  auto* display_name = new QLineEdit(&dialog);
  auto* role = new QComboBox(&dialog);
  role->addItem(QStringLiteral("User"));
  role->addItem(QStringLiteral("Administrator"));

  form->addRow(QStringLiteral("Username"), username);
  form->addRow(QStringLiteral("Password"), password);
  form->addRow(QStringLiteral("Display name"), display_name);
  form->addRow(QStringLiteral("Role"), role);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }
  if (username->text().isEmpty() || password->text().isEmpty()) {
    emit failed(QStringLiteral("invalid_value"),
                QStringLiteral("a username and a password are both required"));
    return;
  }

  (void)send(
      session_.create_user(username->text().toStdString(), password->text().toStdString(),
                           display_name->text().toStdString(),
                           role->currentIndex() == 1 ? models::Role::Admin : models::Role::User));
}

void AdminPanel::on_change_role() {
  const QString user_id = selected_id(users_);
  if (user_id.isEmpty()) {
    return;
  }

  // The role the server last reported, not the word the Role column happens to
  // be showing. Same reason the restrictions dialog reads from here.
  const auto account = accounts_.constFind(user_id);
  if (account == accounts_.constEnd()) {
    return;
  }

  protocol::UpdateUser change;
  change.user_id = user_id.toStdString();
  change.role = account->role == models::Role::Admin ? models::Role::User : models::Role::Admin;
  (void)send(session_.update_user(change));
}

void AdminPanel::on_reset_password() {
  const QString user_id = selected_id(users_);
  if (user_id.isEmpty()) {
    return;
  }

  bool accepted = false;
  const QString password =
      QInputDialog::getText(this, QStringLiteral("Reset password"), QStringLiteral("New password"),
                            QLineEdit::Password, QString(), &accepted);
  if (!accepted || password.isEmpty()) {
    return;
  }

  protocol::UpdateUser change;
  change.user_id = user_id.toStdString();
  change.password = password.toStdString();
  (void)send(session_.update_user(change));
}

QString AdminPanel::label_for(const QString& user_id) const {
  const auto account = accounts_.constFind(user_id);
  if (account == accounts_.constEnd()) {
    return user_id;
  }
  return QString::fromStdString(models::user_label(
      user_id.toStdString(), account->display_name.toStdString(), account->username.toStdString()));
}

void AdminPanel::on_send_notice() {
  const QString user_id = selected_id(users_);
  if (user_id.isEmpty()) {
    return;
  }
  const auto account = accounts_.constFind(user_id);
  if (account == accounts_.constEnd()) {
    return;
  }
  const QString username = account->username;

  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Message %1").arg(username));
  auto* column = new QVBoxLayout(&dialog);

  auto* intro = new QLabel(
      QStringLiteral("%1 sees this in a box they have to dismiss. If they are not signed in, "
                     "it waits for them and arrives the next time they are.")
          .arg(username),
      &dialog);
  // Wrapped, or the sentence sets the width of the dialog: a QLabel with one
  // long line asks for all of it, and the box comes up wider than the window
  // it was opened from.
  intro->setWordWrap(true);
  column->addWidget(intro);
  dialog.setMinimumWidth(420);

  auto* text = new QPlainTextEdit(&dialog);
  text->setPlaceholderText(QStringLiteral("What %1 should read").arg(username));
  // Four lines of room. Enough to see the whole of what fits, which is the
  // point of the limit below: a notice longer than this wants the room's chat.
  text->setFixedHeight(text->fontMetrics().lineSpacing() * 5);
  column->addWidget(text);

  auto* remaining = new QLabel(&dialog);
  // Quiet until it is not. `error` sits after `hint` in the stylesheet, so the
  // two carry equal specificity and the later one wins when both are set,
  // which is what lets the counter change colour by gaining one property
  // rather than by swapping two.
  remaining->setProperty("hint", true);
  column->addWidget(remaining);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  buttons->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Send"));
  column->addWidget(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  // The same rule the server applies, applied as the person types. It is the
  // one field of this dialog that can be wrong, and being told at the moment
  // it goes wrong is worth more than being refused after pressing Send.
  //
  // Bytes and not characters, because the limit is in bytes and an emoji is
  // four of them: a counter that promised 500 characters would refuse a
  // message it had just said was fine.
  const auto update = [text, remaining, buttons] {
    const std::string typed = text->toPlainText().toStdString();
    const std::string trimmed = models::trim_notice_text(typed);
    const auto limit = static_cast<qsizetype>(models::kMaxNoticeTextBytes);
    const auto used = static_cast<qsizetype>(trimmed.size());

    remaining->setText(QStringLiteral("%1 of %2 bytes").arg(used).arg(limit));
    remaining->setProperty("error", used > limit);
    // Qt applies a stylesheet at the moment a widget is polished, so a
    // property changed afterwards does nothing until the widget is asked to
    // look at itself again.
    remaining->style()->unpolish(remaining);
    remaining->style()->polish(remaining);

    buttons->button(QDialogButtonBox::Ok)->setEnabled(models::is_valid_notice_text(typed));
  };
  connect(text, &QPlainTextEdit::textChanged, &dialog, update);
  update();

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  // Nothing is shown here on the way out. The confirmation is the server
  // saying it wrote the notice down, which arrives as the notice itself and
  // reaches the status line through MainWindow::apply_notice_sent - and a
  // dialog that congratulated itself on having sent a message would be
  // claiming something it cannot know yet.
  (void)send(session_.send_notice(user_id.toStdString(), text->toPlainText().toStdString()));
}

void AdminPanel::on_restrict_user() {
  const QString user_id = selected_id(users_);
  if (user_id.isEmpty()) {
    return;
  }

  // The four flags as the server sent them.
  //
  // This used to take the text of column 5 and split it on spaces, looking for
  // the words models::describe writes. Two things were wrong with that and
  // both are quiet. The column number assumes nothing has shifted, and a value
  // carrying a tab shifted every column along - so the Restricted column would
  // be showing the Online one, and every box would open unchecked. And
  // describe() is documented as the display form, so the day its punctuation
  // changes the boxes open unchecked as well, with nothing failing to compile.
  //
  // Unchecked is the dangerous way to be wrong here, because the dialog sends
  // all four boxes on OK: an administrator opening it to add one restriction
  // would have silently lifted the three that were already there.
  const auto account = accounts_.constFind(user_id);
  if (account == accounts_.constEnd()) {
    return;
  }
  const QString username = account->username;
  const models::Restrictions current = account->restrictions;

  QDialog dialog(this);
  dialog.setWindowTitle(QStringLiteral("Restrictions for %1").arg(username));
  auto* form = new QFormLayout(&dialog);

  form->addRow(new QLabel(QStringLiteral("These stay with the account until they are lifted, "
                                         "across rooms and across sign ins."),
                          &dialog));

  auto* banned = new QCheckBox(QStringLiteral("Cannot sign in"), &dialog);
  auto* muted = new QCheckBox(QStringLiteral("Cannot use the microphone"), &dialog);
  auto* silenced = new QCheckBox(QStringLiteral("Cannot write in the chat"), &dialog);
  auto* blocked = new QCheckBox(QStringLiteral("Cannot share their screen"), &dialog);
  banned->setChecked(current.banned);
  muted->setChecked(current.muted);
  silenced->setChecked(current.silenced);
  blocked->setChecked(current.screen_share_blocked);

  form->addRow(banned);
  form->addRow(muted);
  form->addRow(silenced);
  form->addRow(blocked);

  auto* reason = new QLineEdit(&dialog);
  reason->setPlaceholderText(QStringLiteral("Shown to %1").arg(username));
  form->addRow(QStringLiteral("Reason"), reason);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
  form->addRow(buttons);
  connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
  connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

  if (dialog.exec() != QDialog::Accepted) {
    return;
  }

  // Every box is sent, because this dialog is the one place all four are
  // decided together and the administrator has just looked at each of them.
  // The per participant shortcuts in the room send one flag at a time, which
  // is the case where leaving the rest absent matters.
  protocol::RestrictUser change;
  change.user_id = user_id.toStdString();
  change.banned = banned->isChecked();
  change.muted = muted->isChecked();
  change.silenced = silenced->isChecked();
  change.screen_share_blocked = blocked->isChecked();
  change.reason = reason->text().toStdString();
  (void)send(session_.restrict_user(change));
}

void AdminPanel::on_delete_user() {
  const QString user_id = selected_id(users_);
  if (user_id.isEmpty()) {
    return;
  }

  // From the same place the other two take it, so that the name in the
  // question is the name of the account the request will actually delete and
  // not whatever the Username column is drawing in that row.
  const auto account = accounts_.constFind(user_id);
  if (account == accounts_.constEnd()) {
    return;
  }
  const QString username = account->username;
  // Deleting an account ends their session and cannot be undone from here, so
  // it is the one action in this panel that asks first.
  if (QMessageBox::question(this, QStringLiteral("Delete account"),
                            QStringLiteral("Delete '%1'? They will be removed from any room "
                                           "they are in and signed out immediately.")
                                .arg(username)) != QMessageBox::Yes) {
    return;
  }
  (void)send(session_.delete_user(user_id.toStdString()));
}

void AdminPanel::on_create_room() {
  bool accepted = false;
  const QString name =
      QInputDialog::getText(this, QStringLiteral("New room"), QStringLiteral("Room name"),
                            QLineEdit::Normal, QString(), &accepted);
  if (!accepted) {
    return;
  }
  // A second question rather than a form, because this panel asks for one
  // thing at a time everywhere else and a room is two answers, not ten. The
  // range is the protocol's; the server may allow less and answers with the
  // range it does allow.
  const int capacity = QInputDialog::getInt(this, QStringLiteral("New room"),
                                            QStringLiteral("How many people the room holds"),
                                            models::kDefaultRoomCapacity, models::kMinRoomCapacity,
                                            models::kMaxRoomCapacity, 1, &accepted);
  if (!accepted) {
    return;
  }
  // The identifier arrives through room_created, and the panel is refreshed
  // from there rather than here: asking for the list now would race the
  // creation and show the state from before it.
  (void)send(session_.create_room(name.toStdString(), true, capacity));
}

void AdminPanel::on_close_room() {
  const QString room_id = selected_id(rooms_);
  if (room_id.isEmpty()) {
    return;
  }

  if (QMessageBox::question(
          this, QStringLiteral("Close room"),
          QStringLiteral("Close room %1? Everyone in it is removed.").arg(room_id)) !=
      QMessageBox::Yes) {
    return;
  }
  (void)send(session_.delete_room(room_id.toStdString()));
}

}  // namespace dv::ui
