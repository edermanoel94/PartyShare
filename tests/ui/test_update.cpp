// A new release: which file out of GitHub's answer is offered as the update,
// and the dialog that asks about it. Neither needs the network - the first is
// a function of a document, the second a widget that is told what to say.

#include <functional>

#include <gtest/gtest.h>

#include <QDialog>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QLabel>
#include <QList>
#include <QPushButton>
#include <QString>
#include <QUrl>
#include <QtGlobal>

#include "ui/release_info.hpp"
#include "ui/update_dialog.hpp"
#include "ui_test.hpp"

using dv::ui::find_asset;
using dv::ui::installer_name;
using dv::ui::is_safe_release_link;
using dv::ui::UpdateDialog;
using dv::ui::testing::click;
using dv::ui::testing::press_key;
using dv::ui::testing::settle;

namespace {

[[nodiscard]] QJsonObject asset(const QString& name, const QString& address) {
  return QJsonObject{{QStringLiteral("name"), name},
                     {QStringLiteral("browser_download_url"), address}};
}

[[nodiscard]] QString download(const QString& name) {
  return QStringLiteral("https://github.com/edermanoel94/PartyShare/releases/download/v0.1.66/") +
         name;
}

/// The five files v0.1.66 was published with, as GitHub lists them.
[[nodiscard]] QJsonObject release() {
  QJsonArray assets;
  for (const QString& name : {QStringLiteral("partyshare-0.1.66-macos-arm64.dmg"),
                              QStringLiteral("partyshare-0.1.66-windows-x64.msi"),
                              QStringLiteral("partyshare-0.1.66-windows-x64.zip"),
                              QStringLiteral("partyshare-server-0.1.66-linux-x64.tar.gz"),
                              QStringLiteral("SHA256SUMS")}) {
    assets.append(asset(name, download(name)));
  }
  return QJsonObject{{QStringLiteral("tag_name"), QStringLiteral("v0.1.66")},
                     {QStringLiteral("assets"), assets}};
}

[[nodiscard]] QJsonObject release_with(const QJsonObject& only) {
  return QJsonObject{{QStringLiteral("assets"), QJsonArray{only}}};
}

/// Turns the event loop until `done` says so. The one place these tests wait
/// for a clock - the dialog holds its buttons back on purpose - and it waits
/// for the thing to happen and not for a length of time, with a deadline far
/// beyond it for a machine that is busy.
[[nodiscard]] bool wait_until(const std::function<bool()>& done) {
  constexpr qint64 kDeadlineMs = 10000;
  QElapsedTimer clock;
  clock.start();
  while (!done()) {
    if (clock.elapsed() > kDeadlineMs) {
      return false;
    }
    QCoreApplication::processEvents(QEventLoop::WaitForMoreEvents, 20);
  }
  return true;
}

[[nodiscard]] QString text_of(const QDialog& dialog) {
  QString all;
  for (const QLabel* label : dialog.findChildren<QLabel*>()) {
    all += label->text() + QLatin1Char('\n');
  }
  return all;
}

}  // namespace

TEST(ReleaseInfo, FindsTheFileByItsWholeName) {
  const QJsonObject document = release();
  EXPECT_EQ(find_asset(document, QStringLiteral("partyshare-0.1.66-windows-x64.msi")),
            QUrl(download(QStringLiteral("partyshare-0.1.66-windows-x64.msi"))));
  EXPECT_EQ(find_asset(document, QStringLiteral("partyshare-0.1.66-macos-arm64.dmg")),
            QUrl(download(QStringLiteral("partyshare-0.1.66-macos-arm64.dmg"))));
}

TEST(ReleaseInfo, AFileThatOnlyResemblesTheInstallerIsNotOffered) {
  const QJsonObject document = release();
  // Another version's installer, and no name at all, which is what a platform
  // nothing is packaged for asks with.
  EXPECT_FALSE(find_asset(document, QStringLiteral("partyshare-0.1.67-windows-x64.msi")).isValid());
  EXPECT_FALSE(find_asset(document, QStringLiteral("windows-x64.msi")).isValid());
  EXPECT_FALSE(find_asset(document, QString()).isValid());
}

