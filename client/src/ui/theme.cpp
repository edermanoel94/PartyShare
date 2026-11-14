#include "ui/theme.hpp"

#include <algorithm>
#include <array>
#include <cmath>

#include <QApplication>
#include <QCoreApplication>
#include <QEvent>
#include <QFont>
#include <QGuiApplication>
#include <QPainter>
#include <QPalette>
#include <QPointer>
#include <QRectF>
#include <QString>
#include <QStyle>
#include <QStyleHints>
#include <QWidget>

namespace dv::ui::theme {
namespace {

/// Light.
///
/// The window is not white. A white window behind white cards leaves nothing
/// to separate them but a hairline, and the whole interface reads as one flat
/// sheet; a faintly indigo grey gives the cards something to sit on.
Colors light() {
  return Colors{
      .window = QColor(0xF3, 0xF4, 0xFA),
      .chrome = QColor(0xFF, 0xFF, 0xFF),
      .surface = QColor(0xFF, 0xFF, 0xFF),
      .surface_hover = QColor(0xF7, 0xF8, 0xFD),
      .surface_pressed = QColor(0xEC, 0xEE, 0xF8),
      .surface_raised = QColor(0xFF, 0xFF, 0xFF),
      .border = QColor(0xDF, 0xE1, 0xEC),
      .border_strong = QColor(0xC2, 0xC6, 0xDC),
      .text = QColor(0x1C, 0x1D, 0x29),
      // A shade darker than the #6C7089 it was. That one cleared 4.5:1 on a
      // surface and fell just short of it on the window, at 4.43 - and the
      // window is what a page's subtitle and the status bar are written on.
      .muted = QColor(0x68, 0x6C, 0x85),
      .accent = QColor(0x5B, 0x5B, 0xD6),
      .accent_hover = QColor(0x6B, 0x6B, 0xDE),
      .accent_pressed = QColor(0x4A, 0x4A, 0xC0),
      .accent_soft = QColor(0xE7, 0xE7, 0xFA),
      .on_accent = QColor(0xFF, 0xFF, 0xFF),
      .accent_text = QColor(0x4A, 0x4A, 0xC0),
      .danger = QColor(0xD1, 0x3A, 0x3A),
      .danger_soft = QColor(0xFC, 0xEC, 0xEC),
      // Darker than the #B5660A it was, which read at 3.9:1 on its own wash:
      // enough for an icon, not for the word "full" at the size of a caption.
      .warn = QColor(0xA5, 0x5C, 0x08),
      .warn_soft = QColor(0xFD, 0xF3, 0xE4),
      .success = QColor(0x2E, 0x7D, 0x32),
      .success_soft = QColor(0xE6, 0xF4, 0xE7),
      .info = QColor(0x1D, 0x6F, 0xB8),
      .info_soft = QColor(0xE5, 0xF0, 0xFA),
      .canvas = QColor(0x14, 0x15, 0x1C),
      .canvas_text = QColor(0x8B, 0x8F, 0xA6),
      .shadow = QColor(0x1C, 0x1D, 0x29, 0x30),
  };
}

/// Dark.
///
/// The accent is lighter than the light scheme's, not darker. #5b5bd6 on a
/// near black window is a button that has to be looked for; the same hue lifted
/// is the same brand and is legible.
Colors dark() {
  return Colors{
      .window = QColor(0x15, 0x16, 0x1E),
      .chrome = QColor(0x1A, 0x1B, 0x25),
      .surface = QColor(0x1E, 0x1F, 0x2A),
      .surface_hover = QColor(0x26, 0x28, 0x35),
      .surface_pressed = QColor(0x2E, 0x30, 0x40),
      .surface_raised = QColor(0x26, 0x28, 0x35),
      .border = QColor(0x2E, 0x30, 0x40),
      .border_strong = QColor(0x45, 0x48, 0x5F),
      .text = QColor(0xE8, 0xE9, 0xF0),
      .muted = QColor(0x99, 0x9D, 0xB4),
      .accent = QColor(0x7B, 0x7B, 0xEC),
      .accent_hover = QColor(0x8C, 0x8C, 0xF2),
      .accent_pressed = QColor(0x69, 0x69, 0xDB),
      .accent_soft = QColor(0x2A, 0x2B, 0x4B),
      .on_accent = QColor(0xFF, 0xFF, 0xFF),
      .accent_text = QColor(0xA9, 0xA9, 0xF5),
      .danger = QColor(0xF1, 0x6B, 0x6B),
      .danger_soft = QColor(0x3A, 0x22, 0x25),
      .warn = QColor(0xEB, 0xA9, 0x50),
      .warn_soft = QColor(0x38, 0x2B, 0x1A),
      .success = QColor(0x6F, 0xCF, 0x7A),
      .success_soft = QColor(0x1F, 0x33, 0x24),
      .info = QColor(0x6C, 0xB6, 0xF5),
      .info_soft = QColor(0x1B, 0x2C, 0x3D),
      .canvas = QColor(0x0D, 0x0E, 0x14),
      .canvas_text = QColor(0x74, 0x78, 0x8E),
      .shadow = QColor(0x00, 0x00, 0x00, 0x70),
  };
}

/// The colours in force. A function local rather than a namespace scope object
/// so that nothing can read it before apply() has decided what it holds.
Colors& current() {
  static Colors colors = light();
  return colors;
}

/// Whether the system is asking for a dark interface.
///
/// Unknown counts as light, which is what Qt's own default palette does.
bool system_is_dark() {
  return QGuiApplication::styleHints()->colorScheme() == Qt::ColorScheme::Dark;
}

/// What was asked for, which is not the same as what is on screen: System is
/// an instruction to go and look.
Mode& chosen_mode() {
  static Mode chosen = Mode::System;
  return chosen;
}

/// Whether `current()` holds the dark filling. Kept rather than worked out by
/// comparing colours, so that the title bar and the colours cannot disagree.
bool& dark_in_force() {
  static bool dark = false;
  return dark;
}

bool& animations_enabled() {
  static bool enabled = true;
  return enabled;
}

[[nodiscard]] bool wants_dark(Mode mode) {
  switch (mode) {
    case Mode::Light:
      return false;
    case Mode::Dark:
      return true;
    case Mode::System:
      break;
  }
  return system_is_dark();
}

/// The stylesheet, with @{name} standing in for a colour.
///
/// Braces around the name and not the name alone, so that @accent cannot eat
/// the first seven characters of @accent_hover and leave "_hover" behind. That
/// mistake produces a stylesheet Qt discards a rule at a time, in silence.
///
/// What is here and what is not: this file gives shape - corners, padding,
/// borders, the states a control goes through. The palette below gives colour
/// to everything this does not name, so a widget nobody thought about still
/// comes out in the right colours rather than in the desktop's.
constexpr const char* kStyleSheet = R"qss(
QMainWindow, QDialog {
  background: @{window};
}

/* --- text ---------------------------------------------------------------- */

QLabel {
  background: transparent;
  color: @{text};
}
QLabel[hint="true"] {
  color: @{muted};
}
QLabel[error="true"] {
  color: @{danger};
}
QLabel[accent="true"] {
  color: @{accent};
  font-weight: 700;
}

QToolTip {
  background: @{surface};
  color: @{text};
  border: 1px solid @{border};
  border-radius: 8px;
  padding: 6px 8px;
}

/* --- grouping ------------------------------------------------------------ */

/* The margin at the top is where the title goes, entirely above the border
   rather than straddling it. Qt positions the title in the margin box, so a
   margin shorter than the text leaves it sitting on the border with the line
   through it. */
QGroupBox {
  background: @{surface};
  border: 1px solid @{border};
  border-radius: @{card_radius}px;
  margin-top: 24px;
  padding: 16px;
  font-weight: 600;
}
QGroupBox::title {
  subcontrol-origin: margin;
  subcontrol-position: top left;
  left: 4px;
  top: 1px;
  padding: 0;
  color: @{muted};
}

QSplitter::handle {
  background: transparent;
}
QSplitter::handle:horizontal {
  width: 10px;
}
QSplitter::handle:vertical {
  height: 10px;
}

/* --- buttons ------------------------------------------------------------- */

QPushButton {
  background: @{surface};
  color: @{text};
  border: 1px solid @{border};
  border-radius: @{control_radius}px;
  padding: 8px 18px;
  font-weight: 600;
}
QPushButton:hover {
  background: @{surface_hover};
  border-color: @{border_strong};
}
QPushButton:pressed {
  background: @{surface_pressed};
}
QPushButton:disabled {
  background: @{window};
  border-color: @{border};
  color: @{muted};
}

/* The one thing to press on the screen it is on. */
QPushButton[accent="true"] {
  background: @{accent};
  border-color: @{accent};
  color: @{on_accent};
}
QPushButton[accent="true"]:hover {
  background: @{accent_hover};
  border-color: @{accent_hover};
}
QPushButton[accent="true"]:pressed {
  background: @{accent_pressed};
  border-color: @{accent_pressed};
}
QPushButton[accent="true"]:disabled {
  background: @{accent_soft};
  border-color: @{accent_soft};
  color: @{muted};
}

/* Leaving the room and deleting an account: outlined until the pointer is on
   it, because a wall of red is a thing people learn to stop reading. */
QPushButton[danger="true"] {
  background: @{danger_soft};
  border-color: @{danger};
  color: @{danger};
}
QPushButton[danger="true"]:hover {
  background: @{danger};
  border-color: @{danger};
  color: #ffffff;
}
QPushButton[danger="true"]:pressed {
  background: @{danger};
  border-color: @{danger};
  color: #ffffff;
}

/* A toggle that is on. Sharing is a good state and wears the accent; a muted
   microphone is a warning and wears amber, so the two are told apart at a
   glance and not by reading the label. */
QPushButton[toggle="accent"]:checked {
  background: @{accent_soft};
  border-color: @{accent};
  color: @{accent};
}
QPushButton[toggle="warn"]:checked {
  background: @{warn_soft};
  border-color: @{warn};
  color: @{warn};
}

/* Focus, last and one rule per variant, which is not tidiness but the only
   arrangement that works.

   Qt resolves a conflict between two rules by the CSS2 specificity count: the
   number of id selectors, then of attributes and pseudo-states, then of type
   names. A lone QPushButton:focus scores 0-1-1, and so does
   QPushButton[accent="true"] - one type name, one qualifier each. Equal scores
   are broken by the order they appear in, so a single focus rule above is
   overridden by every variant below it that names a border colour, which is
   all of them. That left Connect, Create room, Send and Leave room - the four
   things the eye is meant to land on - with no focus ring at all, while the
   plain buttons around them had one. It is the same fault as the disabled
   QSlider: a rule that reads correctly and never fires.

   Naming the variant in each selector raises it to 0-2-1 and settles it by
   specificity rather than by order, so the block cannot be broken again by
   somebody appending a rule underneath.

   The colour changes per variant because none of them contrasts with every
   background a button can wear. The accent reads against the pale surfaces,
   so the plain and toggled buttons keep it. It disappears on a filled accent
   button, which takes the window colour instead. The danger button is the
   awkward one: its background swings from a near black tint at rest to bright
   red under the pointer, at opposite ends of the scale, and no single colour
   clears 3:1 against both - so it gets two rules, and the hovered one wins on
   its own account by carrying one more pseudo-state than the other. Every pair
   below was measured against WCAG 1.4.11 in both schemes: the worst case is
   3.6:1, on the plain button in the dark scheme, and most are far above it.

   Two pixels rather than one, with the pixel taken back out of the padding.
   The border is inside the box, so widening it alone would move the label and
   change the button's size hint at the moment it took focus. */
QPushButton:focus {
  border: 2px solid @{accent};
  padding: 7px 17px;
}
QPushButton[accent="true"]:focus {
  border: 2px solid @{window};
  padding: 7px 17px;
}
QPushButton[danger="true"]:focus {
  border: 2px solid @{text};
  padding: 7px 17px;
}
QPushButton[danger="true"]:focus:hover {
  border: 2px solid @{canvas};
  padding: 7px 17px;
}
QPushButton[toggle="accent"]:checked:focus,
QPushButton[toggle="warn"]:checked:focus {
  border: 2px solid @{accent};
  padding: 7px 17px;
}

/* --- fields -------------------------------------------------------------- */

QLineEdit, QComboBox, QSpinBox, QAbstractSpinBox {
  background: @{surface};
  color: @{text};
  border: 1px solid @{border};
  border-radius: @{control_radius}px;
  padding: 7px 12px;
  selection-background-color: @{accent};
  selection-color: @{on_accent};
}
QLineEdit:hover, QComboBox:hover, QAbstractSpinBox:hover {
  border-color: @{border_strong};
}
QLineEdit:focus, QComboBox:focus, QAbstractSpinBox:focus {
  border-color: @{accent};
}
QLineEdit:disabled, QComboBox:disabled, QAbstractSpinBox:disabled {
  background: @{window};
  color: @{muted};
}

/* Left alone, Fusion draws the drop-down and the two spin buttons as a raised
   panel with a divider, and it lands square on top of the rounded right-hand
   corner of the frame. Declaring the sub-controls with no border and no
   background takes the panel away.

   The arrow then has to be supplied here: a stylesheet cannot draw a triangle
   out of transparent borders the way a browser can, and once a sub-control has
   a rule of its own the style stops drawing its indicator. So it is an image,
   PNG at three densities. Not SVG - that needs the qsvg image plugin, and the
   release job prunes plugins whose framework it did not deploy, so an arrow
   drawn as SVG is an arrow that vanishes in the packaged build and nowhere
   else. See assets/ui/make_arrows.py for the shape. */
QComboBox::drop-down {
  subcontrol-origin: padding;
  subcontrol-position: center right;
  background: transparent;
  border: none;
  width: 28px;
}
QComboBox::down-arrow {
  image: url(:/ui/chevron-down.png);
  width: 10px;
  height: 6px;
}

QAbstractSpinBox::up-button, QAbstractSpinBox::down-button {
  subcontrol-origin: border;
  background: transparent;
  border: none;
  width: 24px;
  height: 13px;
}
QAbstractSpinBox::up-button {
  subcontrol-position: top right;
  margin: 3px 5px 0 0;
}
QAbstractSpinBox::down-button {
  subcontrol-position: bottom right;
  margin: 0 5px 3px 0;
}
QAbstractSpinBox::up-arrow {
  image: url(:/ui/chevron-up.png);
  width: 10px;
  height: 6px;
}
QAbstractSpinBox::down-arrow {
  image: url(:/ui/chevron-down.png);
  width: 10px;
  height: 6px;
}

/* The popup is a separate window and inherits none of the frame above. */
QComboBox QAbstractItemView {
  background: @{surface};
  color: @{text};
  border: 1px solid @{border};
  border-radius: @{control_radius}px;
  padding: 4px;
  outline: none;
  selection-background-color: @{accent};
  selection-color: @{on_accent};
}

/* --- lists --------------------------------------------------------------- */

QListWidget, QListView, QTreeView, QTableWidget, QTableView, QTextEdit, QPlainTextEdit {
  background: @{surface};
  color: @{text};
  border: 1px solid @{border};
  border-radius: @{control_radius}px;
  padding: 4px;
  outline: none;
}
QListWidget::item, QListView::item {
  border-radius: 8px;
  padding: 6px 8px;
}
QListWidget::item:hover, QListView::item:hover {
  background: @{surface_hover};
}
QListWidget::item:selected, QListView::item:selected {
  background: @{accent_soft};
  color: @{text};
}

QHeaderView::section {
  background: @{window};
  color: @{muted};
  border: none;
  border-bottom: 1px solid @{border};
  padding: 8px 10px;
  font-weight: 600;
}
QTableWidget, QTableView {
  gridline-color: @{border};
}
QTableWidget::item:selected, QTableView::item:selected {
  background: @{accent_soft};
  color: @{text};
}

/* --- tabs ---------------------------------------------------------------- */

/* A bare QTabBar on a line with other things, not a QTabWidget: the pages sit
   under it with no frame, so there is no pane to draw and no base line under
   the tabs. Each tab is a chip, and the selected one wears the accent wash. */
QTabBar {
  background: transparent;
}
QTabBar::tab {
  background: transparent;
  color: @{muted};
  border: 1px solid transparent;
  border-radius: @{control_radius}px;
  padding: 8px 16px;
  margin-right: 4px;
  font-weight: 600;
}
QTabBar::tab:hover {
  color: @{text};
  background: @{surface_hover};
}
QTabBar::tab:selected {
  background: @{accent_soft};
  border-color: @{accent};
  color: @{accent};
}

/* --- meters and sliders -------------------------------------------------- */

QProgressBar {
  background: @{surface_pressed};
  border: none;
  border-radius: 4px;
}
QProgressBar::chunk {
  background: @{muted};
  border-radius: 4px;
}
/* The local microphone meter while somebody is actually speaking into it. */
QProgressBar[speaking="true"]::chunk {
  background: @{accent};
}

QSlider::groove:horizontal {
  background: @{surface_pressed};
  height: 6px;
  border-radius: 3px;
}
QSlider::sub-page:horizontal {
  background: @{accent};
  height: 6px;
  border-radius: 3px;
}
/* The vertical margin is what gives the handle its height: Qt grows it out of
   the 6 pixel groove, half of the difference on each side. -4 against a 14
   pixel width is the circle the radius below assumes; a bigger margin makes an
   ellipse, and one bigger than the row makes an ellipse the widget clips. */
QSlider::handle:horizontal {
  background: @{surface};
  border: 2px solid @{accent};
  width: 14px;
  margin: -4px 0;
  border-radius: 7px;
}
QSlider::handle:horizontal:hover {
  background: @{accent_soft};
}
/* The state comes after the sub-control, never before it. Written the other
   way round Qt keeps the rule and forgets the sub-control, so `QSlider:disabled`
   painted `border_strong` over the whole widget: a slab behind a groove that
   stayed accent coloured, which is what the volume slider looked like with
   nobody selected. */
QSlider::sub-page:horizontal:disabled {
  background: @{border_strong};
}
QSlider::handle:horizontal:disabled {
  background: @{surface};
  border-color: @{border_strong};
}

/* --- scrollbars ---------------------------------------------------------- */

QScrollBar:vertical {
  background: transparent;
  width: 10px;
  margin: 2px;
}
QScrollBar:horizontal {
  background: transparent;
  height: 10px;
  margin: 2px;
}
QScrollBar::handle {
  background: @{border_strong};
  border-radius: 3px;
  min-height: 28px;
  min-width: 28px;
}
QScrollBar::handle:hover {
  background: @{muted};
}
QScrollBar::add-line, QScrollBar::sub-line {
  height: 0;
  width: 0;
}
QScrollBar::add-page, QScrollBar::sub-page {
  background: transparent;
}

)qss";

/// The rest of the sheet, from the chat down. Two literals rather than one
/// because MSVC refuses a single string literal over 16380 bytes (C2026), and
/// the sheet crossed that line; filled_stylesheet joins them.
constexpr const char* kStyleSheetContinued = R"qss(
/* --- chat ---------------------------------------------------------------- */

/* The button that opens the emoji picker, beside the message field. Quiet: a
   bordered box between a bordered field and a filled Send button was three
   shapes fighting for one row, and it was shorter than either of the other
   two. The border comes up under the pointer, the padding goes, and the glyph
   gets the size an emoji needs to read as a face rather than as a dot. The
   focus rule names the property for the reason the button block above does:
   at equal specificity the later rule wins, and QPushButton:focus is later. */
QPushButton[emoji="true"] {
  background: transparent;
  border: 1px solid transparent;
  border-radius: @{control_radius}px;
  padding: 0;
  font-size: 15pt;
}
QPushButton[emoji="true"]:hover {
  background: @{surface_hover};
  border-color: @{border_strong};
}
QPushButton[emoji="true"]:pressed {
  background: @{surface_pressed};
}
QPushButton[emoji="true"]:focus {
  border: 1px solid @{accent};
  padding: 0;
}
QPushButton[emoji="true"]:disabled {
  background: transparent;
  border-color: transparent;
}

/* The picker: a card, drawn the way a menu is, rather than forty bordered
   buttons on a bare window. The widget carries WA_StyledBackground, without
   which a plain QWidget ignores this rule, and WA_TranslucentBackground, so
   that what lies outside the rounded corners is nothing rather than a square
   of window colour. See ui/emoji_picker.cpp. */
QWidget#emojiPicker {
  background: @{surface};
  border: 1px solid @{border};
  border-radius: @{control_radius}px;
}
/* Each emoji in it: a flat square that lights up under the pointer or the
   focus, in the same wash a selected list row wears. */
