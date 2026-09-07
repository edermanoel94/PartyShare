#pragma once

#include <cstdint>

#include <QHash>
#include <QPoint>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "app/call_session.hpp"
#include "ui/account_pane.hpp"

class QEvent;
class QLabel;
class QLineEdit;
class QPushButton;
class QStackedWidget;
class QTabBar;
class QTableWidget;

namespace dv::ui {

/// Accounts, rooms, sessions and the audit log, for an administrator.
///
/// A page of its own rather than a dialog, because managing a dozen accounts
/// is not something anyone does in one glance and a modal window would trap
/// them there while a call is going on.
///
/// One line at the top holds the title, the tabs and the way out, and the
/// pages sit directly under it with no frame of their own: the tables and the
/// pane are the cards, and a card around cards was a second set of corners
/// that agreed with nothing. The interface's own face throughout, the same
/// one every other screen is set in.
///
/// The accounts tab is laid out as a console: a filter line over a table,
/// rows that say their state with a dot, a coloured role and chips rather
/// than with words, and beside the table a pane showing the account that is
/// picked - its four restrictions as boxes, the actions that still need a
/// dialog, and the last audit lines about it. The keys work from the table
/// and are written under it. What this replaced was a grid of six columns,
/// six buttons and a dialog per action, none of which could be read at a
/// glance or driven without the mouse.
///
/// The sessions tab is the screen tools/dbadmin has, reached through the
/// server instead of through the database: who has been connected and from
/// where, the open sessions first, and `k` to sign one of them out.
///
/// Everything here is a request to the server and an answer that arrives
/// later, never a local edit: this widget holds no state that the server does
/// not, so there is no way for the table to disagree with the truth. Each
/// change is answered with the whole new list, which is what refreshes it.
///
/// `accounts_` is not an exception to that. It holds the server's own answer
/// and nothing else, replaced whole every time one arrives; what it is for is
/// keeping that answer in the form it came in, rather than leaving the panel
/// to read it back out of the text it drew.
///
/// The panel is only ever shown to an administrator, but that is presentation.
/// The server refuses every one of these messages from anybody else, and this
/// widget would show empty tables and errors rather than data if it were
/// somehow opened.
// A QObject cannot be copied or moved: its identity is the thing Qt tracks.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class AdminPanel : public QWidget {
  Q_OBJECT

 public:
  explicit AdminPanel(client::app::CallSession& session, QWidget* parent = nullptr);

  /// Asks the server for everything this panel shows. Called when the panel is
  /// opened, because a panel that shows what was true the last time it was
  /// looked at is worse than one that shows nothing.
  void refresh();

  /// How an account should read on screen, from the server's last answer.
  ///
  /// Here rather than in the window because this is where that answer is kept.
  /// An identifier nothing in it matches comes back as itself: an account
  /// deleted a moment ago, or a panel that has never been opened, is better
  /// named by a code than by a blank.
  ///
  /// Only safe to call on the interface thread, like every other method of a
  /// QWidget.
  [[nodiscard]] QString label_for(const QString& user_id) const;

 public slots:
  // Called on the UI thread, from the session's callbacks, which arrive on the
  // signaling thread. Rows travel as tab separated strings for the same reason
  // the participant list does: it needs no registered metatype.
  void apply_users(const QStringList& rows);
  void apply_rooms(const QStringList& rows);
  void apply_audit(const QStringList& rows);
  void apply_sessions(const QStringList& rows);

 signals:
  /// A request could not even be sent. Reported upwards rather than shown
  /// here, so that every error in the application appears in one place.
  void failed(const QString& code, const QString& message);
  /// The administrator asked to go back to the rest of the application.
  void closed();

 private slots:
  void on_create_user();
  void on_send_notice();
  void on_change_role();
  void on_reset_password();
  void on_delete_user();
  void on_create_room();
  void on_close_room();
  /// Signs out whoever the session table has selected, after asking for a
  /// reason. Only a session that is online can be ended; the other two
  /// states are refused on the spot with a sentence about that row, because
  /// the request is about a connection a running server is holding and
  /// neither of them is one.
  void on_end_session();
  void on_tab_changed(int index);
  /// The right-click menu on an account: everything the pane does, and each
  /// restriction on its own, for the row under the pointer.
  void on_user_menu(const QPoint& where);
  /// Another row is current: the pane follows.
  void on_account_selected();
  /// The filter line changed: rows that do not match are hidden, not removed,
  /// so the selection and the identifiers stay where they are.
  void on_filter_changed(const QString& text);
  /// Apply was pressed on the pane.
  void on_restrict(const protocol::RestrictUser& change);

 protected:
  /// The keys of the console, watched on the two widgets they belong to.
  ///
  /// In the account table, a bare letter is a command: `/` goes to the
  /// filter, Return and `r` to the restriction boxes, `m` messages, `b` bans,
  /// `p` resets the password, `d` deletes, `n` creates. In the session table,
  /// `k` ends the selected session. In the filter, Escape clears it and hands
  /// the keyboard back to the table, which is the way out of a search
  /// everywhere else. Watched rather than bound as QShortcuts: a shortcut
  /// only fires while the window is the active one, and these are meant to
  /// work wherever the key press itself is delivered.
  bool eventFilter(QObject* watched, QEvent* event) override;

