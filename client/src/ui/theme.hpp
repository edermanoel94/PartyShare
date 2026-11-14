#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

#include <QColor>
#include <QFont>
#include <QObject>
#include <QString>

class QApplication;
class QPainter;
class QRectF;
class QWidget;

/// The design system: every colour, size, distance and duration the interface
/// is allowed to use, in one place. docs/17-design-system.md is the guide to
/// it; this header is the source.
namespace dv::ui::theme {

/// Every colour the interface is allowed to use.
///
/// One struct with two fillings, light and dark, rather than colours written
/// where they are drawn. The reason is the bug this replaced: the login error
/// was a literal #c62828 and the metrics line was palette(mid), so half the
/// interface followed the system's colour scheme and half of it did not, and
/// nobody could tell by reading any one file.
struct Colors {
  /// Behind everything.
  QColor window;
  /// The navigation rail and the header: one step off the window, so the
  /// frame of the program reads as a frame without a border doing the work.
  QColor chrome;
  /// Cards, group boxes, fields and lists: the paper the content sits on.
  QColor surface;
  QColor surface_hover;
  QColor surface_pressed;
  /// What floats above the page rather than sitting on it: a notification,
  /// the call controls over a shared screen. Lighter than the surface in the
  /// dark scheme, because there a shadow alone does not separate two darks.
  QColor surface_raised;
  QColor border;
  /// The border of something under the pointer, which is how a control says it
  /// can be clicked before it is.
  QColor border_strong;

  QColor text;
  /// Secondary text. Timestamps, hints, the metrics line.
  QColor muted;

  /// The brand indigo of assets/partyshare.svg. The interface and the icon on
  /// the dock are the same program and have no business being different
  /// colours.
  QColor accent;
  QColor accent_hover;
  QColor accent_pressed;
  /// A wash of the accent, for a control that is on rather than one that is
  /// the thing to press.
  QColor accent_soft;
  /// Text and icons drawn on top of `accent`.
  QColor on_accent;
  /// The accent as text on a surface or on `accent_soft`. Its own colour
  /// because `accent` is chosen to be a background: as 13 pixel text on its
  /// own wash it falls short of 4.5:1 in both schemes.
  QColor accent_text;

  QColor danger;
  QColor danger_soft;
  QColor warn;
  QColor warn_soft;
  QColor success;
  QColor success_soft;
  QColor info;
  QColor info_soft;

  /// Behind a shared screen, and the text shown when there is not one. Nearly
  /// black in both schemes: a picture is judged against what surrounds it, and
  /// a light grey surround makes every screen share look washed out.
  QColor canvas;
  QColor canvas_text;

  /// What a raised surface throws on what is under it. Carries its own alpha.
  QColor shadow;
};

/// The colours in force.
[[nodiscard]] const Colors& colors();

/// Which filling `colors()` has: the desktop's, or one held whatever the
/// desktop says. `[ui] theme` in config.ini, in the same three words.
enum class Mode : std::uint8_t { System, Light, Dark };

[[nodiscard]] std::optional<Mode> parse_mode(std::string_view text);
[[nodiscard]] std::string_view to_string(Mode mode);

[[nodiscard]] Mode mode();
/// Changes the colours of everything already on screen, at once. Persisting
/// the choice is the caller's business: see SettingsDialog.
void set_mode(Mode mode);
/// Whether the colours in force are the dark ones, whichever way that came
/// about.
[[nodiscard]] bool is_dark();

/// Distances, in device independent pixels. A layout takes its margins and
/// its spacing from here rather than from a number that looked right, so that
/// two screens built a month apart breathe the same.
namespace space {
inline constexpr int kXs = 4;
inline constexpr int kSm = 8;
inline constexpr int kMd = 12;
inline constexpr int kLg = 16;
inline constexpr int kXl = 24;
inline constexpr int kXxl = 32;
}  // namespace space

/// How round everything is, in pixels. Shared with the widgets that draw
/// themselves, so a corner painted by hand matches one drawn by the stylesheet.
inline constexpr int kCardRadius = 14;
inline constexpr int kControlRadius = 10;
inline constexpr int kChipRadius = 8;

/// What a piece of text is for, which decides its size and weight. A modular
/// scale - each step 1.2 times the last, from a 10 point body - so the sizes
/// are in proportion to each other rather than chosen one at a time.
enum class TextRole : std::uint8_t {
  /// Timestamps, hints, the numbers in the status bar.
  Caption,
  Body,
  /// A body-sized label that names something: a button, a table header.
  Label,
  /// The name of a card or a panel.
  Title,
  /// The name of the page, in the header.
  Heading,
  /// One number worth reading from across the room: a StatCard's figure.
  Display,
};

[[nodiscard]] QFont font(TextRole role);

/// Gives a label the size, weight and colour of `role`, through the
/// stylesheet rather than through setFont: a font set by hand is a font the
/// next change of theme does not reach.
void set_role(QWidget* label, TextRole role);

/// `from`, moved `amount` of the way to `to`: 0 is the first colour, 1 the
/// second. What a hover animation draws on each frame.
[[nodiscard]] QColor mix(const QColor& from, const QColor& to, qreal amount);

/// Near black or white, whichever is the easier to read on `background`. For
/// text on a colour that is not a surface - the danger red is light in the
/// dark scheme and dark in the light one, and white is only right on one.
[[nodiscard]] QColor ink_on(const QColor& background);

/// How long things take to move, in milliseconds. Small things are quick and
/// whole pages are slower, and nothing takes long enough to be waited for.
namespace motion {
inline constexpr int kFastMs = 120;
inline constexpr int kBaseMs = 180;
inline constexpr int kSlowMs = 260;

/// `[ui] animations`. Off means every transition lands on its last frame at
/// once - not a slower or a shorter one.
[[nodiscard]] bool enabled();
void set_enabled(bool enabled);

/// `ms` when animations are on, zero when they are off. What an animation is
/// given for a duration, so that no caller has to remember to ask.
[[nodiscard]] int duration(int ms);
}  // namespace motion

/// Says when the colours changed, to the few widgets a re-polish does not
/// reach: the ones that keep something made from a colour - a rendered icon,
/// a cached pixmap - rather than reading `colors()` each time they paint.
// A QObject cannot be copied or moved, and this one lives as long as the
// application does.
class Notifier : public QObject {
  Q_OBJECT

 public:
  using QObject::QObject;

 signals:
  void changed();

 protected:
  /// Watches for windows being shown, to give each one the title bar the
  /// colours in force call for. See theme_windows.cpp.
  bool eventFilter(QObject* watched, QEvent* event) override;
};

[[nodiscard]] Notifier* notifier();

/// A soft shadow under the rounded rectangle `box`, painted rather than made
/// with QGraphicsDropShadowEffect. The effect renders its widget offscreen and
/// blurs it again on every repaint, and the things that float in this program
/// float over a picture that repaints thirty times a second.
void paint_shadow(QPainter& painter, const QRectF& box, qreal radius);

/// Installs the palette and the stylesheet, and keeps them following the
/// system's colour scheme - while the mode is System - for as long as the
/// application runs.
///
/// Called once, before the first window is built.
void apply(QApplication& application, Mode mode = Mode::System);

/// The stylesheet for `colors()`. Exposed for whoever wants to look at it.
[[nodiscard]] QString stylesheet();

/// Makes `window`'s title bar dark or light, where the platform draws one that
/// can be asked. Windows only; nothing happens anywhere else.
void platform_set_title_bar(QWidget* window, bool dark);

}  // namespace dv::ui::theme
