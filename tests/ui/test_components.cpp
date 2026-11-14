// The component library, one behaviour at a time. Each of these is a promise
// a page relies on without checking: that the rail does not switch pages by
// itself, that a filtered table says why it is empty, that every icon is
// actually drawn.

#include <gtest/gtest.h>

#include <QAbstractItemView>
#include <QColor>
#include <QDialog>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QList>
#include <QPixmap>
#include <QPushButton>
#include <QString>
#include <QStringList>
#include <QTableWidget>
#include <QVariant>
#include <QWidget>

#include "ui/components/app_dialog.hpp"
#include "ui/components/app_sidebar.hpp"
#include "ui/components/avatar.hpp"
#include "ui/components/buttons.hpp"
#include "ui/components/data_table.hpp"
#include "ui/components/empty_state.hpp"
#include "ui/components/icon_button.hpp"
#include "ui/components/icons.hpp"
#include "ui/components/search_input.hpp"
#include "ui/components/stat_card.hpp"
#include "ui/table.hpp"
#include "ui_test.hpp"

using dv::ui::AppDialog;
using dv::ui::AppForm;
using dv::ui::AppSidebar;
using dv::ui::Avatar;
using dv::ui::DataTable;
using dv::ui::EmptyState;
using dv::ui::Icon;
using dv::ui::IconButton;
using dv::ui::testing::click;
using dv::ui::testing::press_key;
using dv::ui::testing::settle;

namespace {

/// Whether anything at all was drawn into `pixmap`.
[[nodiscard]] bool has_ink(const QPixmap& pixmap) {
  const QImage image = pixmap.toImage();
  for (int y = 0; y < image.height(); ++y) {
    for (int x = 0; x < image.width(); ++x) {
      if (qAlpha(image.pixel(x, y)) != 0) {
        return true;
      }
    }
  }
  return false;
}

/// Three rooms as the rows a table is filled from: identifier, then columns.
[[nodiscard]] QStringList three_rows() {
  return {QStringLiteral("A1\tA1\tGame night"), QStringLiteral("B2\tB2\tSprint review"),
          QStringLiteral("C3\tC3\tStandup")};
}

}  // namespace

// --- icons ------------------------------------------------------------------

TEST(Icons, EveryIconDrawsSomething) {
  // An icon added to the enum and not to the switch that draws it is a button
  // with nothing on it, and the compiler says nothing about a missing case in
  // a function that falls through to drawing an empty path.
  for (int value = 0; value <= static_cast<int>(Icon::ArrowLeft); ++value) {
    const QPixmap pixmap =
        dv::ui::icon_pixmap(static_cast<Icon>(value), 24, QColor(Qt::white), 1.0);
    EXPECT_TRUE(has_ink(pixmap)) << "icon " << value;
  }
}

TEST(Icons, APixmapIsRenderedAtTheDensityAskedFor) {
  // Sixteen logical pixels on a screen at 200% is thirty-two real ones. An
  // icon rendered at sixteen and stretched is the soft icon of a program
  // nobody finished.
  const QPixmap dense = dv::ui::icon_pixmap(Icon::Check, 16, QColor(Qt::white), 2.0);
  EXPECT_EQ(dense.width(), 32);
  EXPECT_DOUBLE_EQ(dense.devicePixelRatio(), 2.0);
}

// --- icon button ------------------------------------------------------------

TEST(IconButton, IsSquareWithoutALabelAndWiderWithOne) {
  IconButton button(Icon::Screen);
  button.set_extent(44);
  EXPECT_EQ(button.sizeHint(), QSize(44, 44));

  button.set_label(QStringLiteral("Share screen"));
  EXPECT_GT(button.sizeHint().width(), 44);
  EXPECT_EQ(button.sizeHint().height(), 44);
  // The label is its name for a screen reader; an icon is not a name.
  EXPECT_EQ(button.accessibleName(), QStringLiteral("Share screen"));

  const int short_width = button.sizeHint().width();
  button.set_label(QStringLiteral("Stop sharing the screen"));
  EXPECT_GT(button.sizeHint().width(), short_width);
}

