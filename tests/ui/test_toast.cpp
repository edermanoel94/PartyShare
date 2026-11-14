// How long a notification stays, and what the stack of them does. The first
// half is a policy somebody decided on purpose - docs/17-design-system.md has
// the table - and these are what stop it from being edited back by accident.

#include <gtest/gtest.h>

#include <QList>
#include <QMargins>
#include <QString>
#include <QWidget>

#include "ui/components/toast.hpp"
#include "ui/theme.hpp"
#include "ui_test.hpp"

using dv::ui::Toast;
using dv::ui::toast_lifetime_ms;
using dv::ui::ToastHost;
using dv::ui::ToastKind;
using dv::ui::testing::click;
using dv::ui::testing::press_key;
using dv::ui::testing::settle;

namespace {

/// A sentence of the length a refusal usually is. A function and not a
/// constant: a QString at namespace scope is built before main and can throw
/// where nothing can catch it.
[[nodiscard]] QString sentence() {
  return QStringLiteral("Somebody else is already sharing their screen.");
}

/// The notifications still on `container`, oldest first.
[[nodiscard]] QList<Toast*> toasts_on(const QWidget& container) {
  return container.findChildren<Toast*>(Qt::FindDirectChildrenOnly);
}

}  // namespace

TEST(ToastLifetime, ModerationWaitsToBeDismissedWhereverItLands) {
  // An administrator muting or restricting somebody changes what a person in
  // the room can do. It is the one kind not allowed to come and go unseen,
  // over a film included.
  EXPECT_EQ(toast_lifetime_ms(ToastKind::Moderation, sentence(), false), 0);
  EXPECT_EQ(toast_lifetime_ms(ToastKind::Moderation, sentence(), true), 0);
}

TEST(ToastLifetime, AnErrorWaitsOnAPageButNotOverAPicture) {
  EXPECT_EQ(toast_lifetime_ms(ToastKind::Error, sentence(), false), 0);

  // Nobody watching a film reaches for the mouse to send a notification
  // away, so over one it has to leave by itself - and later than plain news
  // would, because the eyes it is for were on the picture.
  const int over_picture = toast_lifetime_ms(ToastKind::Error, sentence(), true);
  EXPECT_GT(over_picture, 0);
  EXPECT_EQ(over_picture, 2 * toast_lifetime_ms(ToastKind::Info, sentence(), true));
}

TEST(ToastLifetime, PlainNewsLeavesByItselfAndWhereItIsDoesNotMatter) {
  for (const ToastKind kind : {ToastKind::Info, ToastKind::Success, ToastKind::Warning}) {
    const int on_page = toast_lifetime_ms(kind, sentence(), false);
    EXPECT_GT(on_page, 0);
    EXPECT_EQ(on_page, toast_lifetime_ms(kind, sentence(), true));
  }
}

TEST(ToastLifetime, ALongerSentenceStaysLongerAndAShortOneStillLongEnough) {
  const int two_words = toast_lifetime_ms(ToastKind::Info, QStringLiteral("Code copied"), false);
  const int a_sentence = toast_lifetime_ms(ToastKind::Info, sentence(), false);
  EXPECT_GT(a_sentence, two_words);
  // The floor: gone in under two seconds is gone before the eye has arrived.
  EXPECT_GE(toast_lifetime_ms(ToastKind::Info, QStringLiteral("Ok"), false), 2000);
}

TEST(ToastHost, ShowsWhatItIsGivenAndNothingForAnEmptySentence) {
  QWidget container;
  container.resize(800, 600);
  ToastHost host(&container);

  host.show_toast(ToastKind::Info, QString{});
  EXPECT_TRUE(toasts_on(container).isEmpty());

  host.show_toast(ToastKind::Info, sentence());
  ASSERT_EQ(toasts_on(container).size(), 1);
  EXPECT_EQ(toasts_on(container).first()->accessibleName(), sentence());
}

TEST(ToastHost, DoesNotSayTheSameThingTwiceBesideItself) {
  // A refusal repeated by an impatient click would otherwise fill the stack
  // with one message three times.
  QWidget container;
  container.resize(800, 600);
  ToastHost host(&container);

  host.show_toast(ToastKind::Error, sentence());
  host.show_toast(ToastKind::Error, sentence());
  host.show_toast(ToastKind::Error, sentence());
  EXPECT_EQ(toasts_on(container).size(), 1);
}