QPushButton[emoji="cell"] {
  background: transparent;
  border: 1px solid transparent;
  border-radius: 8px;
  padding: 0;
  font-size: 17pt;
}
QPushButton[emoji="cell"]:hover,
QPushButton[emoji="cell"]:focus {
  background: @{accent_soft};
  border: 1px solid transparent;
  padding: 0;
}
QPushButton[emoji="cell"]:pressed {
  background: @{surface_pressed};
}

/* --- the administrator's console ----------------------------------------- */

/* The filter over the account table. The window colour rather than the
   surface, so it reads as the console's input line rather than as one more
   field; the padding is every other field's, so it stands as tall as the
   button beside it. */
QLineEdit[console="true"] {
  background: @{window};
}
QLineEdit[console="true"]:focus {
  border-color: @{accent};
}

/* The pane beside the table. The table's radius and not a card's: the two
   stand side by side at the same height, and corners that disagree between
   neighbours are the first thing the eye lands on. */
QFrame#accountPane {
  background: @{surface};
  border: 1px solid @{border};
  border-radius: @{control_radius}px;
}
/* The rules between its sections. */
QFrame#rule {
  background: @{border};
  border: none;
}
/* Small capitals over a section: the text is upper cased where it is set,
   because a stylesheet cannot ask for it. */
