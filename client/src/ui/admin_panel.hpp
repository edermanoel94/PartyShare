#pragma once

#include <QHash>
#include <QString>
#include <QStringList>
#include <QWidget>

#include "app/call_session.hpp"

class QLabel;
class QPushButton;
class QTableWidget;
class QTabWidget;

namespace dv::ui {

/// Accounts, rooms and the audit log, for an administrator.
///
/// A page of its own rather than a dialog, because managing a dozen accounts
/// is not something anyone does in one glance and a modal window would trap
/// them there while a call is going on.
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

 public slots:
  // Called on the UI thread, from the session's callbacks, which arrive on the
  // signaling thread. Rows travel as tab separated strings for the same reason
  // the participant list does: it needs no registered metatype.
  void apply_users(const QStringList& rows);
  void apply_rooms(const QStringList& rows);
  void apply_audit(const QStringList& rows);

 signals:
  /// A request could not even be sent. Reported upwards rather than shown
  /// here, so that every error in the application appears in one place.
  void failed(const QString& code, const QString& message);
  /// The administrator asked to go back to the rest of the application.
  void closed();

 private slots:
  void on_create_user();
  void on_change_role();
  void on_reset_password();
  void on_restrict_user();
  void on_delete_user();
  void on_create_room();
  void on_close_room();
  void on_tab_changed(int index);

  // Not redundant: the section above is `private slots:`, which Qt's moc
  // needs as its own specifier, and these members are not slots.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  QWidget* build_users_tab();
  QWidget* build_rooms_tab();
  QWidget* build_audit_tab();

  /// What the server last said about one account, as it said it.
  ///
  /// The role and the restrictions are here because the two dialogs that
  /// change them need to know what they are now, and the only other place to
  /// find that is the table - where they exist as the sentence
  /// models::describe wrote and the word models::to_string wrote, in cells
  /// whose column number depends on nothing having shifted. Reading a
  /// rendering back as though it were protocol is a decision that fails
  /// quietly: the boxes open wrong and the dialog sends all four.
  ///
  /// The username rides along so that the dialogs can title themselves without
  /// reaching into a cell either.
  struct Account {
    QString username;
    models::Role role = models::Role::User;
    models::Restrictions restrictions;
  };

  /// Sends `request`, turning a local failure into the `failed` signal.
  /// Answers whether it went out, so a caller making several requests can stop
  /// at the first failure instead of reporting the same dead socket again.
  bool send(const Result<std::monostate>& request);

  client::app::CallSession& session_;

  /// The accounts of the last `apply_users`, by user id. Cleared and rebuilt
  /// with each one, so an account that has gone leaves with it.
  QHash<QString, Account> accounts_;

  QTabWidget* tabs_ = nullptr;

  QTableWidget* users_ = nullptr;
  QPushButton* create_user_ = nullptr;
  QPushButton* change_role_ = nullptr;
  QPushButton* reset_password_ = nullptr;
  QPushButton* restrict_user_ = nullptr;
  QPushButton* delete_user_ = nullptr;

  QTableWidget* rooms_ = nullptr;
  QPushButton* create_room_ = nullptr;
  QPushButton* close_room_ = nullptr;

  QTableWidget* audit_ = nullptr;
};

}  // namespace dv::ui
