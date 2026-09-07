#include "ui/account_delegate.hpp"

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include <QApplication>
#include <QColor>
#include <QFont>
#include <QFontMetricsF>
#include <QModelIndex>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QSize>
#include <QString>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The padding the stylesheet gives a table cell, so that what is drawn here
/// starts where the style's own text would. See theme.cpp.
constexpr int kHorizontalPadding = 6;

/// The status dot, in device independent pixels.
constexpr qreal kDotSize = 8.0;

/// Between chips, and inside one.
constexpr qreal kChipGap = 4.0;
constexpr qreal kChipPadding = 5.0;

/// The chips and the role are set a little smaller than the row, so that they
/// read as marks on the line rather than as words in it.
constexpr qreal kSmallScale = 0.86;

/// A chip: the word on it and whether it is the one that locks somebody out.
/// The words are short on purpose - four chips have to fit beside a name -
/// and the full names are in the tooltip and the filter.
struct Chip {
  QString word;
  bool severe;
};

[[nodiscard]] std::vector<Chip> chips_for(int flags) {
  std::vector<Chip> chips;
  if ((flags & kAccountBanned) != 0) {
    chips.push_back(Chip{.word = QStringLiteral("banned"), .severe = true});
  }
  if ((flags & kAccountMuted) != 0) {
    chips.push_back(Chip{.word = QStringLiteral("mic"), .severe = false});
  }
  if ((flags & kAccountSilenced) != 0) {
    chips.push_back(Chip{.word = QStringLiteral("chat"), .severe = false});
  }
  if ((flags & kAccountScreenBlocked) != 0) {
    chips.push_back(Chip{.word = QStringLiteral("screen"), .severe = false});
  }
  return chips;
}

[[nodiscard]] QFont small_font(const QFont& font) {
  QFont small = font;
  small.setPointSizeF(font.pointSizeF() * kSmallScale);
  small.setBold(true);
  return small;
}

/// Paints the cell's background, selection and focus through the style with
/// the text taken out, which is what stops the style from drawing a plain
/// copy under whatever is drawn here.
void draw_ground(QPainter* painter, QStyleOptionViewItem& styled) {
  styled.text.clear();
  QStyle* style = styled.widget != nullptr ? styled.widget->style() : QApplication::style();
  style->drawControl(QStyle::CE_ItemViewItem, &styled, painter, styled.widget);
}

}  // namespace

AccountDelegate::AccountDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void AccountDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                            const QModelIndex& index) const {
  const auto kind = static_cast<AccountCell>(index.data(kAccountCellRole).toInt());
  if (kind == AccountCell::Plain) {
    QStyledItemDelegate::paint(painter, option, index);
    return;
  }

  QStyleOptionViewItem styled = option;
  initStyleOption(&styled, index);
  const QString text = styled.text;
  draw_ground(painter, styled);

  const theme::Colors& colours = theme::colors();
  const QRectF cell = QRectF(styled.rect).adjusted(kHorizontalPadding, 0, -kHorizontalPadding, 0);

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);

  switch (kind) {
    case AccountCell::Status: {
      const bool online = index.data(kAccountOnlineRole).toBool();
      const QRectF dot(styled.rect.center().x() - (kDotSize / 2),
                       styled.rect.center().y() - (kDotSize / 2), kDotSize, kDotSize);
      if (online) {
        painter->setPen(Qt::NoPen);
        painter->setBrush(colours.success);
      } else {
        painter->setPen(QPen(colours.muted, 1.5));
        painter->setBrush(Qt::NoBrush);
      }
      painter->drawEllipse(dot);
      break;
    }
    case AccountCell::Role: {
      // Small capitals by hand: a stylesheet cannot ask for them, and the
      // point of the treatment is that "admin" reads as a mark rather than a
      // word. The accent for an administrator, the muted grey for a user,
      // which is most rows: the exception is what should catch the eye.
      const bool admin = text.compare(QStringLiteral("admin"), Qt::CaseInsensitive) == 0;
      painter->setFont(small_font(styled.font));
      painter->setPen(admin ? colours.accent : colours.muted);
      painter->drawText(cell, Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine,
                        text.toUpper());
      break;
    }
    case AccountCell::Restrictions: {
      const QFont font = small_font(styled.font);
      const QFontMetricsF metrics(font);
      painter->setFont(font);
      const qreal height = std::floor(metrics.height()) + 2;
      qreal x = cell.left();
      for (const Chip& chip : chips_for(index.data(kAccountFlagsRole).toInt())) {
        const qreal width = metrics.horizontalAdvance(chip.word) + (2 * kChipPadding);
        if (x + width > cell.right()) {
          // Out of room: the rest are in the tooltip. Better than a chip cut
          // in half, which reads as a word nobody can finish.
          break;
        }
        const QRectF box(x, cell.center().y() - (height / 2), width, height);
        painter->setPen(Qt::NoPen);
        painter->setBrush(chip.severe ? colours.danger_soft : colours.warn_soft);
        painter->drawRoundedRect(box, 5, 5);
        painter->setPen(chip.severe ? colours.danger : colours.warn);
        painter->drawText(box, Qt::AlignCenter | Qt::TextSingleLine, chip.word);
        x += width + kChipGap;
      }
      break;
    }
    case AccountCell::Plain:
      break;
  }
  painter->restore();
}

QSize AccountDelegate::sizeHint(const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
  QSize hint = QStyledItemDelegate::sizeHint(option, index);
  const auto kind = static_cast<AccountCell>(index.data(kAccountCellRole).toInt());
  if (kind == AccountCell::Restrictions) {
    // Wide enough for the chips it holds, so a column sized to its contents
    // makes room for them rather than for the text underneath.
    const QFontMetricsF metrics(small_font(option.font));
    qreal width = 2 * kHorizontalPadding;
    for (const Chip& chip : chips_for(index.data(kAccountFlagsRole).toInt())) {
      width += metrics.horizontalAdvance(chip.word) + (2 * kChipPadding) + kChipGap;
    }
    hint.setWidth(std::max(hint.width(), static_cast<int>(std::ceil(width))));
  } else if (kind == AccountCell::Status) {
    hint.setWidth(static_cast<int>(kDotSize) + (2 * kHorizontalPadding) + 4);
  }
  return hint;
}

}  // namespace dv::ui