QLabel[eyebrow="true"] {
  color: @{muted};
  font-size: 11px;
  font-weight: 700;
}
QLabel[eyebrow="true"][accent="true"] {
  color: @{accent};
}

/* A button that is a line of text until the pointer is on it. The pane holds
   four side by side, and four bordered boxes would be a second toolbar. The
   focus rule names the property for the reason the button block does. */
QPushButton[quiet="accent"], QPushButton[quiet="danger"] {
  background: transparent;
  border: 1px solid transparent;
  padding: 5px 8px;
  text-align: left;
}
QPushButton[quiet="accent"] {
  color: @{accent};
}
QPushButton[quiet="danger"] {
  color: @{danger};
}
QPushButton[quiet="accent"]:hover {
  background: @{accent_soft};
  border-color: transparent;
}
QPushButton[quiet="danger"]:hover {
  background: @{danger_soft};
  border-color: transparent;
}
QPushButton[quiet="accent"]:focus, QPushButton[quiet="danger"]:focus {
  border: 1px solid @{accent};
  padding: 5px 8px;
}

/* --- chrome -------------------------------------------------------------- */

/* The padding is not decoration: without it the status text starts in the
   very corner of the window, touching two edges at once. */
QStatusBar {
  background: transparent;
  color: @{muted};
  padding: 2px 8px;
}
QStatusBar QLabel {
  color: @{muted};
  padding: 0 4px;
}
QStatusBar::item {
  border: none;
}