TEST(ReleaseInfo, AReleaseWithoutItsFilesYetHasNoInstaller) {
  const QString name = QStringLiteral("partyshare-0.1.66-windows-x64.msi");
  // Published and not yet uploaded to, and the shapes an answer that is not
  // GitHub's could have: none of them may be anything but "no".
  EXPECT_FALSE(find_asset(QJsonObject{}, name).isValid());
  EXPECT_FALSE(find_asset(QJsonObject{{QStringLiteral("assets"), QJsonArray{}}}, name).isValid());
  EXPECT_FALSE(
      find_asset(QJsonObject{{QStringLiteral("assets"), QStringLiteral("soon")}}, name).isValid());
  EXPECT_FALSE(
      find_asset(QJsonObject{{QStringLiteral("assets"), QJsonArray{1, name}}}, name).isValid());
}

TEST(ReleaseInfo, AnAddressThatIsNotHttpsOnGithubIsRefused) {
  const QString name = QStringLiteral("partyshare-0.1.66-windows-x64.msi");
  for (const QString& address :
       {QStringLiteral("http://github.com/edermanoel94/PartyShare/releases/download/v0.1.66/") +
            name,
        QStringLiteral("https://github.com.example.org/") + name,
        QStringLiteral("https://example.org/github.com/") + name,
        QStringLiteral("file:///C:/Windows/System32/calc.exe"),
        QStringLiteral("javascript:alert(1)"), QString()}) {
    EXPECT_FALSE(find_asset(release_with(asset(name, address)), name).isValid())
        << address.toStdString();
  }
}

TEST(ReleaseInfo, OnlyHttpsOnGithubIsALinkABrowserIsHandedTo) {
  EXPECT_TRUE(is_safe_release_link(
      QUrl(QStringLiteral("https://github.com/edermanoel94/PartyShare/releases/tag/v0.1.66"))));
  EXPECT_TRUE(is_safe_release_link(QUrl(QStringLiteral("https://GitHub.com/a/b"))));
  EXPECT_TRUE(is_safe_release_link(QUrl(QStringLiteral("https://objects.github.com/a"))));

  EXPECT_FALSE(is_safe_release_link(QUrl(QStringLiteral("http://github.com/a/b"))));
  EXPECT_FALSE(is_safe_release_link(QUrl(QStringLiteral("https://notgithub.com/a/b"))));
  EXPECT_FALSE(is_safe_release_link(QUrl(QStringLiteral("https://github.com.evil.example/a"))));
  EXPECT_FALSE(is_safe_release_link(QUrl()));
}

TEST(ReleaseInfo, TheInstallerIsNamedTheWayThePackagingNamesIt) {
  const QString name = installer_name(QStringLiteral("0.1.66"));
#if defined(Q_OS_WIN) && defined(Q_PROCESSOR_X86_64)
  EXPECT_EQ(name, QStringLiteral("partyshare-0.1.66-windows-x64.msi"));
#elif defined(Q_OS_MACOS) && defined(Q_PROCESSOR_ARM_64)
  EXPECT_EQ(name, QStringLiteral("partyshare-0.1.66-macos-arm64.dmg"));
#elif defined(Q_OS_LINUX) && defined(Q_PROCESSOR_X86_64)
  EXPECT_EQ(name, QStringLiteral("partyshare-0.1.66-linux-x64.AppImage"));
#else
  EXPECT_TRUE(name.isEmpty());
#endif
  // And never a name that is merely the version: whatever it is, it is either
  // nothing or a file of this program's.
  EXPECT_TRUE(name.isEmpty() || name.startsWith(QStringLiteral("partyshare-0.1.66-")));
}

TEST(UpdateDialog, SaysWhichVersionIsNewAndWhichIsRunning) {
  const UpdateDialog dialog(QStringLiteral("0.1.67"), QStringLiteral("0.1.66"), true);
  const QString text = text_of(dialog);
  EXPECT_TRUE(text.contains(QStringLiteral("PartyShare 0.1.67 is available")));
  EXPECT_TRUE(text.contains(QStringLiteral("You are running 0.1.66")));
}