TEST(ToastHost, AFourthSendsTheOldestAway) {
  QWidget container;
  container.resize(800, 600);
  ToastHost host(&container);

  for (int number = 1; number <= 4; ++number) {
    host.show_toast(ToastKind::Moderation, QStringLiteral("notice %1").arg(number));
  }
  settle();

  const QList<Toast*> left = toasts_on(container);
  ASSERT_EQ(left.size(), 3);
  for (const Toast* toast : left) {
    EXPECT_NE(toast->accessibleName(), QStringLiteral("notice 1"));
  }
}

TEST(ToastHost, StacksDownwardsAndCentred) {
  QWidget container;
  container.resize(800, 600);
  container.show();
  ToastHost host(&container);
  host.set_margins(QMargins(0, 64, 0, 0));

  host.show_toast(ToastKind::Moderation, QStringLiteral("first"));
  host.show_toast(ToastKind::Moderation, QStringLiteral("second"));
  settle();

  const QList<Toast*> shown = toasts_on(container);
  ASSERT_EQ(shown.size(), 2);
  EXPECT_EQ(shown.at(0)->y(), 64);
  EXPECT_GT(shown.at(1)->y(), shown.at(0)->y());
  for (const Toast* toast : shown) {
    // Centred to within the pixel an odd width leaves over.
    EXPECT_NEAR(toast->geometry().center().x(), container.width() / 2.0, 1.0);
  }
}

TEST(ToastHost, GoesBackOnTopOfAPageThatArrivedAfterIt) {
  // A notification that waits to be dismissed outlives the page it was
  // raised over. The page that replaces it is put in front of it, and what
  // was left of the notification was one edge showing through a gap.
  QWidget container;
  container.resize(800, 600);
  ToastHost host(&container);
  host.show_toast(ToastKind::Moderation, sentence());
  ASSERT_EQ(toasts_on(container).size(), 1);

  auto* page = new QWidget(&container);
  page->raise();
  ASSERT_EQ(container.findChildren<QWidget*>(Qt::FindDirectChildrenOnly).last(), page);

  host.bring_to_front();
  EXPECT_EQ(container.findChildren<QWidget*>(Qt::FindDirectChildrenOnly).last(),
            toasts_on(container).first());
}

TEST(ToastHost, DismissingAllTakesTheOnesThatWaitAsWell) {
  // Signing out. What is up belongs to the session that ended, and the next
  // person to read it may not be who it was for.
  QWidget container;
  container.resize(800, 600);
  ToastHost host(&container);
  host.show_toast(ToastKind::Moderation, QStringLiteral("Carla was muted by Bruno"));
  host.show_toast(ToastKind::Error, sentence());
  host.show_toast(ToastKind::Info, QStringLiteral("Room code copied"));
  ASSERT_EQ(toasts_on(container).size(), 3);

  host.dismiss_all();
  settle();
  EXPECT_TRUE(toasts_on(container).isEmpty());

  // And the host is still good for the next one.
  host.show_toast(ToastKind::Info, QStringLiteral("Room code copied"));
  EXPECT_EQ(toasts_on(container).size(), 1);
}

TEST(Toast, AClickSendsItAway) {
  QWidget container;
  container.resize(800, 600);
  container.show();
  ToastHost host(&container);
  host.show_toast(ToastKind::Moderation, sentence());
  ASSERT_EQ(toasts_on(container).size(), 1);

  click(toasts_on(container).first());
  settle();
  EXPECT_TRUE(toasts_on(container).isEmpty());
}

TEST(Toast, SoDoesTheKeyboard) {
  // Some kinds wait to be dismissed, and a thing only a mouse can dismiss is
  // a thing a keyboard is stuck with.
  for (const Qt::Key key : {Qt::Key_Escape, Qt::Key_Space, Qt::Key_Return}) {
    QWidget container;
    container.resize(800, 600);
    container.show();
    ToastHost host(&container);
    host.show_toast(ToastKind::Moderation, sentence());
    ASSERT_EQ(toasts_on(container).size(), 1);
    EXPECT_NE(toasts_on(container).first()->focusPolicy(), Qt::NoFocus);

    press_key(toasts_on(container).first(), key);
    settle();
    EXPECT_TRUE(toasts_on(container).isEmpty()) << "key " << static_cast<int>(key);
  }
}

TEST(Toast, AnotherKeyLeavesItWhereItIs) {
  QWidget container;
  container.resize(800, 600);
  container.show();
  ToastHost host(&container);
  host.show_toast(ToastKind::Moderation, sentence());

  press_key(toasts_on(container).first(), Qt::Key_A);
  settle();
  EXPECT_EQ(toasts_on(container).size(), 1);
}
