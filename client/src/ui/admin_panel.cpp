#include "ui/admin_panel.hpp"

#include <array>
#include <utility>

#include <dv/models/room.hpp>

#include <QAction>
#include <QBrush>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QEvent>
#include <QFont>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QModelIndex>
#include <QPlainTextEdit>
#include <QPoint>
#include <QPushButton>
#include <QStackedWidget>
#include <QStyle>
#include <QTabBar>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>

#include "ui/account_delegate.hpp"
#include "ui/table.hpp"
#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The columns of the account table, in order. Named because the fill puts
/// something different in each and a column number says nothing on its own.
constexpr int kStatusColumn = 0;
constexpr int kUserColumn = 1;
constexpr int kNameColumn = 2;
constexpr int kRoleColumn = 3;
constexpr int kRestrictionsColumn = 4;
constexpr int kCreatedColumn = 5;

/// How many audit lines the pane shows about one account. Five is what fits
/// under the actions without the pane growing a scrollbar, and the tab next
/// door has the rest.
constexpr int kAuditTail = 5;

/// The columns of the session table, in order. The same five tools/dbadmin
/// shows, in its order: the account and whether they are here are the whole
/// question, the address is the other half of what the screen is for, and of
/// the two times the one that decides whether the state is true comes first.
constexpr int kSessionAccountColumn = 0;
constexpr int kSessionStateColumn = 1;
constexpr int kSessionAddressColumn = 2;
constexpr int kSessionSeenColumn = 3;
constexpr int kSessionConnectedColumn = 4;

/// Carried on a session row's first cell beside kIdRole, which holds the
/// session: the account it belongs to, which is what ending it names, and the
/// state it was drawn in, which is what decides whether it can be.
constexpr int kSessionUserRole = Qt::UserRole + 4;
constexpr int kSessionStateRole = Qt::UserRole + 5;

/// A time the server never wrote, or an address the transport did not give,
/// as a dash rather than as a blank: a blank reads as a cell that failed.
[[nodiscard]] QString or_dash(const QString& text) {
  return text.isEmpty() ? QStringLiteral("-") : text;
}

}  // namespace

AdminPanel::AdminPanel(client::app::CallSession& session, QWidget* parent)
    : QWidget(parent), session_(session) {
  auto* outer = new QVBoxLayout(this);
  outer->setSpacing(10);

  // One line: the title, the tabs and the way out, on one baseline and
  // against the same two margins as everything under them. The tabs used to
  // sit on a line of their own over a framed pane, with Back a line above
  // and the page's own buttons a frame's width inside it: three rows and
  // three right edges, none of which agreed.
  auto* header = new QHBoxLayout();
  header->setSpacing(12);
  auto* title = new QLabel(QStringLiteral("Admin"), this);
  QFont bold = title->font();
  bold.setBold(true);
  title->setFont(bold);
  tabs_ = new QTabBar(this);
  // A bare tab bar stretches its tabs across whatever width it is given, and
  // draws a base line under them for a pane that is not there. These are
  // chips, sized to their word.
  tabs_->setExpanding(false);
  tabs_->setDrawBase(false);
  tabs_->addTab(QStringLiteral("Users"));
  tabs_->addTab(QStringLiteral("Rooms"));
  tabs_->addTab(QStringLiteral("Sessions"));
  tabs_->addTab(QStringLiteral("Audit"));
  auto* back = new QPushButton(QStringLiteral("Back"), this);
  header->addWidget(title);
  header->addWidget(tabs_);
  header->addStretch();
  header->addWidget(back);
  outer->addLayout(header);

  // The pages, in the order of the tabs. No frame around them: the tables and
  // the pane are the cards, and a card around cards was a second set of
  // corners.
  pages_ = new QStackedWidget(this);
  pages_->addWidget(build_users_tab());
  pages_->addWidget(build_rooms_tab());
  pages_->addWidget(build_sessions_tab());
  pages_->addWidget(build_audit_tab());
  outer->addWidget(pages_, 1);

  connect(back, &QPushButton::clicked, this, &AdminPanel::closed);
  // Connected once the pages exist: a tab bar announces its first tab as it
  // is added, and the slot turns a page that would not be there yet.
  connect(tabs_, &QTabBar::currentChanged, this, &AdminPanel::on_tab_changed);
}

