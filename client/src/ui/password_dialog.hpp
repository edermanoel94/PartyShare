#pragma once

#include <QDialog>
#include <QString>

class QDialogButtonBox;
class QLabel;
class QLineEdit;

namespace dv::ui {

/// The form behind "Change password": the password the account has now, the
/// one it should have, and that one again.
///
/// A form and nothing else. It does not hold the session and never sends
/// anything - it is opened, filled in, and read by MainWindow, which is what
/// talks to the server. The reason is that succeeding at this ends the session,
/// and a dialog that signs the application out from inside itself is a dialog
/// that has to know about pages, states and the login screen. Collecting three
/// strings is the whole job.
///
/// What it does check is the shape of the answer: three fields filled in, the
/// two new ones matching, and the new one different from the current one. Those
/// are checks the server makes too, and it is the server's answer that counts.
/// They are repeated here because the server's answer costs a round trip and
/// arrives after the dialog has closed, and "you typed the confirmation wrong"
/// is worth saying while the person can still see what they typed.
class PasswordDialog : public QDialog {
  Q_OBJECT

 public:
  explicit PasswordDialog(QWidget* parent = nullptr);

  [[nodiscard]] QString current_password() const;
  [[nodiscard]] QString new_password() const;

  // Not redundant: the section above is public and these are not.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  /// Enables the confirming button, or disables it and says what is missing.
  ///
  /// A hint under a disabled button rather than a warning after a rejected
  /// click. The button being grey is the part somebody notices; the sentence is
  /// what tells them which of the three fields is the reason.
  void revalidate();

  QLineEdit* current_ = nullptr;
  QLineEdit* new_ = nullptr;
  QLineEdit* confirm_ = nullptr;
  QLabel* hint_ = nullptr;
  QDialogButtonBox* buttons_ = nullptr;
};

}  // namespace dv::ui
