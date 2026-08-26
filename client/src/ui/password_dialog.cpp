#include "ui/password_dialog.hpp"

#include <QDialogButtonBox>
#include <QFormLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>

namespace dv::ui {

PasswordDialog::PasswordDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("Change password"));
  setModal(true);

  auto* outer = new QVBoxLayout(this);
  auto* form = new QFormLayout();

  current_ = new QLineEdit(this);
  current_->setEchoMode(QLineEdit::Password);
  new_ = new QLineEdit(this);
  new_->setEchoMode(QLineEdit::Password);
  confirm_ = new QLineEdit(this);
  confirm_->setEchoMode(QLineEdit::Password);

  form->addRow(QStringLiteral("Current password"), current_);
  form->addRow(QStringLiteral("New password"), new_);
  form->addRow(QStringLiteral("Repeat new password"), confirm_);

  // Said before the change rather than after it, because it is the part that
  // surprises people: the other machines they are signed in on stop working,
  // and so does this one. A sentence here is a decision; the same sentence on
  // the login screen afterwards is an explanation of something already done.
  auto* warning = new QLabel(
      QStringLiteral("Changing the password signs you out everywhere, including here."), this);
  warning->setWordWrap(true);
  warning->setProperty("hint", true);

  hint_ = new QLabel(QString{}, this);
  hint_->setWordWrap(true);
  hint_->setProperty("hint", true);

  buttons_ = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
  buttons_->button(QDialogButtonBox::Ok)->setText(QStringLiteral("Change password"));

  outer->addLayout(form);
  outer->addWidget(warning);
  outer->addWidget(hint_);
  outer->addWidget(buttons_);

  connect(current_, &QLineEdit::textChanged, this, [this] { revalidate(); });
  connect(new_, &QLineEdit::textChanged, this, [this] { revalidate(); });
  connect(confirm_, &QLineEdit::textChanged, this, [this] { revalidate(); });
  connect(buttons_, &QDialogButtonBox::accepted, this, &QDialog::accept);
  connect(buttons_, &QDialogButtonBox::rejected, this, &QDialog::reject);

  revalidate();
}

QString PasswordDialog::current_password() const {
  return current_->text();
}

QString PasswordDialog::new_password() const {
  return new_->text();
}

void PasswordDialog::revalidate() {
  // Not trimmed, and not validated for length or shape either. A password is
  // whatever somebody typed, spaces included, and a client that quietly trims
  // one is a client that cannot sign back in with the password it just set.
  const QString current = current_->text();
  const QString fresh = new_->text();
  const QString again = confirm_->text();

  QString problem;
  if (current.isEmpty() || fresh.isEmpty() || again.isEmpty()) {
    // Nothing said while the form is simply unfinished. Telling somebody their
    // confirmation does not match while they are still typing it is noise.
    problem.clear();
  } else if (fresh != again) {
    problem = QStringLiteral("The two new passwords do not match.");
  } else if (fresh == current) {
    problem = QStringLiteral("The new password is the same as the current one.");
  }

  const bool complete =
      !current.isEmpty() && !fresh.isEmpty() && again == fresh && fresh != current;
  buttons_->button(QDialogButtonBox::Ok)->setEnabled(complete);

  hint_->setText(problem);
  // Shown but empty, the label still takes its height and the dialog jumps by
  // a line the first time there is something to say.
  hint_->setVisible(!problem.isEmpty());
}

}  // namespace dv::ui