  // Not redundant: the section above is `protected:`, for an override, and
  // these are the widget's own members.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  QWidget* build_users_tab();
  QWidget* build_rooms_tab();
  QWidget* build_sessions_tab();
  QWidget* build_audit_tab();

  /// What one row of the session table says about the connection it names.
  ///
  /// Three answers and not two, because "open" and "online" are different
  /// claims and the difference is the thing an administrator has to be able
  /// to see. A row the server has not closed whose account is not connected
  /// is a row the server failed to close, or one another server left behind;
  /// calling it online would be the panel repeating a lie the store is
  /// telling it. Decided against the account list rather than sent by the
  /// server, because that list already says who is connected.
  enum class SessionState : std::uint8_t { Online, Stale, Ended };

  /// What the server last said about one account, as it said it.
  ///
  /// The role and the restrictions are here because the pane and the menu
  /// need to know what they are now, and the only other place to find that
  /// is the table - where they exist as chips a delegate drew and a word in
  /// small capitals, in cells whose column number depends on nothing having
  /// shifted. Reading a rendering back as though it were protocol is a
  /// decision that fails quietly: the boxes open wrong and Apply sends all
  /// four.
  ///
  /// The username rides along so that the dialogs can title themselves without
  /// reaching into a cell either.
  struct Account {
    QString username;
    /// What they call themselves, kept beside the username so that anything
    /// naming this account on screen reads the way a log line does. See
    /// models::user_label, which is what turns the two into one string.
    QString display_name;
    models::Role role = models::Role::User;
    bool online = false;
    QString created;
    models::Restrictions restrictions;
  };

  /// One of the four, for the menu entries and the `b` key that turn a single
  /// flag on or off.
  enum class Flag : std::uint8_t { Banned, Muted, Silenced, ScreenBlocked };

  /// Rebuilds the account table from the rows of `apply_users`, keeping the
  /// selection by identifier. The table's own fill rather than ui::fill,
  /// because its cells carry what the delegate draws.
  void fill_accounts(const QStringList& rows);

  /// Rebuilds the session table from `session_rows_`. Its own fill because
  /// the account column is a name looked up in `accounts_` and the state is
  /// decided rather than read, so it runs again when either list arrives.
  void fill_sessions();

  /// The state of the session row `fields` describes, against `accounts_`.
  [[nodiscard]] SessionState session_state(const QStringList& fields) const;

  /// How an account reads in the session table: its username, or the first
  /// characters of the identifier for an account that has since been deleted.
  /// A row that names nobody is still a row that says somebody was here.
  [[nodiscard]] QString session_account_label(const QString& user_id) const;

  /// Shows the current row in the pane, or nothing when there is none.
  void refresh_pane();

  /// The pane's view of one account.
  [[nodiscard]] AccountView view_of(const QString& user_id, const Account& account) const;

  /// The last few audit lines whose target is `user_id`, newest first, one
  /// line each.
  [[nodiscard]] QStringList audit_tail(const QString& user_id) const;

  /// Turns one restriction the other way for `user_id`, the other three left
  /// absent so that this cannot undo what somebody else applied. See
  /// protocol::RestrictUser for why absent and false are different requests.
  /// Applying a ban asks for a reason first, because it is the one that locks
  /// somebody out.
  void toggle_flag(const QString& user_id, Flag flag);

  /// Sends `request`, turning a local failure into the `failed` signal.
  /// Answers whether it went out, so a caller making several requests can stop
  /// at the first failure instead of reporting the same dead socket again.
  bool send(const Result<std::monostate>& request);

  /// Sends an action about an account, and then asks for the audit log, so
  /// that the pane's tail shows the line the action just wrote. The two go
  /// down the same socket in order, so the answer to the second is the log
  /// after the first.
  void act(const Result<std::monostate>& request);

  client::app::CallSession& session_;

  /// The accounts of the last `apply_users`, by user id. Cleared and rebuilt
  /// with each one, so an account that has gone leaves with it.
  QHash<QString, Account> accounts_;
  /// The rows of the last `apply_audit`, kept for the pane's tail.
  QStringList audit_rows_;
  /// The rows of the last `apply_sessions`, kept so the table can be drawn
  /// again with names when the account list arrives after them.
  QStringList session_rows_;

  QTabBar* tabs_ = nullptr;
  QStackedWidget* pages_ = nullptr;

  QLineEdit* filter_ = nullptr;
  QLabel* filter_count_ = nullptr;
  QTableWidget* users_ = nullptr;
  AccountPane* pane_ = nullptr;
  QPushButton* create_user_ = nullptr;

  QTableWidget* rooms_ = nullptr;
  QPushButton* create_room_ = nullptr;
  QPushButton* close_room_ = nullptr;

  QTableWidget* sessions_ = nullptr;
  /// "2 online · 14 sessions", under the table.
  QLabel* sessions_count_ = nullptr;
  QPushButton* end_session_ = nullptr;

  QTableWidget* audit_ = nullptr;
};

}  // namespace dv::ui
