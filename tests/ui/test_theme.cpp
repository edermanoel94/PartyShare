// The tokens every screen is drawn from. A mistake here is not a wrong pixel
// in one place: it is a wrong pixel everywhere, and most of the ones below
// are mistakes Qt reports to nobody - a stylesheet rule it could not parse is
// a rule it drops in silence.

#include <array>
#include <string>

#include <gtest/gtest.h>

#include <QColor>
#include <QObject>
#include <QString>

#include "ui/theme.hpp"
#include "ui_test.hpp"

namespace theme = dv::ui::theme;
using dv::ui::testing::contrast;
using dv::ui::testing::ThemeGuard;

namespace {

/// What WCAG asks of body text, and of a large or bold label on a control.
constexpr double kTextContrast = 4.5;
constexpr double kControlContrast = 3.0;

constexpr std::array<theme::Mode, 2> kHeldModes = {theme::Mode::Light, theme::Mode::Dark};

}  // namespace

TEST(Theme, EveryModeHasAWordAndReadsItBack) {
  // The word is what config.ini holds and what Settings writes: a mode that
  // does not survive the round trip is a choice that is forgotten at the next
  // launch.
  for (const theme::Mode mode : {theme::Mode::System, theme::Mode::Light, theme::Mode::Dark}) {
    const auto read = theme::parse_mode(theme::to_string(mode));
    ASSERT_TRUE(read.has_value()) << theme::to_string(mode);
    EXPECT_EQ(read.value_or(theme::Mode::System), mode);
  }
  EXPECT_FALSE(theme::parse_mode("sepia").has_value());
  EXPECT_FALSE(theme::parse_mode("").has_value());
}

TEST(Theme, TheStylesheetLeavesNoTokenUnfilled) {
  // A `@{name}` nobody replaced reaches Qt as a colour it cannot read, and Qt
  // answers by discarding the rule and saying nothing: the widget is drawn
  // from the palette, slightly wrong, on every machine. This is the only place
  // a token spelled wrongly, or added to the sheet and not to the table, is
  // ever reported.
  const ThemeGuard guard;
  for (const theme::Mode mode : kHeldModes) {
    theme::set_mode(mode);
    const QString sheet = theme::stylesheet();
    ASSERT_FALSE(sheet.isEmpty());
    const qsizetype at = sheet.indexOf(QStringLiteral("@{"));
    EXPECT_EQ(at, -1) << "unfilled token: " << sheet.mid(at, 32).toStdString();
  }
}

TEST(Theme, ChangingTheModeChangesTheColoursAndSaysSoOnce) {
  const ThemeGuard guard;
  theme::set_mode(theme::Mode::Dark);
  ASSERT_TRUE(theme::is_dark());
  const QColor dark_window = theme::colors().window;

  int told = 0;
  const QMetaObject::Connection listening = QObject::connect(
      theme::notifier(), &theme::Notifier::changed, theme::notifier(), [&told] { ++told; });

  theme::set_mode(theme::Mode::Light);
  EXPECT_FALSE(theme::is_dark());
  EXPECT_EQ(theme::mode(), theme::Mode::Light);
  EXPECT_GT(theme::colors().window.lightness(), dark_window.lightness());
  EXPECT_EQ(told, 1);

  // Asking for what is already in force re-polishes nothing: every widget
  // restyled for no change is a flicker about nothing.
  theme::set_mode(theme::Mode::Light);
  EXPECT_EQ(told, 1);

  QObject::disconnect(listening);
}

