#include "ui/theme.hpp"

#include <array>

#include <QApplication>
#include <QGuiApplication>
#include <QPalette>
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
      .surface = QColor(0xFF, 0xFF, 0xFF),
      .surface_hover = QColor(0xF7, 0xF8, 0xFD),
      .surface_pressed = QColor(0xEC, 0xEE, 0xF8),
      .border = QColor(0xDF, 0xE1, 0xEC),
      .border_strong = QColor(0xC2, 0xC6, 0xDC),
      .text = QColor(0x1C, 0x1D, 0x29),
      .muted = QColor(0x6C, 0x70, 0x89),
      .accent = QColor(0x5B, 0x5B, 0xD6),
      .accent_hover = QColor(0x6B, 0x6B, 0xDE),
      .accent_pressed = QColor(0x4A, 0x4A, 0xC0),
      .accent_soft = QColor(0xE7, 0xE7, 0xFA),
      .on_accent = QColor(0xFF, 0xFF, 0xFF),
      .danger = QColor(0xD1, 0x3A, 0x3A),
      .danger_soft = QColor(0xFC, 0xEC, 0xEC),
      .warn = QColor(0xB5, 0x66, 0x0A),
      .warn_soft = QColor(0xFD, 0xF3, 0xE4),
      .success = QColor(0x2E, 0x7D, 0x32),
      .canvas = QColor(0x14, 0x15, 0x1C),
      .canvas_text = QColor(0x8B, 0x8F, 0xA6),
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
      .surface = QColor(0x1E, 0x1F, 0x2A),
      .surface_hover = QColor(0x26, 0x28, 0x35),
      .surface_pressed = QColor(0x2E, 0x30, 0x40),
      .border = QColor(0x2E, 0x30, 0x40),
      .border_strong = QColor(0x45, 0x48, 0x5F),
      .text = QColor(0xE8, 0xE9, 0xF0),
      .muted = QColor(0x99, 0x9D, 0xB4),
      .accent = QColor(0x7B, 0x7B, 0xEC),
      .accent_hover = QColor(0x8C, 0x8C, 0xF2),
      .accent_pressed = QColor(0x69, 0x69, 0xDB),
      .accent_soft = QColor(0x2A, 0x2B, 0x4B),
      .on_accent = QColor(0xFF, 0xFF, 0xFF),
      .danger = QColor(0xF1, 0x6B, 0x6B),
      .danger_soft = QColor(0x3A, 0x22, 0x25),
      .warn = QColor(0xEB, 0xA9, 0x50),
      .warn_soft = QColor(0x38, 0x2B, 0x1A),
      .success = QColor(0x6F, 0xCF, 0x7A),
      .canvas = QColor(0x0D, 0x0E, 0x14),
      .canvas_text = QColor(0x74, 0x78, 0x8E),
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
QPushButton:focus {
  border-color: @{accent};
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

QTabWidget::pane {
  background: @{surface};
  border: 1px solid @{border};
  border-radius: @{card_radius}px;
  top: -1px;
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

QString filled_stylesheet(const Colors& colors) {
  QString sheet = QString::fromUtf8(kStyleSheet);

  const std::array<std::pair<const char*, QColor>, 20> tokens{{
      {"window", colors.window},
      {"surface", colors.surface},
      {"surface_hover", colors.surface_hover},
      {"surface_pressed", colors.surface_pressed},
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
  current() = system_is_dark() ? dark() : light();
  QApplication::setPalette(palette_for(current()));
  application.setStyleSheet(filled_stylesheet(current()));
}

}  // namespace

const Colors& colors() {
  return current();
}

QString stylesheet() {
  return filled_stylesheet(current());
}

void apply(QApplication& application) {
  // Fusion, and not the platform's own style.
  //
  // The native styles on macOS and Windows draw their controls themselves and
  // ignore most of what a stylesheet asks for, so a rounded button would be
  // rounded on one of the three platforms this ships to and square on the
  // others. Fusion draws everything from the palette and the stylesheet, which
  // is the only way the same corner radius reaches all three.
  QApplication::setStyle(QStringLiteral("Fusion"));
  install(application);

  // Somebody switching their desktop to dark in the middle of a call should
  // not be looking at a white window until they restart.
  QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, &application,
                   [&application](Qt::ColorScheme /*scheme*/) {
                     install(application);
                     // A stylesheet that has already been resolved does not
                     // re-resolve on its own for every widget, and the video
                     // canvas paints from the palette rather than from the
                     // sheet.
                     const QWidgetList widgets = QApplication::allWidgets();
                     for (QWidget* widget : widgets) {
                       widget->style()->unpolish(widget);
                       widget->style()->polish(widget);
                       widget->update();
                     }
                   });
}

}  // namespace dv::ui::theme
