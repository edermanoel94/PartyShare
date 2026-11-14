#include "ui/password_dialog.hpp"

#include <QLabel>
#include <QLineEdit>
#include <QPushButton>

// Written by uic from password_dialog.ui, which is the form itself: the three
// fields, the sentence over them and the two buttons under them were laid out
// in Qt Designer and are not repeated here. See docs/17-design-system.md.
#include "ui_password_dialog.h"

namespace dv::ui {

PasswordDialog::PasswordDialog(QWidget* parent) : QDialog(parent) {
  // The form's own object is only needed for as long as it takes to build the
  // widgets, which belong to this dialog from then on. The few this class
  // talks to afterwards are kept by name; the rest are the form's business.
  Ui::PasswordDialog form;
  form.setupUi(this);
  current_ = form.current;
  new_ = form.fresh;
  confirm_ = form.again;
  hint_ = form.problem;
  confirm_button_ = form.confirm;

  // The sentence over the fields is said before the change rather than after
  // it, because it is the part that surprises people: the other machines they
  // are signed in on stop working, and so does this one. A sentence here is a
  // decision; the same sentence on the login screen afterwards is an
  // explanation of something already done. It is in the form, with the rest
  // of the wording.

  connect(current_, &QLineEdit::textChanged, this, [this] { revalidate(); });
  connect(new_, &QLineEdit::textChanged, this, [this] { revalidate(); });
  connect(confirm_, &QLineEdit::textChanged, this, [this] { revalidate(); });

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
  confirm_button_->setEnabled(complete);

  hint_->setText(problem);
  // Shown but empty, the label still takes its height and the dialog jumps by
  // a line the first time there is something to say.
  hint_->setVisible(!problem.isEmpty());
}

}  // namespace dv::ui