TEST(IconButton, ACheckableOneTogglesAndSaysSo) {
  IconButton button(Icon::Microphone);
  button.setCheckable(true);
  int toggles = 0;
  QObject::connect(&button, &IconButton::toggled, &button, [&toggles] { ++toggles; });

  click(&button);
  EXPECT_TRUE(button.isChecked());
  click(&button);
  EXPECT_FALSE(button.isChecked());
  EXPECT_EQ(toggles, 2);
}

TEST(IconButton, ADisabledOneDoesNothing) {
  IconButton button(Icon::Leave);
  int clicks = 0;
  QObject::connect(&button, &IconButton::clicked, &button, [&clicks] { ++clicks; });
  button.setEnabled(false);
  click(&button);
  EXPECT_EQ(clicks, 0);
}

TEST(IconButton, TheKeyboardReachesItAndPressesIt) {
  IconButton button(Icon::Settings);
  EXPECT_NE(button.focusPolicy(), Qt::NoFocus);
  int clicks = 0;
  QObject::connect(&button, &IconButton::clicked, &button, [&clicks] { ++clicks; });
  press_key(&button, Qt::Key_Space);
  EXPECT_EQ(clicks, 1);
}

// --- navigation rail --------------------------------------------------------

TEST(AppSidebar, AskingToGoSomewhereDoesNotSelectIt) {
  // The rail shows where the window is. A place that lit up on the click
  // would stay lit over a page that was refused and never came.
  AppSidebar rail;
  IconButton* rooms = rail.add_destination(0, Icon::Rooms, QStringLiteral("Rooms"));
  IconButton* admin = rail.add_destination(1, Icon::Shield, QStringLiteral("Administration"));
  rail.set_current(0);

  QList<int> asked;
  QObject::connect(&rail, &AppSidebar::destination_chosen, &rail,
                   [&asked](int destination) { asked.append(destination); });

  click(admin);
  EXPECT_EQ(asked, QList<int>{1});
  EXPECT_TRUE(rooms->isChecked());
  EXPECT_FALSE(admin->isChecked());

  // The listener says so when the page really changed, and only then.
  rail.set_current(1);
  EXPECT_FALSE(rooms->isChecked());
  EXPECT_TRUE(admin->isChecked());
}

TEST(AppSidebar, PressingWhereTheWindowAlreadyIsStillAsks) {
  // The first place of the rail is both the list of rooms and the room, and
  // pressing it from the administration panel is the way back to a call.
  AppSidebar rail;
  IconButton* rooms = rail.add_destination(0, Icon::Rooms, QStringLiteral("Rooms"));
  rail.set_current(0);
  int asked = 0;
  QObject::connect(&rail, &AppSidebar::destination_chosen, &rail, [&asked] { ++asked; });

  click(rooms);
  EXPECT_EQ(asked, 1);
  EXPECT_TRUE(rooms->isChecked());
}

TEST(AppSidebar, AnActionNeverLooksSelected) {
  AppSidebar rail;
  IconButton* settings = rail.add_action(Icon::Settings, QStringLiteral("Settings"));
  EXPECT_FALSE(settings->isCheckable());
  EXPECT_EQ(settings->accessibleName(), QStringLiteral("Settings"));
  EXPECT_EQ(settings->toolTip(), QStringLiteral("Settings"));
}

// --- buttons ----------------------------------------------------------------

TEST(Buttons, EachKindAsksTheStylesheetForItsLooksByProperty) {
  // The property is the whole of the styling contract: it is what lets a
  // button dropped into a Designer form with the same property be the same
  // button. See docs/17-design-system.md, section 5.
  EXPECT_TRUE(dv::ui::PrimaryButton(QStringLiteral("Create")).property("accent").toBool());
  EXPECT_TRUE(dv::ui::DangerButton(QStringLiteral("Delete")).property("danger").toBool());
  EXPECT_EQ(dv::ui::QuietButton(QStringLiteral("Message")).property("quiet").toString(),
            QStringLiteral("accent"));
  const dv::ui::SecondaryButton plain(QStringLiteral("Cancel"));
  EXPECT_FALSE(plain.property("accent").isValid());
  EXPECT_FALSE(plain.property("danger").isValid());
}

