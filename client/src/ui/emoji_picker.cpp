#include "ui/emoji_picker.hpp"

#include <algorithm>
#include <cstddef>
#include <iterator>

#include <QAbstractAnimation>
#include <QEasingCurve>
#include <QGridLayout>
#include <QKeyEvent>
#include <QParallelAnimationGroup>
#include <QPoint>
#include <QPropertyAnimation>
#include <QPushButton>
#include <QWidget>

namespace dv::ui {
namespace {

/// How long the picker takes to fade and lift into place, in milliseconds.
///
/// Short enough to be over before anybody could want to click, long enough
/// that the card arrives rather than appears. The same length as the page
/// change in MainWindow, so the two motions in this interface match.
constexpr int kOpenMs = 140;

/// How far below its final place the picker starts, in pixels. A lift this
/// small reads as motion without reading as travel.
constexpr int kLiftPx = 8;

/// The gap between the top of the button and the bottom of the card, so the
/// card does not sit on the button's border.
constexpr int kGapPx = 6;

/// The space between the outermost cells and the card's edge.
constexpr int kMarginPx = 6;

/// The space between cells.
constexpr int kSpacingPx = 2;

}  // namespace

const QStringList& EmojiPicker::quick_emoji() {
  // A function local static: a QStringList has a non-trivial constructor and
  // cannot be constexpr, which is the case client/src/ui/.clang-tidy's naming
  // rules call out.
  // clang-format off
  //
  // Laid out eight to a line because that is how many go on a row of the
  // picker, so the source has the shape of the thing on screen, and written as
  // the characters themselves so that what is on offer can be read here rather
  // than decoded. clang-format measures a line in bytes and an emoji is four
  // of them, so left alone it reflows this to one per line and the grid goes.
  static const QStringList kQuickEmoji = {
      // Answers, which is most of what a chat during a call is for.
      QStringLiteral("👍"), QStringLiteral("👎"), QStringLiteral("👌"), QStringLiteral("🙌"),
      QStringLiteral("👏"), QStringLiteral("🙏"), QStringLiteral("💪"), QStringLiteral("🤝"),
      // Faces.
      QStringLiteral("😀"), QStringLiteral("😄"), QStringLiteral("😅"), QStringLiteral("😂"),
      QStringLiteral("🙂"), QStringLiteral("😉"), QStringLiteral("😍"), QStringLiteral("🤔"),
      QStringLiteral("😐"), QStringLiteral("😴"), QStringLiteral("😭"), QStringLiteral("😱"),
      QStringLiteral("😡"), QStringLiteral("🤯"), QStringLiteral("🤦"), QStringLiteral("🤷"),
      // How it went.
      QStringLiteral("🎉"), QStringLiteral("🎊"), QStringLiteral("🔥"), QStringLiteral("⭐"),
      QStringLiteral("✨"), QStringLiteral("💡"), QStringLiteral("✅"), QStringLiteral("❌"),
      // The work itself.
      QStringLiteral("🚀"), QStringLiteral("🐛"), QStringLiteral("🔧"), QStringLiteral("📌"),
      QStringLiteral("⏰"), QStringLiteral("☕"), QStringLiteral("👀"), QStringLiteral("❤️"),
  };
  // clang-format on

  return kQuickEmoji;
}

EmojiPicker::EmojiPicker(QWidget* anchor) : QWidget(anchor, Qt::Popup) {
  setAttribute(Qt::WA_DeleteOnClose);
  // The name is what the stylesheet's `QWidget#emojiPicker` rule matches, and
  // the attribute is what makes a plain QWidget paint that rule at all: left
  // alone, a QWidget draws no background of its own and the rule is ignored.
  setObjectName(QStringLiteral("emojiPicker"));
  setAttribute(Qt::WA_StyledBackground);
  // The card has rounded corners, and what lies outside them has to be
  // nothing rather than a square of window colour. Without this the popup is
  // an opaque rectangle with a rounded card drawn on it, and the corners show.
  setAttribute(Qt::WA_TranslucentBackground);
  // The popup takes the keyboard while it is up, and the arrows and Escape are
  // handled here before any cell has been reached with Tab.
  setFocusPolicy(Qt::StrongFocus);

  auto* grid = new QGridLayout(this);
  grid->setContentsMargins(kMarginPx, kMarginPx, kMarginPx, kMarginPx);
  grid->setSpacing(kSpacingPx);

  int row = 0;
  int column = 0;
  cells_.reserve(static_cast<std::size_t>(quick_emoji().size()));
  for (const QString& emoji : quick_emoji()) {
    auto* cell = new QPushButton(emoji, this);
    // "cell", which the stylesheet tells apart from the button that opened the
    // picker: the same glyphs, but flat squares that light up under the
    // pointer rather than bordered buttons.
    cell->setProperty("emoji", QStringLiteral("cell"));
    cell->setFixedSize(kCellSize, kCellSize);
    cell->setCursor(Qt::PointingHandCursor);
    // Reached with the arrows and with Tab, so a keyboard user gets there; a
    // click focuses it too, and the picker is gone before that shows.
    cell->setFocusPolicy(Qt::StrongFocus);
    connect(cell, &QPushButton::clicked, this, [this, emoji] {
      emit picked(emoji);
      // Closed after one pick. Somebody who wants a second one presses the
      // button again, which is a click either way, and a picker that stays
      // open over the field it is typing into is one that has to be dismissed.
      close();
    });
    grid->addWidget(cell, row, column);
    cells_.push_back(cell);
    if (++column == kColumns) {
      column = 0;
      ++row;
    }
  }
}

void EmojiPicker::open_above(const QWidget* anchor) {
  adjustSize();
  const QPoint place = anchor->mapToGlobal(QPoint(anchor->width() - width(), -height() - kGapPx));
  const QPoint from = place + QPoint(0, kLiftPx);

  // Shown transparent and a little low, then faded and lifted into place. Both
  // properties are the window's own, so nothing here needs a graphics effect
  // on the children, and both are cheap: eight or nine frames of one window
  // moving.
  setWindowOpacity(0.0);
  move(from);
  show();
  setFocus();

  auto* fade = new QPropertyAnimation(this, "windowOpacity", this);
  fade->setDuration(kOpenMs);
  fade->setStartValue(0.0);
  fade->setEndValue(1.0);
  fade->setEasingCurve(QEasingCurve::OutCubic);

  auto* lift = new QPropertyAnimation(this, "pos", this);
  lift->setDuration(kOpenMs);
  lift->setStartValue(from);
  lift->setEndValue(place);
  lift->setEasingCurve(QEasingCurve::OutCubic);

  auto* motion = new QParallelAnimationGroup(this);
  motion->addAnimation(fade);
  motion->addAnimation(lift);
  motion->start(QAbstractAnimation::DeleteWhenStopped);
}

void EmojiPicker::keyPressEvent(QKeyEvent* event) {
  switch (event->key()) {
    case Qt::Key_Left:
      move_focus(-1);
      return;
    case Qt::Key_Right:
      move_focus(1);
      return;
    case Qt::Key_Up:
      move_focus(-kColumns);
      return;
    case Qt::Key_Down:
      move_focus(kColumns);
      return;
    case Qt::Key_Return:
    case Qt::Key_Enter:
      // A QPushButton answers Space on its own and leaves Return to whoever
      // owns it, which outside a dialog is nobody. Here it is the same as a
      // click on the focused cell.
      if (auto* cell = qobject_cast<QPushButton*>(focusWidget()); cell != nullptr) {
        cell->click();
        return;
      }
      break;
    default:
      break;
  }
  // Escape lands here, and QWidget closes a popup on it.
  QWidget::keyPressEvent(event);
}

void EmojiPicker::move_focus(int by) {
  if (cells_.empty()) {
    return;
  }
  const QWidget* const target = focusWidget();
  const auto focused =
      std::ranges::find_if(cells_, [target](const QPushButton* cell) { return cell == target; });
  // From nowhere, the first arrow lands on the first cell rather than one
  // step away from it: there is no "one step from nothing".
  const auto current =
      focused == cells_.end() ? -1 : static_cast<int>(std::distance(cells_.begin(), focused));
  const auto last = static_cast<int>(cells_.size()) - 1;
  const int next = current < 0 ? 0 : std::clamp(current + by, 0, last);
  cells_[static_cast<std::size_t>(next)]->setFocus(Qt::TabFocusReason);
}

}  // namespace dv::ui
