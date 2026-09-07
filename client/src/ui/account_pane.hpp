#pragma once

#include <dv/models/user.hpp>
#include <dv/protocol/message.hpp>

#include <QFrame>
#include <QString>
#include <QStringList>

class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QWidget;

namespace dv::ui {

/// One account as the pane shows it: the server's last word about it, in the
/// form AdminPanel keeps it, plus whether it is the administrator's own.
struct AccountView {
  QString id;
  QString username;
  QString display_name;
  bool admin = false;
  bool online = false;
  /// The day it was created, as the table shows it, or empty.
  QString created;
  models::Restrictions restrictions;
  /// The server refuses every change an administrator asks about themselves,
  /// so the pane shows the account and offers nothing.
  bool is_me = false;
};

/// The account the administrator has picked, beside the table: who they are,
/// the four restrictions as boxes, the actions that still need a dialog, and
/// the last things the audit log says about them.
///
/// What it replaces is the restrictions dialog and the row of buttons under
/// the table. A dialog is a thing to open, read and dismiss; a pane is read
/// while the table is, and the boxes on it show the account's state before
/// anybody decides to change it. It holds no state the table does not: what
/// it shows is what `show_account` was last given, and the boxes send what
/// they show.
///
/// Every change is a request, emitted upwards. The pane does not talk to the
/// session, so that AdminPanel stays the one place that sends and the one
/// place that reports a failure.
// A QObject cannot be copied or moved: its identity is the thing Qt tracks.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class AccountPane : public QFrame {
  Q_OBJECT

 public:
  explicit AccountPane(QWidget* parent = nullptr);

  /// Shows `account`, replacing whatever was there. The boxes take the
  /// account's restrictions and the reason field is emptied: a reason belongs
  /// to one decision about one account.
  void show_account(const AccountView& account);

  /// The last few audit lines about the account on show, newest first,
  /// already reduced to one line each. Separate from `show_account` because
  /// the two lists arrive from the server as two answers.
  void show_audit(const QStringList& lines);

  /// Nothing picked: says so, and offers nothing.
  void show_nothing();

  /// The identifier of the account on show, or empty.
  [[nodiscard]] QString account_id() const { return id_; }

  /// Puts the keyboard on the first restriction box, which is where `r` in
  /// the table and "Restrictions" in the menu land.
  void focus_restrictions();

 signals:
  /// Apply was pressed: all four boxes and the reason, for the account on
  /// show. All four, because this is the one place they are decided together
  /// and the administrator has just looked at each of them.
  void restrict_requested(const protocol::RestrictUser& change);
  void message_requested();
  void role_change_requested();
  void password_reset_requested();
  void delete_requested();

 private slots:
  void on_apply();

  // Not redundant: the section above is `private slots:`, which Qt's moc
  // needs as its own specifier, and these members are not slots.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  QString id_;

  QLabel* empty_ = nullptr;
  QWidget* body_ = nullptr;

  QLabel* name_ = nullptr;
  QLabel* handle_ = nullptr;
  QLabel* role_ = nullptr;
  QLabel* created_ = nullptr;
  QLabel* status_ = nullptr;

  QWidget* controls_ = nullptr;
  QCheckBox* banned_ = nullptr;
  QCheckBox* muted_ = nullptr;
  QCheckBox* silenced_ = nullptr;
  QCheckBox* blocked_ = nullptr;
  QLineEdit* reason_ = nullptr;
  QPushButton* apply_ = nullptr;
  QPushButton* message_ = nullptr;
  QPushButton* role_change_ = nullptr;
  QPushButton* password_ = nullptr;
  QPushButton* remove_ = nullptr;
  /// Shown in place of the controls on the administrator's own row.
  QLabel* self_note_ = nullptr;

  QLabel* audit_ = nullptr;
};

}  // namespace dv::ui