QMenu {
  background: @{surface};
  color: @{text};
  border: 1px solid @{border};
  border-radius: @{control_radius}px;
  padding: 6px;
}
QMenu::item {
  border-radius: 7px;
  padding: 7px 22px 7px 14px;
}
QMenu::item:selected {
  background: @{accent_soft};
  color: @{text};
}
QMenu::separator {
  height: 1px;
  background: @{border};
  margin: 5px 8px;
}
)qss";

/// The third part: the type scale as the stylesheet sees it, and the shapes of
/// the reusable components in ui/components. A literal of its own for the
/// reason the second one is.
constexpr const char* kStyleSheetComponents = R"qss(
/* --- type ---------------------------------------------------------------- */

/* What a label is for, said once where it is made - theme::set_role - rather
   than a font built by hand beside every label. The sizes are the steps of
   theme::TextRole. */
QLabel[role="caption"] {
  font-size: @{font_caption}pt;
  color: @{muted};
}
QLabel[role="title"] {
  font-size: @{font_title}pt;
  font-weight: 600;
}
QLabel[role="heading"] {
  font-size: @{font_heading}pt;
  font-weight: 600;
}
QLabel[role="display"] {
  font-size: @{font_display}pt;
  font-weight: 600;
}

/* A group box that stands under a tab or a heading of its own and so has no
   title: the margin the title would have stood in goes with it. */