TEST(Theme, TextCanBeReadOnWhatItIsWrittenOn) {
  // The pairs that carry sentences. Measured rather than judged by eye,
  // because the eye is on one monitor and the people reading are on others.
  const ThemeGuard guard;
  for (const theme::Mode mode : kHeldModes) {
    theme::set_mode(mode);
    const theme::Colors& c = theme::colors();
    const std::string scheme(theme::to_string(mode));

    EXPECT_GE(contrast(c.text, c.window), kTextContrast) << scheme;
    EXPECT_GE(contrast(c.text, c.surface), kTextContrast) << scheme;
    EXPECT_GE(contrast(c.text, c.surface_raised), kTextContrast) << scheme;
    // Secondary text is written on all three: a hint in a card, a subtitle in
    // a header, a tooltip-sized label on the rail.
    EXPECT_GE(contrast(c.muted, c.surface), kTextContrast) << scheme;
    EXPECT_GE(contrast(c.muted, c.window), kTextContrast) << scheme;
    EXPECT_GE(contrast(c.muted, c.chrome), kTextContrast) << scheme;
    // The accent as text: a selected tab, a chip, a quiet button.
    EXPECT_GE(contrast(c.accent_text, c.accent_soft), kTextContrast) << scheme;
    EXPECT_GE(contrast(c.accent_text, c.surface), kTextContrast) << scheme;
    // The word "full" on a room's card.
    EXPECT_GE(contrast(c.warn, c.warn_soft), kTextContrast) << scheme;
    EXPECT_GE(contrast(c.danger, c.surface), kTextContrast) << scheme;
    // A filled button's label is bold and a control: the lower bar applies,
    // and the dark scheme's accent clears it with not much to spare.
    EXPECT_GE(contrast(c.on_accent, c.accent), kControlContrast) << scheme;
    // The ink a danger button and a notification's icon are drawn in.
    EXPECT_GE(contrast(theme::ink_on(c.danger), c.danger), kTextContrast) << scheme;
    EXPECT_GE(contrast(c.info, c.surface_raised), kControlContrast) << scheme;
    EXPECT_GE(contrast(c.success, c.surface_raised), kControlContrast) << scheme;
  }
}

TEST(Theme, InkIsDarkOnLightAndLightOnDark) {
  EXPECT_LT(theme::ink_on(QColor(Qt::white)).lightness(), 128);
  EXPECT_GT(theme::ink_on(QColor(Qt::black)).lightness(), 128);
}

TEST(Theme, MixStartsAtOneColourAndEndsAtTheOther) {
  const QColor from(10, 20, 30);
  const QColor to(210, 220, 230);
  EXPECT_EQ(theme::mix(from, to, 0.0).rgb(), from.rgb());
  EXPECT_EQ(theme::mix(from, to, 1.0).rgb(), to.rgb());
  // Out of range is clamped, not extrapolated: a hover animation that
  // overshoots must not paint a colour outside the two it moves between.
  EXPECT_EQ(theme::mix(from, to, 4.0).rgb(), to.rgb());
  EXPECT_EQ(theme::mix(from, to, -1.0).rgb(), from.rgb());
  const QColor half = theme::mix(from, to, 0.5);
  EXPECT_NEAR(half.red(), 110, 1);
  EXPECT_NEAR(half.green(), 120, 1);
}

TEST(Theme, EachStepOfTheTypeScaleIsLargerThanTheLast) {
  const double caption = theme::font(theme::TextRole::Caption).pointSizeF();
  const double body = theme::font(theme::TextRole::Body).pointSizeF();
  const double title = theme::font(theme::TextRole::Title).pointSizeF();
  const double heading = theme::font(theme::TextRole::Heading).pointSizeF();
  const double display = theme::font(theme::TextRole::Display).pointSizeF();
  EXPECT_LT(caption, body);
  EXPECT_LT(body, title);
  EXPECT_LT(title, heading);
  EXPECT_LT(heading, display);
  // A label names something at the size of the text beside it; what sets it
  // apart is its weight.
  EXPECT_DOUBLE_EQ(theme::font(theme::TextRole::Label).pointSizeF(), body);
  EXPECT_GT(theme::font(theme::TextRole::Label).weight(),
            theme::font(theme::TextRole::Body).weight());
}

TEST(Theme, AnimationsOffMeansNoDurationAtAll) {
  const ThemeGuard guard;
  theme::motion::set_enabled(true);
  EXPECT_EQ(theme::motion::duration(theme::motion::kBaseMs), theme::motion::kBaseMs);
  // Zero, not a shorter one: off is for a remote desktop, where every frame
  // of a transition is a frame sent down the wire.
  theme::motion::set_enabled(false);
  EXPECT_EQ(theme::motion::duration(theme::motion::kBaseMs), 0);
  EXPECT_EQ(theme::motion::duration(theme::motion::kSlowMs), 0);
}