TEST(Buttons, CanBeBuiltFromAParentAloneTheWayDesignerBuildsThem) {
  QWidget form;
  const dv::ui::PrimaryButton promoted(&form);
  EXPECT_EQ(promoted.parentWidget(), &form);
  EXPECT_TRUE(promoted.property("accent").toBool());
  EXPECT_TRUE(promoted.text().isEmpty());
}

TEST(Buttons, ALeadingIconIsThereAndSurvivesAChangeOfTheme) {
  const dv::ui::testing::ThemeGuard guard;
  dv::ui::PrimaryButton button(QStringLiteral("New room"));
  EXPECT_TRUE(button.icon().isNull());
  button.set_leading_icon(Icon::Plus);
  EXPECT_FALSE(button.icon().isNull());

  // Rendered again in the new colours, not left as a picture of the old ones
  // - and certainly not lost.
  dv::ui::theme::set_mode(dv::ui::theme::Mode::Light);
  EXPECT_FALSE(button.icon().isNull());
}

// --- search -----------------------------------------------------------------

TEST(SearchInput, IsALineEditWithANameAndAWayToEmptyIt) {
  dv::ui::SearchInput search(QStringLiteral("Search rooms or paste a code"));
  EXPECT_EQ(search.placeholderText(), QStringLiteral("Search rooms or paste a code"));
  // A placeholder is not read by every screen reader.
  EXPECT_EQ(search.accessibleName(), QStringLiteral("Search rooms or paste a code"));
  EXPECT_TRUE(search.isClearButtonEnabled());
  EXPECT_TRUE(search.property("search").toBool());
}

// --- tables -----------------------------------------------------------------

TEST(Table, IsReadOnlyOneRowAtATimeAndHasNoGrid) {
  QTableWidget table;
  dv::ui::set_up_table(&table, {QStringLiteral("Room"), QStringLiteral("Name")});
  EXPECT_EQ(table.columnCount(), 2);
  EXPECT_EQ(table.editTriggers(), QAbstractItemView::NoEditTriggers);
  EXPECT_EQ(table.selectionBehavior(), QAbstractItemView::SelectRows);
  EXPECT_EQ(table.selectionMode(), QAbstractItemView::SingleSelection);
  EXPECT_FALSE(table.showGrid());
}

TEST(DataTable, KeepsTheSelectionByIdentifierWhenTheRowsMove) {
  // The order changes as rooms come and go, and a selection that jumps to a
  // different row between a refresh and a click is how the wrong account
  // gets deleted.
  DataTable table({QStringLiteral("Room"), QStringLiteral("Name")});
  table.set_rows(three_rows());
  table.selectRow(1);
  ASSERT_EQ(table.selected_id(), QStringLiteral("B2"));

  QStringList reordered = three_rows();
  reordered.move(1, 0);
  reordered.prepend(QStringLiteral("Z9\tZ9\tNew arrival"));
  table.set_rows(reordered);
  EXPECT_EQ(table.selected_id(), QStringLiteral("B2"));
}

TEST(DataTable, AFilterHidesWhatDoesNotMatchWhateverItsCase) {
  DataTable table({QStringLiteral("Room"), QStringLiteral("Name")});
  table.set_rows(three_rows());

  table.set_filter(QStringLiteral("SPRINT"));
  EXPECT_TRUE(table.isRowHidden(0));
  EXPECT_FALSE(table.isRowHidden(1));
  EXPECT_TRUE(table.isRowHidden(2));

  // By any column, the code as well as the name.
  table.set_filter(QStringLiteral("c3"));
  EXPECT_FALSE(table.isRowHidden(2));
  EXPECT_TRUE(table.isRowHidden(0));

  table.set_filter(QString{});
  for (int row = 0; row < table.rowCount(); ++row) {
    EXPECT_FALSE(table.isRowHidden(row));
  }
}

TEST(DataTable, AFilterStillAppliesToRowsThatArriveLater) {
  DataTable table({QStringLiteral("Room"), QStringLiteral("Name")});
  table.set_filter(QStringLiteral("standup"));
  table.set_rows(three_rows());
  EXPECT_TRUE(table.isRowHidden(0));
  EXPECT_FALSE(table.isRowHidden(2));
}