QGroupBox[plain="true"] {
  margin-top: 0;
}

/* --- check boxes --------------------------------------------------------- */

/* Fusion's own box is a bevelled square in the palette's button colour, which
   beside a rounded field looks like a control from a different program. The
   tick is an image for the reason the chevrons are: see QComboBox above. */
QCheckBox {
  color: @{text};
  spacing: 8px;
}
QCheckBox:disabled {
  color: @{muted};
}
QCheckBox::indicator {
  width: 16px;
  height: 16px;
  border: 1px solid @{border_strong};
  border-radius: 5px;
  background: @{surface};
}
QCheckBox::indicator:hover {
  border-color: @{accent};
}
QCheckBox::indicator:checked {
  background: @{accent};
  border-color: @{accent};
  image: url(:/ui/check.png);
}
QCheckBox::indicator:disabled {
  background: @{window};
  border-color: @{border};
}
QCheckBox::indicator:checked:disabled {
  background: @{border_strong};
  border-color: @{border_strong};
}
QCheckBox:focus {
  outline: none;
}
QCheckBox::indicator:focus {
  border: 2px solid @{accent};
}

/* --- cards --------------------------------------------------------------- */

/* A plain panel with the surface colour and a hairline: what a QGroupBox is
   without its title. Card, StatCard and the panels of the room are this. */
QFrame[card="true"] {
  background: @{surface};
  border: 1px solid @{border};
  border-radius: @{card_radius}px;
}
QFrame[card="true"][interactive="true"]:hover {
  border-color: @{border_strong};
}

/* --- table rows ---------------------------------------------------------- */

/* Rows rather than cells: ui::set_up_table turns the grid off, and this is
   the line under each row that replaces it. No line between the columns -
   the header and the alignment are enough to say where one ends. */
QTableWidget::item, QTableView::item {
  border: none;
  border-bottom: 1px solid @{border};
  padding: 0 10px;
}
QTableWidget::item:hover, QTableView::item:hover {
  background: @{surface_hover};
}
QHeaderView {
  background: transparent;
}

