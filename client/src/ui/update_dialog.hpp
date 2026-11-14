#pragma once

#include <QString>

#include "ui/components/app_dialog.hpp"

class QPushButton;
class QShowEvent;

namespace dv::ui {

/// The question asked when a newer release has been published: update now, or
/// later.
///
/// It asks and does nothing else. Accepting it is the answer "now", and what
/// that means - which address is opened, and with what - belongs to whoever
/// raised the dialog; nothing here knows a URL. Rejecting it, by the other
/// button or by Esc, is "later", and the link in the status bar stays for when
/// later comes.
///
/// The buttons do not answer for the first moment the dialog is up. The news
/// arrives seconds after the program starts, which is when somebody is typing a
/// password, and a dialog that takes the keyboard mid-word would take the Enter
/// that ends it as its own answer. Esc is not held back: nobody types it into a
/// field by accident, and a dialog that cannot be sent away is worse than one
/// that can be sent away early.
class UpdateDialog : public AppDialog {
  Q_OBJECT

 public:
  /// How long the buttons wait before they answer.
  static constexpr int kArmDelayMs = 700;

  /// `direct` says whether pressing the button starts the download of an
  /// installer, or only opens the page a person downloads from - a release for
  /// a platform nothing is packaged for, or one whose files are not attached
  /// yet. The button says which, rather than promising an update and opening
  /// a list.
  UpdateDialog(const QString& version, const QString& running, bool direct,
               QWidget* parent = nullptr);

  [[nodiscard]] QPushButton* update_button() const { return update_; }
  [[nodiscard]] QPushButton* later_button() const { return later_; }

 protected:
  void showEvent(QShowEvent* event) override;

 private:
  void arm();

  QPushButton* update_ = nullptr;
  QPushButton* later_ = nullptr;
};

}  // namespace dv::ui