TEST(DataTable, SaysWhichKindOfEmptyItIs) {
  // A table with no rows is indistinguishable from a table that failed to
  // load - and a list the filter emptied is a third thing again.
  DataTable table({QStringLiteral("Room"), QStringLiteral("Name")});
  table.resize(400, 300);
  table.show();
  table.set_empty_state(Icon::Rooms, QStringLiteral("No rooms yet"),
                        QStringLiteral("Create the first one."));

  auto* empty = table.findChild<EmptyState*>();
  ASSERT_NE(empty, nullptr);
  const auto title = [empty] { return empty->findChildren<QLabel*>().first()->text(); };
  EXPECT_TRUE(empty->isVisible());
  EXPECT_EQ(title(), QStringLiteral("No rooms yet"));

  table.set_rows(three_rows());
  EXPECT_FALSE(empty->isVisible());

  table.set_filter(QStringLiteral("nothing like this"));
  EXPECT_TRUE(empty->isVisible());
  EXPECT_NE(title(), QStringLiteral("No rooms yet"));

  // And back to the first sentence when the filter goes and so do the rows.
  table.set_filter(QString{});
  table.set_rows({});
  EXPECT_TRUE(empty->isVisible());
  EXPECT_EQ(title(), QStringLiteral("No rooms yet"));
}

// --- dialogs and forms ------------------------------------------------------

TEST(AppDialog, TheButtonThatAnswersItIsLastAndTheDefault) {
  AppDialog dialog(QStringLiteral("New room"));
  // Asked for in the order a caller happens to write them.
  QPushButton* create = dialog.add_primary(QStringLiteral("Create room"));
  QPushButton* cancel = dialog.add_secondary(QStringLiteral("Cancel"));
  dialog.show();
  settle();

  EXPECT_TRUE(create->isDefault());
  EXPECT_FALSE(cancel->isDefault());
  // Left to right: Cancel, then the answer, where the eye and the pointer end.
  EXPECT_LT(cancel->x(), create->x());
}

TEST(AppDialog, ThePrimaryAcceptsAndTheFirstSecondaryRejects) {
  {
    AppDialog dialog(QStringLiteral("New room"));
    QPushButton* create = dialog.add_primary(QStringLiteral("Create room"));
    dialog.add_secondary(QStringLiteral("Cancel"));
    dialog.show();
    click(create);
    EXPECT_EQ(dialog.result(), QDialog::Accepted);
  }
  {
    AppDialog dialog(QStringLiteral("New room"));
    dialog.add_primary(QStringLiteral("Create room"));
    QPushButton* cancel = dialog.add_secondary(QStringLiteral("Cancel"));
    dialog.show();
    click(cancel);
    EXPECT_EQ(dialog.result(), QDialog::Rejected);
    EXPECT_FALSE(dialog.isVisible());
  }
}

TEST(AppDialog, ACallerCanKeepItOpenUntilTheAnswerArrives) {
  // What the New room dialog does: the room exists when the server says so,
  // and until then a refusal needs somewhere to be read.
  AppDialog dialog(QStringLiteral("New room"));
  QPushButton* create = dialog.add_primary(QStringLiteral("Create room"));
  QObject::disconnect(create, &QPushButton::clicked, &dialog, &QDialog::accept);
  dialog.show();
  click(create);
  EXPECT_TRUE(dialog.isVisible());
}

TEST(AppForm, AnErrorTakesNoRoomUntilThereIsOne) {
  AppForm form;
  auto* name = new QLineEdit(&form);
  form.add_row(QStringLiteral("Name"), name);
  form.show();

  const auto errors = [&form] {
    QList<QLabel*> found;
    for (QLabel* label : form.findChildren<QLabel*>()) {
      if (label->property("error").toBool()) {
        found.append(label);
      }
    }
    return found;
  };
  ASSERT_EQ(errors().size(), 1);
  EXPECT_FALSE(errors().first()->isVisible());

  form.show_error(QStringLiteral("Another room already has that name."));
  EXPECT_TRUE(errors().first()->isVisible());
  EXPECT_EQ(errors().first()->text(), QStringLiteral("Another room already has that name."));

  // Taken away again, not left as a blank line: a form that keeps a row for
  // an error it does not have jumps the first time it gets one.
  form.show_error(QString{});
  EXPECT_FALSE(errors().first()->isVisible());
}

// --- cards ------------------------------------------------------------------