/* --- inside a card ------------------------------------------------------- */

/* A list that lives on a card of its own - the people and the conversation
   in the room's side panel - is on that card's paper already. Its own border
   and background would be a box inside a box. */
QListWidget[flat="true"], QListView[flat="true"] {
  background: transparent;
  border: none;
  border-radius: 0;
  padding: 4px;
}

/* A page that scrolls: the cards of the lobby. The area is a window onto the
   page and has no edge of its own. */
QScrollArea {
  background: transparent;
  border: none;
}
QScrollArea > QWidget > QWidget {
  background: transparent;
}

/* A room's code, as a button that copies it: the size of a chip rather than
   of a button, and in the weight of a fact rather than of an action. The
   focus rule names the property for the reason the button block does. */
QPushButton[code="true"] {
  padding: 3px 10px;
  border-radius: @{chip_radius}px;
  color: @{muted};
  font-weight: 400;
}
QPushButton[code="true"]:hover {
  color: @{text};
}
QPushButton[code="true"]:focus {
  border: 2px solid @{accent};
  padding: 2px 9px;
}

/* --- search -------------------------------------------------------------- */

/* The magnifying glass is painted by SearchInput into the left padding. */
QLineEdit[search="true"] {
  padding-left: 34px;
}

/* --- chips --------------------------------------------------------------- */

/* A short fact beside a title: a room code, "sharing", "full". Not a control,
   so nothing happens under the pointer. */
QLabel[chip="true"] {
  background: @{surface_hover};
  border: 1px solid @{border};
  border-radius: @{chip_radius}px;
  padding: 3px 8px;
  color: @{muted};
  font-size: @{font_caption}pt;
  font-weight: 600;
}
QLabel[chip="accent"] {
  background: @{accent_soft};
  border: 1px solid transparent;
  border-radius: @{chip_radius}px;
  padding: 3px 8px;
  color: @{accent_text};
  font-size: @{font_caption}pt;
  font-weight: 600;
}
QLabel[chip="warn"] {
  background: @{warn_soft};
  border: 1px solid transparent;
  border-radius: @{chip_radius}px;
  padding: 3px 8px;
  color: @{warn};
  font-size: @{font_caption}pt;
  font-weight: 600;
}
)qss";

QString filled_stylesheet(const Colors& colors) {
  QString sheet = QString::fromUtf8(kStyleSheet) + QString::fromUtf8(kStyleSheetContinued) +
                  QString::fromUtf8(kStyleSheetComponents);

  const std::array<std::pair<const char*, QColor>, 26> tokens{{
      {"window", colors.window},
      {"chrome", colors.chrome},
      {"surface", colors.surface},
      {"surface_hover", colors.surface_hover},
      {"surface_pressed", colors.surface_pressed},
      {"surface_raised", colors.surface_raised},
      {"accent_text", colors.accent_text},
      {"success_soft", colors.success_soft},
      {"info", colors.info},
      {"info_soft", colors.info_soft},
      {"border", colors.border},
      {"border_strong", colors.border_strong},
      {"text", colors.text},
      {"muted", colors.muted},
      {"accent", colors.accent},
      {"accent_hover", colors.accent_hover},
      {"accent_pressed", colors.accent_pressed},
      {"accent_soft", colors.accent_soft},
      {"on_accent", colors.on_accent},
      {"danger", colors.danger},
      {"danger_soft", colors.danger_soft},
      {"warn", colors.warn},
      {"warn_soft", colors.warn_soft},
      {"success", colors.success},
      {"canvas", colors.canvas},
      {"canvas_text", colors.canvas_text},
  }};
  for (const auto& [name, colour] : tokens) {
    sheet.replace(QStringLiteral("@{%1}").arg(QLatin1StringView(name)),
                  colour.name(QColor::HexRgb));
  }

  sheet.replace(QStringLiteral("@{card_radius}"), QString::number(kCardRadius));
  sheet.replace(QStringLiteral("@{control_radius}"), QString::number(kControlRadius));
  sheet.replace(QStringLiteral("@{chip_radius}"), QString::number(kChipRadius));

  // The type scale, in points so that it follows the desktop's text scaling
  // the way the application font does. One decimal: Qt's stylesheet parser
  // takes a fractional point size, and 14.4 rounded to 14 is a step of the
  // scale quietly moved.
  const auto points = [](TextRole role) {
    return QString::number(font(role).pointSizeF(), 'f', 1);
  };
  sheet.replace(QStringLiteral("@{font_caption}"), points(TextRole::Caption));
  sheet.replace(QStringLiteral("@{font_body}"), points(TextRole::Body));
  sheet.replace(QStringLiteral("@{font_title}"), points(TextRole::Title));
  sheet.replace(QStringLiteral("@{font_heading}"), points(TextRole::Heading));
  sheet.replace(QStringLiteral("@{font_display}"), points(TextRole::Display));
  return sheet;
}

