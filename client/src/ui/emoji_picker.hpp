#pragma once

#include <vector>

#include <QString>
#include <QStringList>
#include <QWidget>

class QKeyEvent;
class QPushButton;

namespace dv::ui {

/// A short grid of emoji that opens above the chat's message field.
///
/// Forty of them, chosen and not generated. The system picker already does the
/// whole of Unicode with a search box, skin tones and a list of recents, and
/// it will know about emoji that do not exist yet; competing with it would be
/// a worse copy that goes out of date. What it cannot do is put the handful
/// somebody reaches for during a call one click away, and that is what this
/// is. Win+. on Windows and Ctrl+Cmd+Space on macOS open the system one in the
/// same field, so nothing here is a ceiling on what can be sent.
///
/// A window of its own with Qt::Popup, and not a QMenu holding one widget. The
/// QMenu route looks right - it draws the grid, it closes on a click outside,
/// it needs no lifetime handling - and it never delivers a click to a single
/// one of those buttons on macOS: the menu keeps the mouse grab for itself and
/// the presses die inside it. Qt::Popup gives the same behaviour a picker
/// needs, closing on a click outside and on Escape, and what is inside it
/// stays an ordinary widget that sees ordinary events.
///
/// It draws itself as a card, the way a menu does, rather than as forty
/// bordered buttons on a bare window; it fades and lifts into place instead of
/// appearing; and the arrow keys walk the grid, so that it is usable from the
/// keyboard the message field was being typed into. The card and the cells
/// take their look from ui/theme.cpp, under `emojiPicker` and `emoji`.
///
/// Deleted when closed, and every way out - a pick, Escape, a click outside -
/// ends in close(). Whoever opens one connects to `picked` and forgets it.
// A QObject cannot be copied or moved: its identity is the thing Qt tracks.
// NOLINTNEXTLINE(cppcoreguidelines-special-member-functions)
class EmojiPicker : public QWidget {
  Q_OBJECT

 public:
  /// Builds the grid, hidden. `anchor` is the parent, which is what keeps the
  /// picker on the same screen and in the same colour scheme as the field.
  explicit EmojiPicker(QWidget* anchor);

  /// Shows the picker above `anchor` with its right edge on the anchor's, and
  /// fades it in. Above rather than below, because the field it serves sits at
  /// the bottom of the window and a picker dropped downwards would open off
  /// the screen.
  void open_above(const QWidget* anchor);

  /// What the grid offers, in the order it shows them.
  [[nodiscard]] static const QStringList& quick_emoji();

  /// How many emoji go on a row. Eight of them at `kCellSize` plus the
  /// margins is about 310 wide, which fits under a sidebar that is 240 pixels
  /// at its narrowest without hanging off the side of the window.
  static constexpr int kColumns = 8;
  /// The square each emoji sits in, in pixels. Big enough for the glyph at the
  /// size the stylesheet gives it, with room around it for the highlight.
  static constexpr int kCellSize = 36;

 signals:
  /// One emoji was chosen. The picker closes itself right after.
  void picked(const QString& emoji);

 protected:
  /// The arrow keys move between cells, Return picks the focused one, and
  /// Escape closes, which the base class already does for a popup.
  void keyPressEvent(QKeyEvent* event) override;

 private:
  /// Moves the focus `by` cells from the focused one, staying in the grid.
  void move_focus(int by);

  /// The cells in reading order, which is also `quick_emoji()`'s order.
  std::vector<QPushButton*> cells_;
};

}  // namespace dv::ui