TEST(StatCard, TheStatusLineIsOnlyThereWhenThereIsSomethingToSay) {
  dv::ui::StatCard card(QStringLiteral("Server"));
  card.show();
  const auto status = [&card] {
    // Caption, value, status: the third label is the line under the number.
    return card.findChildren<QLabel*>().at(2);
  };
  EXPECT_FALSE(status()->isVisible());

  card.set_value(QStringLiteral("12 ms"));
  card.set_status(QStringLiteral("good"), dv::ui::StatCard::Tone::Good);
  EXPECT_TRUE(status()->isVisible());
  // The word is in it, not only the colour.
  EXPECT_TRUE(status()->text().contains(QStringLiteral("good")));

  card.set_status(QString{});
  EXPECT_FALSE(status()->isVisible());
}

TEST(StatCard, WhatIsWrittenInItIsTextAndNotMarkup) {
  // The status line is rich text, for its coloured dot. What goes into it
  // may one day come from a server.
  dv::ui::StatCard card(QStringLiteral("Server"));
  card.set_status(QStringLiteral("<b>good</b>"));
  EXPECT_TRUE(card.findChildren<QLabel*>().at(2)->text().contains(QStringLiteral("&lt;b&gt;")));
}

// --- people -----------------------------------------------------------------

TEST(Avatar, TheSamePersonIsTheSameColourEveryTime) {
  // The room list, the participant list and the chat all draw the badge, and
  // nobody chose the colour: it has to come out of the name the same way in
  // each of them.
  const dv::ui::AvatarColours first = dv::ui::avatar_colours(QStringLiteral("Carla"));
  const dv::ui::AvatarColours again = dv::ui::avatar_colours(QStringLiteral("Carla"));
  EXPECT_EQ(first.ground, again.ground);
  EXPECT_EQ(first.ink, again.ink);
  EXPECT_GE(dv::ui::testing::contrast(first.ink, first.ground), 4.5);
}

TEST(Avatar, IsAPictureUnlessToldItIsAButton) {
  Avatar avatar(34);
  avatar.set_name(QStringLiteral("Carla"));
  int clicks = 0;
  QObject::connect(&avatar, &Avatar::clicked, &avatar, [&clicks] { ++clicks; });

  click(&avatar);
  press_key(&avatar, Qt::Key_Space);
  EXPECT_EQ(clicks, 0);
  EXPECT_EQ(avatar.focusPolicy(), Qt::NoFocus);

  avatar.set_interactive(true);
  EXPECT_NE(avatar.focusPolicy(), Qt::NoFocus);
  click(&avatar);
  press_key(&avatar, Qt::Key_Space);
  press_key(&avatar, Qt::Key_Return);
  EXPECT_EQ(clicks, 3);
}

TEST(Avatar, BeingToldWhatItAlreadyShowsCostsNoRepaint) {
  // The room hands every badge its name and whether it is speaking several
  // times a second, for the whole of a call. A repaint for each would be a
  // row of badges redrawn all call long to stay exactly as they were.
  Avatar avatar(28);
  avatar.set_name(QStringLiteral("Carla"));
  avatar.set_ring(true);
  avatar.show();
  settle();

  // A tooltip set by hand is the cheapest thing to see being overwritten:
  // set_name writes the name into it whenever it does anything at all.
  avatar.setToolTip(QStringLiteral("Carla - speaking"));
  avatar.set_name(QStringLiteral("Carla"));
  avatar.set_ring(true);
  EXPECT_EQ(avatar.toolTip(), QStringLiteral("Carla - speaking"));

  avatar.set_name(QStringLiteral("Bruno"));
  EXPECT_EQ(avatar.toolTip(), QStringLiteral("Bruno"));
}

// --- empty states -----------------------------------------------------------

TEST(EmptyState, OffersTheOneThingToDoAboutIt) {
  EmptyState empty(Icon::Rooms, QStringLiteral("No rooms yet"),
                   QStringLiteral("Create the first one."));
  EXPECT_TRUE(empty.findChildren<QPushButton*>().isEmpty());

  QPushButton* action = empty.add_action(QStringLiteral("New room"));
  ASSERT_NE(action, nullptr);
  EXPECT_EQ(action->text(), QStringLiteral("New room"));
  EXPECT_EQ(action->parentWidget(), &empty);
}