/// The same colours as a QPalette.
///
/// Not a duplicate of the stylesheet: the stylesheet only reaches the widgets
/// it names, and the palette is what everything else is drawn from, including
/// the parts of a widget a stylesheet cannot address. Skipping it is how a
/// styled application ends up with one dialog in the desktop's colours.
QPalette palette_for(const Colors& colors) {
  QPalette palette;
  palette.setColor(QPalette::Window, colors.window);
  palette.setColor(QPalette::WindowText, colors.text);
  palette.setColor(QPalette::Base, colors.surface);
  palette.setColor(QPalette::AlternateBase, colors.window);
  palette.setColor(QPalette::Text, colors.text);
  palette.setColor(QPalette::PlaceholderText, colors.muted);
  palette.setColor(QPalette::Button, colors.surface);
  palette.setColor(QPalette::ButtonText, colors.text);
  palette.setColor(QPalette::Highlight, colors.accent);
  palette.setColor(QPalette::HighlightedText, colors.on_accent);
  palette.setColor(QPalette::ToolTipBase, colors.surface);
  palette.setColor(QPalette::ToolTipText, colors.text);
  palette.setColor(QPalette::Link, colors.accent);
  palette.setColor(QPalette::LinkVisited, colors.accent_pressed);

  // Mid and Dark are not decoration here. Existing code asks for palette(mid)
  // for secondary text, and ScreenView fills itself with palette().dark() and
  // writes on it in BrightText. Leaving those three at Qt's defaults would put
  // the video canvas and the placeholder on it outside this file's reach.
  palette.setColor(QPalette::Mid, colors.muted);
  palette.setColor(QPalette::Dark, colors.canvas);
  palette.setColor(QPalette::BrightText, colors.canvas_text);
  palette.setColor(QPalette::Shadow, colors.border);
  palette.setColor(QPalette::Light, colors.surface_hover);
  palette.setColor(QPalette::Midlight, colors.border);

  palette.setColor(QPalette::Disabled, QPalette::Text, colors.muted);
  palette.setColor(QPalette::Disabled, QPalette::ButtonText, colors.muted);
  palette.setColor(QPalette::Disabled, QPalette::WindowText, colors.muted);
  palette.setColor(QPalette::Disabled, QPalette::Highlight, colors.border_strong);
  return palette;
}

void install(QApplication& application) {
  dark_in_force() = wants_dark(chosen_mode());
  current() = dark_in_force() ? dark() : light();
  QApplication::setPalette(palette_for(current()));
  application.setStyleSheet(filled_stylesheet(current()));
}

/// Installs the colours again and makes everything already on screen wear
/// them. The one path for both ways the colours change while the program is
/// running: the desktop switching scheme, and somebody choosing in Settings.
void reinstall() {
  auto* application = qobject_cast<QApplication*>(QCoreApplication::instance());
  if (application == nullptr) {
    return;
  }
  install(*application);

  // A stylesheet that has already been resolved does not re-resolve on its own
  // for every widget, and the video canvas paints from the palette rather than
  // from the sheet.
  const QWidgetList widgets = QApplication::allWidgets();
  for (QWidget* widget : widgets) {
    widget->style()->unpolish(widget);
    widget->style()->polish(widget);
    widget->update();
    if (widget->isWindow()) {
      platform_set_title_bar(widget, dark_in_force());
    }
  }
  emit notifier() -> changed();
}

/// The body size the scale is built on, in points. One more than the nine
/// Windows hands out: nine points is twelve pixels, which is a size for a
/// status bar and was being used for everything.
constexpr qreal kBodyPoints = 10.0;
/// Each step of the scale is this many times the one below it.
constexpr qreal kScaleRatio = 1.2;

}  // namespace

const Colors& colors() {
  return current();
}

std::optional<Mode> parse_mode(std::string_view text) {
  if (text == "system") {
    return Mode::System;
  }
  if (text == "light") {
    return Mode::Light;
  }
  if (text == "dark") {
    return Mode::Dark;
  }
  return std::nullopt;
}

std::string_view to_string(Mode mode) {
  switch (mode) {
    case Mode::Light:
      return "light";
    case Mode::Dark:
      return "dark";
    case Mode::System:
      break;
  }
  return "system";
}

Mode mode() {
  return chosen_mode();
}

void set_mode(Mode mode) {
  if (chosen_mode() == mode) {
    return;
  }
  chosen_mode() = mode;
  // Only when the answer actually moved. Dark to System on a dark desktop is a
  // change of instruction and not of colour, and re-polishing every widget for
  // it would be a flicker about nothing.
  if (wants_dark(mode) != dark_in_force()) {
    reinstall();
  }
}

bool is_dark() {
  return dark_in_force();
}

QFont font(TextRole role) {
  QFont font = QApplication::font();
  // Rounded to the half point. The steps are 8.3, 10, 12, 14.4 and 17.3, and
  // a font engine asked for 14.4 hints it as it pleases; a half is as fine as
  // the difference stays visible.
  const auto step = [](int exponent) {
    return std::round(kBodyPoints * std::pow(kScaleRatio, exponent) * 2.0) / 2.0;
  };
  switch (role) {
    case TextRole::Caption:
      // Not step(-1), which is 8.5: under nine points Segoe UI stops being
      // read and starts being deciphered.
      font.setPointSizeF(9.0);
      break;
    case TextRole::Body:
      font.setPointSizeF(step(0));
      break;
    case TextRole::Label:
      font.setPointSizeF(step(0));
      font.setWeight(QFont::DemiBold);
      break;
    case TextRole::Title:
      font.setPointSizeF(step(1));
      font.setWeight(QFont::DemiBold);
      break;
    case TextRole::Heading:
      font.setPointSizeF(step(2));
      font.setWeight(QFont::DemiBold);
      break;
    case TextRole::Display:
      font.setPointSizeF(step(3) + 3.0);
      font.setWeight(QFont::DemiBold);
      break;
  }
  return font;
}

