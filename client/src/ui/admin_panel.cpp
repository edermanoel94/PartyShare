#include "ui/admin_panel.hpp"

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
#include <QPushButton>
#include <QTabWidget>
#include <QTableWidget>
#include <QVBoxLayout>

namespace dv::ui {
namespace {

/// Where the identifier of a row lives, the same idea as the participant list.
constexpr int kIdRole = Qt::UserRole;

/// Fills a table from rows of tab separated fields.
///
/// The first field is the identifier and is not shown; the rest are the
/// columns, in order. Rebuilt wholesale on every answer, because the answer is
/// the whole list and merging it into what is on screen would be a second
/// implementation of the truth.
void fill(QTableWidget* table, const QStringList& rows) {
  const QString selected =
      table->currentRow() >= 0 && table->item(table->currentRow(), 0) != nullptr
          ? table->item(table->currentRow(), 0)->data(kIdRole).toString()
          : QString();

  table->setRowCount(static_cast<int>(rows.size()));
  for (int row = 0; row < rows.size(); ++row) {
    const QStringList fields = rows.at(row).split(QLatin1Char('\t'));
    for (int column = 0; column < table->columnCount(); ++column) {
      // The identifier is field 0, so the columns start at 1.
      auto* item = new QTableWidgetItem(fields.value(column + 1));
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
      if (column == 0) {
        item->setData(kIdRole, fields.value(0));
      }
      table->setItem(row, column, item);
    }
    // Restored by identity rather than by row: the order changes as accounts
    // and rooms come and go, and a selection that jumps to a different account
    // between a refresh and a click is how the wrong person gets deleted.
    if (!selected.isEmpty() && fields.value(0) == selected) {
      table->selectRow(row);
    }
  }
}

QTableWidget* make_table(const QStringList& headers, QWidget* parent) {
  auto* table = new QTableWidget(0, static_cast<int>(headers.size()), parent);
  table->setHorizontalHeaderLabels(headers);
  table->setSelectionBehavior(QAbstractItemView::SelectRows);
  table->setSelectionMode(QAbstractItemView::SingleSelection);
  table->setEditTriggers(QAbstractItemView::NoEditTriggers);
  table->verticalHeader()->setVisible(false);
  table->horizontalHeader()->setStretchLastSection(true);
  return table;
}

}  // namespace

AdminPanel::AdminPanel(client::app::CallSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
  auto* outer = new QVBoxLayout(this);

  auto* header = new QHBoxLayout();
  auto* title = new QLabel(QStringLiteral("Administration"), this);
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

  users_ = make_table({QStringLiteral("Username"), QStringLiteral("Name"), QStringLiteral("Role"),
                       QStringLiteral("Created"), QStringLiteral("Online")},
                      page);
  column->addWidget(users_, 1);

  auto* controls = new QHBoxLayout();
  create_user_ = new QPushButton(QStringLiteral("New account"), page);
  create_user_->setProperty("accent", true);
  change_role_ = new QPushButton(QStringLiteral("Change role"), page);
  reset_password_ = new QPushButton(QStringLiteral("Reset password"), page);
  delete_user_ = new QPushButton(QStringLiteral("Delete"), page);
  delete_user_->setProperty("danger", true);
  controls->addWidget(create_user_);
  controls->addWidget(change_role_);
  controls->addWidget(reset_password_);
  controls->addStretch();
  controls->addWidget(delete_user_);
  column->addLayout(controls);

  connect(create_user_, &QPushButton::clicked, this, &AdminPanel::on_create_user);
  connect(change_role_, &QPushButton::clicked, this, &AdminPanel::on_change_role);
  connect(reset_password_, &QPushButton::clicked, this, &AdminPanel::on_reset_password);
  connect(delete_user_, &QPushButton::clicked, this, &AdminPanel::on_delete_user);
  return page;
}

QWidget* AdminPanel::build_rooms_tab() {
  auto* page = new QWidget(this);
  auto* column = new QVBoxLayout(page);

  rooms_ = make_table({QStringLiteral("Room"), QStringLiteral("Name"), QStringLiteral("People"),
                       QStringLiteral("Persistent")},
                      page);
  column->addWidget(rooms_, 1);

  auto* controls = new QHBoxLayout();
  create_room_ = new QPushButton(QStringLiteral("New persistent room"), page);
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
      QStringLiteral("Newest first. Only administrative actions are recorded here."), page);
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

QString AdminPanel::selected_id(const QTableWidget* table) {
  const int row = table->currentRow();
  if (row < 0 || table->item(row, 0) == nullptr) {
    return {};
  }
  return table->item(row, 0)->data(kIdRole).toString();
}

void AdminPanel::apply_users(const QStringList& rows) {
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

  const QString current = users_->item(users_->currentRow(), 2)->text();
  const bool promoting = current != QStringLiteral("admin");

  protocol::UpdateUser change;
  change.user_id = user_id.toStdString();
  change.role = promoting ? models::Role::Admin : models::Role::User;
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

void AdminPanel::on_delete_user() {
  const QString user_id = selected_id(users_);
  if (user_id.isEmpty()) {
    return;
  }

  const QString username = users_->item(users_->currentRow(), 0)->text();
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
      QInputDialog::getText(this, QStringLiteral("New persistent room"),
                            QStringLiteral("Room name"), QLineEdit::Normal, QString(), &accepted);
  if (!accepted) {
    return;
  }
  // The identifier arrives through room_created, and the panel is refreshed
  // from there rather than here: asking for the list now would race the
  // creation and show the state from before it.
  (void)send(session_.create_room(name.toStdString(), true));
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