QWidget* AdminPanel::build_users_tab() {
  auto* page = new QWidget(this);
  auto* column = new QVBoxLayout(page);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(8);

  // The filter line, with the one button that is about no account in
  // particular beside it, flush with Back on the line above.
  auto* top = new QHBoxLayout();
  top->setSpacing(8);
  filter_ = new QLineEdit(page);
  filter_->setProperty("console", true);
  filter_->setPlaceholderText(QStringLiteral("Filter by username, name, role or restriction"));
  filter_->setClearButtonEnabled(true);
  filter_count_ = new QLabel(page);
  filter_count_->setProperty("hint", true);
  create_user_ = new QPushButton(QStringLiteral("New account"), page);
  create_user_->setProperty("accent", true);
  top->addWidget(filter_, 1);
  top->addWidget(filter_count_);
  top->addWidget(create_user_);
  column->addLayout(top);

  auto* body = new QHBoxLayout();
  body->setSpacing(10);
  users_ =
      make_table({QString(), QStringLiteral("User"), QStringLiteral("Name"), QStringLiteral("Role"),
                  QStringLiteral("Restrictions"), QStringLiteral("Created")},
                 page);
  users_->setItemDelegate(new AccountDelegate(users_));
  // No grid: the rows are read across, and a lattice between cells that hold
  // a dot and a chip is lines for their own sake.
  users_->setShowGrid(false);
  // The slack goes to the name, which is as long as somebody made it. The
  // dot and the chips are sized to what they draw.
  QHeaderView* columns = users_->horizontalHeader();
  columns->setStretchLastSection(false);
  columns->setSectionResizeMode(QHeaderView::ResizeToContents);
  columns->setSectionResizeMode(kNameColumn, QHeaderView::Stretch);
  columns->setMinimumSectionSize(20);
  // The menu is built on demand from the row under the pointer, so there is
  // nothing to enable or disable here.
  users_->setContextMenuPolicy(Qt::CustomContextMenu);
  pane_ = new AccountPane(page);
  body->addWidget(users_, 1);
  body->addWidget(pane_);
  column->addLayout(body, 1);

  // What the keys do, written where a terminal writes it. They work from the
  // table, which is where the arrows already do.
  auto* keys = new QLabel(QStringLiteral("/ filter · ↑↓ move · ⏎ open · m message · r restrict · "
                                         "b ban · p password · d delete · n new account"),
                          page);
  keys->setProperty("hint", true);
  column->addWidget(keys);

  connect(create_user_, &QPushButton::clicked, this, &AdminPanel::on_create_user);
  connect(filter_, &QLineEdit::textChanged, this, &AdminPanel::on_filter_changed);
  connect(users_, &QTableWidget::itemSelectionChanged, this, &AdminPanel::on_account_selected);
  connect(users_, &QTableWidget::customContextMenuRequested, this, &AdminPanel::on_user_menu);
  connect(pane_, &AccountPane::restrict_requested, this, &AdminPanel::on_restrict);
  connect(pane_, &AccountPane::message_requested, this, &AdminPanel::on_send_notice);
  connect(pane_, &AccountPane::role_change_requested, this, &AdminPanel::on_change_role);
  connect(pane_, &AccountPane::password_reset_requested, this, &AdminPanel::on_reset_password);
  connect(pane_, &AccountPane::delete_requested, this, &AdminPanel::on_delete_user);

  users_->installEventFilter(this);
  filter_->installEventFilter(this);

  on_filter_changed(QString());
  return page;
}