void set_role(QWidget* label, TextRole role) {
  const char* name = "";
  switch (role) {
    case TextRole::Caption:
      name = "caption";
      break;
    case TextRole::Title:
      name = "title";
      break;
    case TextRole::Heading:
      name = "heading";
      break;
    case TextRole::Display:
      name = "display";
      break;
    case TextRole::Body:
    case TextRole::Label:
      // The application font already is the body, and a label's weight comes
      // from the control it names.
      break;
  }
  label->setProperty("role", QString::fromLatin1(name));
  // A property is not a repaint: without this a role set after the widget was
  // first shown changes nothing until something else re-polishes it.
  label->style()->unpolish(label);
  label->style()->polish(label);
}

QColor mix(const QColor& from, const QColor& to, qreal amount) {
  const qreal weight = std::clamp(amount, 0.0, 1.0);
  const auto channel = [weight](qreal first, qreal second) {
    return first + ((second - first) * weight);
  };
  return QColor::fromRgbF(static_cast<float>(channel(from.redF(), to.redF())),
                          static_cast<float>(channel(from.greenF(), to.greenF())),
                          static_cast<float>(channel(from.blueF(), to.blueF())),
                          static_cast<float>(channel(from.alphaF(), to.alphaF())));
}

QColor ink_on(const QColor& background) {
  // WCAG relative luminance, and the two contrast ratios it gives against a
  // dark ink and against white. Whichever is larger is the one to write in.
  const auto linear = [](qreal channel) {
    return channel <= 0.03928 ? channel / 12.92 : std::pow((channel + 0.055) / 1.055, 2.4);
  };
  const qreal luminance = (0.2126 * linear(background.redF())) +
                          (0.7152 * linear(background.greenF())) +
                          (0.0722 * linear(background.blueF()));
  const QColor dark_ink(0x16, 0x10, 0x14);
  const qreal on_white = 1.05 / (luminance + 0.05);
  const qreal on_dark = (luminance + 0.05) / 0.06;
  return on_dark > on_white ? dark_ink : QColor(Qt::white);
}

namespace motion {

bool enabled() {
  return animations_enabled();
}

void set_enabled(bool enabled) {
  animations_enabled() = enabled;
}

int duration(int ms) {
  return animations_enabled() ? ms : 0;
}

}  // namespace motion

Notifier* notifier() {
  // Parented to the application, so it goes when the application does and
  // not at static destruction, after Qt itself has been torn down.
  static QPointer<Notifier> instance;
  if (instance.isNull()) {
    instance = new Notifier(QCoreApplication::instance());
  }
  return instance;
}

bool Notifier::eventFilter(QObject* watched, QEvent* event) {
  // Every window as it is shown, the dialogs and the menus included: each is
  // a window of its own to the system, created after apply() ran, and each
  // would otherwise come up with the desktop's title bar over this program's
  // colours.
  if (event->type() == QEvent::Show) {
    if (auto* widget = qobject_cast<QWidget*>(watched); widget != nullptr && widget->isWindow()) {
      platform_set_title_bar(widget, dark_in_force());
    }
  }
  return QObject::eventFilter(watched, event);
}

void paint_shadow(QPainter& painter, const QRectF& box, qreal radius) {
  // Rings of the shadow colour, each a pixel further out and fainter than the
  // last, a little lower than the box so the light reads as coming from
  // above. Eight rounded rectangles: nothing next to a blur, and close enough
  // to one at the size a shadow is looked at.
  constexpr int kRings = 8;
  constexpr qreal kDrop = 3.0;
  const QColor base = current().shadow;
  painter.save();
  painter.setRenderHint(QPainter::Antialiasing, true);
  painter.setPen(Qt::NoPen);
  for (int ring = kRings; ring >= 1; --ring) {
    QColor colour = base;
    const qreal fade = 1.0 - (static_cast<qreal>(ring) / (kRings + 1));
    colour.setAlphaF(static_cast<float>(base.alphaF() * fade * fade * 0.5));
    painter.setBrush(colour);
    const QRectF grown = box.adjusted(-ring, -ring + kDrop, ring, ring + kDrop);
    painter.drawRoundedRect(grown, radius + ring, radius + ring);
  }
  painter.restore();
}

QString stylesheet() {
  return filled_stylesheet(current());
}

void apply(QApplication& application, Mode mode) {
  // Fusion, and not the platform's own style.
  //
  // The native styles on macOS and Windows draw their controls themselves and
  // ignore most of what a stylesheet asks for, so a rounded button would be
  // rounded on one of the three platforms this ships to and square on the
  // others. Fusion draws everything from the palette and the stylesheet, which
  // is the only way the same corner radius reaches all three.
  QApplication::setStyle(QStringLiteral("Fusion"));

  // The family the desktop chose, at the size the scale starts from. Before
  // the stylesheet, which reads the scale back out of font().
  QFont base = QApplication::font();
  base.setPointSizeF(kBodyPoints);
  QApplication::setFont(base);

  chosen_mode() = mode;
  install(application);
  application.installEventFilter(notifier());

  // Somebody switching their desktop to dark in the middle of a call should
  // not be looking at a white window until they restart. Only while the
  // desktop is what was asked to decide: a mode held in Settings stays held.
  QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &application,
                   [](Qt::ColorScheme /*scheme*/) {
                     if (chosen_mode() == Mode::System) {
                       reinstall();
                     }
                   });
}

}  // namespace dv::ui::theme
