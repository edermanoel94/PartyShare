#include "ui/update_dialog.hpp"

#include <QLabel>
#include <QPushButton>
#include <QShowEvent>
#include <QTimer>
#include <QVBoxLayout>

namespace dv::ui {
namespace {

/// Wide enough for the description to settle in three lines, which is as much
/// as anybody reads before answering.
constexpr int kWidth = 440;

}  // namespace

UpdateDialog::UpdateDialog(const QString& version, const QString& running, bool direct,
                           QWidget* parent)
    : AppDialog(QStringLiteral("PartyShare %1 is available").arg(version), parent) {
  setWindowTitle(QStringLiteral("Update available"));
  setMinimumWidth(kWidth);

  // The reason, and not only the news. The protocol has no version
  // negotiation: an older client does nothing with a message it does not know,
  // and nobody in the room is told. That is the one consequence worth a
  // sentence here.
  set_description(QStringLiteral("You are running %1. People in the same room on different "
                                 "versions quietly miss what the newer one sends, so it is worth "
                                 "updating before your next call.")
                      .arg(running));

  auto* what_happens = new QLabel(
      direct ? QStringLiteral("Update now downloads the installer in your browser. Close "
                              "PartyShare before you run it.")
             : QStringLiteral("This release has no installer for this system yet. The download "
                              "page lists what was published."),
      this);
  what_happens->setWordWrap(true);
  body()->addWidget(what_happens);

  update_ =
      add_primary(direct ? QStringLiteral("Update now") : QStringLiteral("Open the download page"));
  later_ = add_secondary(QStringLiteral("Later"));

  update_->setEnabled(false);
  later_->setEnabled(false);
}

void UpdateDialog::showEvent(QShowEvent* event) {
  AppDialog::showEvent(event);
  // From the moment it is on screen and not from construction: a dialog built
  // and then held back would otherwise arrive already answering.
  QTimer::singleShot(kArmDelayMs, this, &UpdateDialog::arm);
}

void UpdateDialog::arm() {
  update_->setEnabled(true);
  later_->setEnabled(true);
  // A disabled button cannot hold the focus, so nothing in the dialog had it
  // until now.
  update_->setFocus();
}

}  // namespace dv::ui