TEST(UpdateDialog, TheButtonSaysWhatPressingItDoes) {
  const UpdateDialog direct(QStringLiteral("0.1.67"), QStringLiteral("0.1.66"), true);
  EXPECT_EQ(direct.update_button()->text(), QStringLiteral("Update now"));
  EXPECT_TRUE(text_of(direct).contains(QStringLiteral("downloads the installer")));

  // No installer to download: the button must not promise an update and then
  // open a list.
  const UpdateDialog page_only(QStringLiteral("0.1.67"), QStringLiteral("0.1.66"), false);
  EXPECT_EQ(page_only.update_button()->text(), QStringLiteral("Open the download page"));
  EXPECT_FALSE(text_of(page_only).contains(QStringLiteral("downloads the installer")));

  EXPECT_EQ(direct.later_button()->text(), QStringLiteral("Later"));
}

TEST(UpdateDialog, DoesNotTakeAnEnterThatWasMeantForSomethingElse) {
  UpdateDialog dialog(QStringLiteral("0.1.67"), QStringLiteral("0.1.66"), true);
  int accepted = 0;
  int rejected = 0;
  QObject::connect(&dialog, &QDialog::accepted, &dialog, [&accepted] { ++accepted; });
  QObject::connect(&dialog, &QDialog::rejected, &dialog, [&rejected] { ++rejected; });

  dialog.show();
  settle();
  // The moment it arrives: somebody is finishing a password, and the Enter
  // that ends it lands here.
  EXPECT_FALSE(dialog.update_button()->isEnabled());
  EXPECT_FALSE(dialog.later_button()->isEnabled());
  press_key(&dialog, Qt::Key_Return);
  press_key(&dialog, Qt::Key_Enter);
  press_key(&dialog, Qt::Key_Space);
  click(dialog.update_button());
  click(dialog.later_button());
  settle();
  EXPECT_EQ(accepted, 0);
  EXPECT_EQ(rejected, 0);
  EXPECT_TRUE(dialog.isVisible());

  // And then it answers, with the keyboard on the button that updates.
  ASSERT_TRUE(wait_until([&dialog] { return dialog.update_button()->isEnabled(); }));
  EXPECT_TRUE(dialog.later_button()->isEnabled());
  press_key(&dialog, Qt::Key_Return);
  settle();
  EXPECT_EQ(accepted, 1);
  EXPECT_EQ(rejected, 0);
  EXPECT_FALSE(dialog.isVisible());
}

TEST(UpdateDialog, LaterAndEscBothMeanLater) {
  {
    UpdateDialog dialog(QStringLiteral("0.1.67"), QStringLiteral("0.1.66"), true);
    int accepted = 0;
    int rejected = 0;
    QObject::connect(&dialog, &QDialog::accepted, &dialog, [&accepted] { ++accepted; });
    QObject::connect(&dialog, &QDialog::rejected, &dialog, [&rejected] { ++rejected; });
    dialog.show();
    ASSERT_TRUE(wait_until([&dialog] { return dialog.later_button()->isEnabled(); }));
    click(dialog.later_button());
    settle();
    EXPECT_EQ(accepted, 0);
    EXPECT_EQ(rejected, 1);
  }
  {
    // Esc is not held back with the buttons: nobody types it into a field by
    // accident, and a dialog that cannot be sent away is worse than one that
    // can be sent away early.
    UpdateDialog dialog(QStringLiteral("0.1.67"), QStringLiteral("0.1.66"), true);
    int rejected = 0;
    QObject::connect(&dialog, &QDialog::rejected, &dialog, [&rejected] { ++rejected; });
    dialog.show();
    settle();
    press_key(&dialog, Qt::Key_Escape);
    settle();
    EXPECT_EQ(rejected, 1);
    EXPECT_FALSE(dialog.isVisible());
  }
}