QWidget* AdminPanel::build_rooms_tab() {
  auto* page = new QWidget(this);
  auto* column = new QVBoxLayout(page);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(8);

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

QWidget* AdminPanel::build_sessions_tab() {
  auto* page = new QWidget(this);
  auto* column = new QVBoxLayout(page);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(8);

  sessions_ =
      make_table({QStringLiteral("Account"), QStringLiteral("State"), QStringLiteral("Address"),
                  QStringLiteral("Last seen"), QStringLiteral("Connected")},
                 page);
  sessions_->setShowGrid(false);
  // The slack goes to the address, which is the one column whose width is
  // not known in advance: an IPv6 address is as long as it is.
  QHeaderView* columns = sessions_->horizontalHeader();
  columns->setStretchLastSection(false);
  columns->setSectionResizeMode(QHeaderView::ResizeToContents);
  columns->setSectionResizeMode(kSessionAddressColumn, QHeaderView::Stretch);
  column->addWidget(sessions_, 1);

  // How many are here, and the one thing this screen can do about a row.
  auto* controls = new QHBoxLayout();
  sessions_count_ = new QLabel(page);
  sessions_count_->setProperty("hint", true);
  end_session_ = new QPushButton(QStringLiteral("End session..."), page);
  end_session_->setProperty("danger", true);
  end_session_->setToolTip(
      QStringLiteral("Signs them out at once. Nothing is taken from the account, and they may "
                     "sign in again straight away."));
  controls->addWidget(sessions_count_);
  controls->addStretch();
  controls->addWidget(end_session_);
  column->addLayout(controls);

  auto* keys = new QLabel(QStringLiteral("↑↓ move · k end session"), page);
  keys->setProperty("hint", true);
  column->addWidget(keys);

  connect(end_session_, &QPushButton::clicked, this, &AdminPanel::on_end_session);
  sessions_->installEventFilter(this);

  fill_sessions();
  return page;
}

QWidget* AdminPanel::build_audit_tab() {
  auto* page = new QWidget(this);
  auto* column = new QVBoxLayout(page);
  column->setContentsMargins(0, 0, 0, 0);
  column->setSpacing(8);

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
  if (!send(session_.list_audit())) {
    return;
  }
  (void)send(session_.list_sessions());
}

bool AdminPanel::eventFilter(QObject* watched, QEvent* event) {
  if (event->type() != QEvent::KeyPress) {
    return QWidget::eventFilter(watched, event);
  }
  auto* key = dynamic_cast<QKeyEvent*>(event);
  if (key == nullptr) {
    return QWidget::eventFilter(watched, event);
  }

  if (watched == filter_ && key->key() == Qt::Key_Escape) {
    filter_->clear();
    users_->setFocus(Qt::OtherFocusReason);
    return true;
  }

  // Bare keys only. Ctrl+C in a table is still a copy, and a modifier is how
  // somebody types a letter that is not a command.
  if ((key->modifiers() & ~Qt::KeypadModifier) != Qt::NoModifier) {
    return QWidget::eventFilter(watched, event);
  }
  if (watched == sessions_) {
    if (key->key() == Qt::Key_K) {
      on_end_session();
      return true;
    }
    return QWidget::eventFilter(watched, event);
  }
  if (watched != users_) {
    return QWidget::eventFilter(watched, event);
  }
  switch (key->key()) {
    case Qt::Key_Slash:
      filter_->setFocus(Qt::OtherFocusReason);
      filter_->selectAll();
      return true;
    case Qt::Key_Return:
    case Qt::Key_Enter:
    case Qt::Key_R:
      pane_->focus_restrictions();
      return true;
    case Qt::Key_M:
      on_send_notice();
      return true;
    case Qt::Key_B:
      toggle_flag(selected_id(users_), Flag::Banned);
      return true;
    case Qt::Key_P:
      on_reset_password();
      return true;
    case Qt::Key_D:
      on_delete_user();
      return true;
    case Qt::Key_N:
      on_create_user();
      return true;
    default:
      return QWidget::eventFilter(watched, event);
  }
}

void AdminPanel::on_tab_changed(int index) {
  pages_->setCurrentIndex(index);
  // The panel stays open while other people are doing things, so what it shows
  // goes stale on its own. Asking again when a tab is brought forward costs
  // a few messages and is the moment somebody is about to read it.
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

void AdminPanel::act(const Result<std::monostate>& request) {
  if (send(request)) {
    (void)send(session_.list_audit());
  }
}

void AdminPanel::apply_users(const QStringList& rows) {
  // Read here and not out of the table afterwards, because the table keeps
  // only what it draws. The row is the one MainWindow's on_user_list builds:
  // id, username, display name, role, created, online, the restrictions as
  // models::describe writes them, and the same four flags once more as one
  // character each.
  accounts_.clear();
  accounts_.reserve(static_cast<int>(rows.size()));
  for (const QString& row : rows) {
    const QStringList fields = row.split(QLatin1Char('\t'));
    const QString id = fields.value(0);
    if (id.isEmpty()) {
      continue;
    }
    // Bounds checked rather than indexed blindly: a row from an older client,
    // or one that lost its tail somewhere, should leave the boxes as they
    // were and not read off the end of a string.
    const QString flags = fields.value(7);
    const auto flag = [&flags](qsizetype at) {
      return flags.size() > at && flags.at(at) == QLatin1Char('1');
    };
    accounts_.insert(id, Account{
                             .username = fields.value(1),
                             .display_name = fields.value(2),
                             .role = models::role_from_string(fields.value(3).toStdString()),
                             .online = fields.value(5) == QStringLiteral("yes"),
                             .created = fields.value(4),
                             .restrictions = models::Restrictions{.banned = flag(0),
                                                                  .muted = flag(1),
                                                                  .silenced = flag(2),
                                                                  .screen_share_blocked = flag(3)},
                         });
  }
  fill_accounts(rows);
  on_filter_changed(filter_->text());
  refresh_pane();
  // The session table names accounts and decides who is online from this
  // list, so it is drawn again with what just arrived.
  fill_sessions();
}

void AdminPanel::fill_accounts(const QStringList& rows) {
  // Restored by identity rather than by row, for the reason ui::fill does:
  // the order changes as accounts come and go, and a selection that jumps to
  // a different account between a refresh and a key is how the wrong person
  // gets deleted.
  const QString selected = selected_id(users_);
  const theme::Colors& colours = theme::colors();

  users_->setRowCount(static_cast<int>(rows.size()));
  for (int row = 0; row < rows.size(); ++row) {
    const QStringList fields = rows.at(row).split(QLatin1Char('\t'));
    const QString id = fields.value(0);
    const QString flags = fields.value(7);
    int bits = 0;
    for (const auto& [at, bit] :
         {std::pair{0, kAccountBanned}, std::pair{1, kAccountMuted}, std::pair{2, kAccountSilenced},
          std::pair{3, kAccountScreenBlocked}}) {
      if (flags.size() > at && flags.at(at) == QLatin1Char('1')) {
        bits |= bit;
      }
    }
    const bool online = fields.value(5) == QStringLiteral("yes");

    auto* status = new QTableWidgetItem();
    status->setData(kIdRole, id);
    status->setData(kAccountCellRole, static_cast<int>(AccountCell::Status));
    status->setData(kAccountOnlineRole, online);
    status->setToolTip(online ? QStringLiteral("signed in") : QStringLiteral("not signed in"));

    auto* user = new QTableWidgetItem(fields.value(1));
    auto* name = new QTableWidgetItem(fields.value(2));
    name->setForeground(QBrush(colours.muted));
    auto* role = new QTableWidgetItem(fields.value(3));
    role->setData(kAccountCellRole, static_cast<int>(AccountCell::Role));
    // The text stays the sentence models::describe wrote, so the filter finds
    // "silenced" and the tooltip can say it; the chips come from the bits.
    auto* restrictions = new QTableWidgetItem(fields.value(6));
    restrictions->setData(kAccountCellRole, static_cast<int>(AccountCell::Restrictions));
    restrictions->setData(kAccountFlagsRole, bits);
    auto* created = new QTableWidgetItem(fields.value(4));
    created->setForeground(QBrush(colours.muted));

    const std::array<std::pair<int, QTableWidgetItem*>, 6> cells{{
        {kStatusColumn, status},
        {kUserColumn, user},
        {kNameColumn, name},
        {kRoleColumn, role},
        {kRestrictionsColumn, restrictions},
        {kCreatedColumn, created},
    }};
    for (const auto& [column, item] : cells) {
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
      if (item->toolTip().isEmpty() && !item->text().isEmpty()) {
        item->setToolTip(item->text());
      }
      users_->setItem(row, column, item);
    }
    if (!selected.isEmpty() && id == selected) {
      users_->selectRow(row);
    }
  }
}

void AdminPanel::on_filter_changed(const QString& text) {
  const QString needle = text.trimmed().toLower();
  int shown = 0;
  for (int row = 0; row < users_->rowCount(); ++row) {
    bool matches = needle.isEmpty();
    if (!matches) {
      // Everything a row says, the state of the dot included, so "online"
      // and "admin" work as well as a name does.
      QString haystack;
      for (int column = kUserColumn; column <= kCreatedColumn; ++column) {
        if (const QTableWidgetItem* item = users_->item(row, column); item != nullptr) {
          haystack += item->text() + QLatin1Char(' ');
        }
      }
      const QTableWidgetItem* status = users_->item(row, kStatusColumn);
      haystack += status != nullptr && status->data(kAccountOnlineRole).toBool()
                      ? QStringLiteral("online")
                      : QStringLiteral("offline");
      matches = haystack.toLower().contains(needle);
    }
    users_->setRowHidden(row, !matches);
    shown += matches ? 1 : 0;
  }
  filter_count_->setText(QStringLiteral("%1/%2").arg(shown).arg(users_->rowCount()));
}

void AdminPanel::on_account_selected() {
  refresh_pane();
}

void AdminPanel::refresh_pane() {
  const QString user_id = selected_id(users_);
  const auto account = accounts_.constFind(user_id);
  if (user_id.isEmpty() || account == accounts_.constEnd()) {
    pane_->show_nothing();
    return;
  }
  pane_->show_account(view_of(user_id, *account));
  pane_->show_audit(audit_tail(user_id));
}

AccountView AdminPanel::view_of(const QString& user_id, const Account& account) const {
  return AccountView{
      .id = user_id,
      .username = account.username,
      .display_name = account.display_name,
      .admin = account.role == models::Role::Admin,
      .online = account.online,
      .created = account.created,
      .restrictions = account.restrictions,
      .is_me = user_id == QString::fromStdString(session_.local_user().id),
  };
}

QStringList AdminPanel::audit_tail(const QString& user_id) const {
  // The rows are the ones MainWindow's on_audit_list builds: id, when, who,
  // action, target id, room, detail. Newest first, as the server sends them.
  QStringList lines;
  for (const QString& row : audit_rows_) {
    QStringList fields = row.split(QLatin1Char('\t'));
    if (fields.value(4) != user_id) {
      continue;
    }
    // "09-07 15:07 · bruno · restrict user": the day and the minute, who, and
    // the action with its underscores taken out. The detail is a sentence
    // and the Audit tab has it.
    lines.push_back(QStringLiteral("%1 · %2 · %3")
                        .arg(fields.value(1).mid(5, 11), fields.value(2),
                             fields.value(3).replace(QLatin1Char('_'), QLatin1Char(' '))));
    if (lines.size() == kAuditTail) {
      break;
    }
  }
  return lines;
}

void AdminPanel::apply_rooms(const QStringList& rows) {
  fill(rooms_, rows);
}

void AdminPanel::apply_audit(const QStringList& rows) {
  audit_rows_ = rows;
  fill(audit_, rows);
  // The pane's tail is read from the same rows, and this is when they change.
  if (const QString user_id = pane_->account_id(); !user_id.isEmpty()) {
    pane_->show_audit(audit_tail(user_id));
  }
}

void AdminPanel::apply_sessions(const QStringList& rows) {
  // The rows are the ones MainWindow's on_session_list builds: session id,
  // account id, address, and the three times as text - connected, last seen,
  // ended - with a time the server never wrote as an empty field.
  session_rows_ = rows;
  fill_sessions();
}

void AdminPanel::fill_sessions() {
  // Restored by identity, for the reason fill_accounts does: the rows move as
  // sessions open and close, and `k` lands on whatever is selected.
  const QString selected = selected_id(sessions_);
  const theme::Colors& colours = theme::colors();
  int online = 0;

  sessions_->setRowCount(static_cast<int>(session_rows_.size()));
  for (int row = 0; row < session_rows_.size(); ++row) {
    const QStringList fields = session_rows_.at(row).split(QLatin1Char('\t'));
    const QString id = fields.value(0);
    const QString user_id = fields.value(1);
    const SessionState state = session_state(fields);
    online += state == SessionState::Online ? 1 : 0;

    auto* account = new QTableWidgetItem(session_account_label(user_id));
    account->setData(kIdRole, id);
    account->setData(kSessionUserRole, user_id);
    account->setData(kSessionStateRole, static_cast<int>(state));
    // The identifier in full, which is the one thing the column cannot show
    // and the thing that matches a row in the server's log.
    account->setToolTip(user_id);

    // The one cell somebody reads this table for, in a colour: the exception
    // is what should catch the eye, and here the exception is somebody being
    // here.
    auto* status = new QTableWidgetItem();
    switch (state) {
      case SessionState::Online:
        status->setText(QStringLiteral("online"));
        status->setForeground(QBrush(colours.success));
        break;
      case SessionState::Stale:
        status->setText(QStringLiteral("stale"));
        status->setForeground(QBrush(colours.warn));
        status->setToolTip(
            QStringLiteral("Open, but the server is not holding it: what a server killed rather "
                           "than stopped leaves behind. It is closed the next time one starts."));
        break;
      case SessionState::Ended:
        status->setText(QStringLiteral("ended"));
        status->setForeground(QBrush(colours.muted));
        break;
    }

    auto* address = new QTableWidgetItem(or_dash(fields.value(2)));
    auto* seen = new QTableWidgetItem(or_dash(fields.value(4)));
    auto* connected = new QTableWidgetItem(or_dash(fields.value(3)));
    if (state == SessionState::Ended) {
      // The rows that are history read as history.
      for (QTableWidgetItem* item : {address, seen, connected}) {
        item->setForeground(QBrush(colours.muted));
      }
    }

    const std::array<std::pair<int, QTableWidgetItem*>, 5> cells{{
        {kSessionAccountColumn, account},
        {kSessionStateColumn, status},
        {kSessionAddressColumn, address},
        {kSessionSeenColumn, seen},
        {kSessionConnectedColumn, connected},
    }};
    for (const auto& [column, item] : cells) {
      item->setFlags(item->flags() & ~Qt::ItemIsEditable);
      sessions_->setItem(row, column, item);
    }
    if (!selected.isEmpty() && id == selected) {
      sessions_->selectRow(row);
    }
  }

  sessions_count_->setText(
      QStringLiteral("%1 online · %2 sessions").arg(online).arg(session_rows_.size()));
}

AdminPanel::SessionState AdminPanel::session_state(const QStringList& fields) const {
  if (!fields.value(5).isEmpty()) {
    return SessionState::Ended;
  }
  const auto account = accounts_.constFind(fields.value(1));
  return account != accounts_.constEnd() && account->online ? SessionState::Online
                                                            : SessionState::Stale;
}

QString AdminPanel::session_account_label(const QString& user_id) const {
  const auto account = accounts_.constFind(user_id);
  if (account != accounts_.constEnd()) {
    return account->username;
  }
  // The first eight characters, which is what the pane and the server's log
  // show of an identifier; the whole of it is in the tooltip.
  return user_id.left(8) + QStringLiteral("…");
}

void AdminPanel::on_end_session() {
  const int row = sessions_->currentRow();
  const QTableWidgetItem* cell = row >= 0 ? sessions_->item(row, kSessionAccountColumn) : nullptr;
  if (cell == nullptr) {
    return;
  }
  const QString user_id = cell->data(kSessionUserRole).toString();
  const auto state = static_cast<SessionState>(cell->data(kSessionStateRole).toInt());

  // Refused here, with a sentence about the row, rather than sent to be
  // refused: the state is on screen, and the server's answer would be a code.
  // The sentences are tools/dbadmin's, for the same three refusals.
  switch (state) {
    case SessionState::Ended:
      emit failed(QStringLiteral("invalid_target"),
                  QStringLiteral("this session has already ended"));
      return;
    case SessionState::Stale:
      emit failed(QStringLiteral("invalid_target"),
                  QStringLiteral("this session is not answering, so no server is holding it; "
                                 "nobody is signed out by ending it"));
      return;
    case SessionState::Online:
      break;
  }
  if (user_id == QString::fromStdString(session_.local_user().id)) {
    emit failed(QStringLiteral("invalid_target"),
                QStringLiteral("an administrator cannot end their own session from here"));
    return;
  }

  // The same shape as a ban's question, because it is the same kind of
  // decision about a person who is mid-sentence: what will happen, when, and
  // what will not. Broken into lines by hand: the dialog's label does not
  // wrap, and one long sentence makes a box wider than the window.
  const QString name = session_account_label(user_id);
  bool accepted = false;
  const QString reason = QInputDialog::getText(
      this, QStringLiteral("End the session of %1").arg(name),
      QStringLiteral("%1 is signed out at once: out of the room, tokens revoked, and everybody "
                     "in the room told.\nNothing is taken from the account, and they may sign "
                     "in again straight away.\nTo keep them out, restrict the account on the "
                     "Users tab instead.\n\nReason, shown to them (optional)")
          .arg(name),
      QLineEdit::Normal, QString(), &accepted);
  if (!accepted) {
    return;
  }

  act(session_.end_session(user_id.toStdString(), reason.toStdString()));
  // The answer is the new session list. The dot on the account table and the
  // count under this one follow the account list, which is asked for as well.
  (void)send(session_.list_users());
}

void AdminPanel::on_restrict(const protocol::RestrictUser& change) {
  // Nothing changes locally. The answer is the whole new user list, which is
  // what refreshes the table, the accounts_ and the pane's boxes.
  act(session_.restrict_user(change));
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

  act(session_.create_user(username->text().toStdString(), password->text().toStdString(),
                           display_name->text().toStdString(),
                           role->currentIndex() == 1 ? models::Role::Admin : models::Role::User));
}

void AdminPanel::on_change_role() {
  const QString user_id = selected_id(users_);
  if (user_id.isEmpty()) {
    return;
  }

  // The role the server last reported, not the word the Role column happens to
  // be showing. Same reason the pane reads from here.
  const auto account = accounts_.constFind(user_id);
  if (account == accounts_.constEnd()) {
    return;
  }

  protocol::UpdateUser change;
  change.user_id = user_id.toStdString();
  change.role = account->role == models::Role::Admin ? models::Role::User : models::Role::Admin;
  act(session_.update_user(change));
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
  act(session_.update_user(change));
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
  act(session_.send_notice(user_id.toStdString(), text->toPlainText().toStdString()));
}

void AdminPanel::toggle_flag(const QString& user_id, Flag flag) {
  const auto account = accounts_.constFind(user_id);
  if (user_id.isEmpty() || account == accounts_.constEnd()) {
    return;
  }
  if (user_id == QString::fromStdString(session_.local_user().id)) {
    return;
  }
  const QString name = account->display_name.isEmpty() ? account->username : account->display_name;
  const models::Restrictions now = account->restrictions;

  protocol::RestrictUser change;
  change.user_id = user_id.toStdString();
  switch (flag) {
    case Flag::Banned: {
      change.banned = !now.banned;
      if (*change.banned) {
        bool accepted = false;
        const QString reason = QInputDialog::getText(
            this, QStringLiteral("Ban %1").arg(name),
            QStringLiteral("%1 is signed out now and cannot sign in again until the ban is "
                           "lifted.\nReason, shown to them (optional)")
                .arg(name),
            QLineEdit::Normal, QString(), &accepted);
        if (!accepted) {
          return;
        }
        change.reason = reason.toStdString();
      }
      break;
    }
    case Flag::Muted:
      change.muted = !now.muted;
      break;
    case Flag::Silenced:
      change.silenced = !now.silenced;
      break;
    case Flag::ScreenBlocked:
      change.screen_share_blocked = !now.screen_share_blocked;
      break;
  }
  act(session_.restrict_user(change));
}

void AdminPanel::on_user_menu(const QPoint& where) {
  // The row under the pointer, made the current one first. The slots this
  // menu reaches read the selection, and the pane follows it: an
  // administrator who right-clicks one account should be looking at that
  // account's boxes when the menu closes.
  const QModelIndex index = users_->indexAt(where);
  if (!index.isValid()) {
    return;
  }
  users_->selectRow(index.row());

  const QString user_id = selected_id(users_);
  const auto account = accounts_.constFind(user_id);
  if (user_id.isEmpty() || account == accounts_.constEnd()) {
    return;
  }
  // Nothing here applies to yourself. The server refuses every one of these
  // about the administrator sending them (invalid_target), and a menu of
  // actions that will all be refused is worse than no menu.
  if (user_id == QString::fromStdString(session_.local_user().id)) {
    return;
  }
  const QString name = account->display_name.isEmpty() ? account->username : account->display_name;
  const models::Restrictions now = account->restrictions;

  QMenu menu(this);
  QAction* notice = menu.addAction(QStringLiteral("Message %1...").arg(name));
  QAction* role = menu.addAction(account->role == models::Role::Admin
                                     ? QStringLiteral("Make %1 a user").arg(name)
                                     : QStringLiteral("Make %1 an administrator").arg(name));
  QAction* password = menu.addAction(QStringLiteral("Reset the password of %1...").arg(name));

  // The four restrictions, one flag each, worded as what they do to the person
  // rather than as the flag's name. The ban is the one that asks a question
  // first: it is the one that locks somebody out, and a reason is worth more
  // to somebody who cannot sign in to ask for one.
  menu.addSeparator();
  QAction* ban = menu.addAction(now.banned ? QStringLiteral("Lift the ban on %1").arg(name)
                                           : QStringLiteral("Ban %1...").arg(name));
  QAction* microphone =
      menu.addAction(now.muted ? QStringLiteral("Let %1 use the microphone again").arg(name)
                               : QStringLiteral("Stop %1 from using the microphone").arg(name));
  QAction* chat =
      menu.addAction(now.silenced ? QStringLiteral("Let %1 use the chat again").arg(name)
                                  : QStringLiteral("Silence %1 in the chat").arg(name));
  QAction* screen = menu.addAction(
      now.screen_share_blocked ? QStringLiteral("Let %1 share their screen again").arg(name)
                               : QStringLiteral("Stop %1 from sharing their screen").arg(name));
  // All four together, with a reason: the boxes on the pane.
  QAction* all = menu.addAction(QStringLiteral("Edit the restrictions"));

  menu.addSeparator();
  QAction* remove = menu.addAction(QStringLiteral("Delete %1...").arg(name));

  const QAction* chosen = menu.exec(users_->viewport()->mapToGlobal(where));
  if (chosen == nullptr) {
    return;
  }

  if (chosen == notice) {
    on_send_notice();
  } else if (chosen == role) {
    on_change_role();
  } else if (chosen == password) {
    on_reset_password();
  } else if (chosen == all) {
    pane_->focus_restrictions();
  } else if (chosen == remove) {
    on_delete_user();
  } else if (chosen == ban) {
    toggle_flag(user_id, Flag::Banned);
  } else if (chosen == microphone) {
    toggle_flag(user_id, Flag::Muted);
  } else if (chosen == chat) {
    toggle_flag(user_id, Flag::Silenced);
  } else if (chosen == screen) {
    toggle_flag(user_id, Flag::ScreenBlocked);
  }
}

void AdminPanel::on_delete_user() {
  const QString user_id = selected_id(users_);
  if (user_id.isEmpty()) {
    return;
  }

  // From the same place the other actions take it, so that the name in the
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
  act(session_.delete_user(user_id.toStdString()));
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
