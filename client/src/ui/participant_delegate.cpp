#include "ui/participant_delegate.hpp"

#include <algorithm>

#include <QApplication>
#include <QColor>
#include <QFontMetrics>
#include <QModelIndex>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>
#include <QSize>
#include <QStringList>
#include <QStyle>
#include <QStyleOptionViewItem>

#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The same padding the stylesheet gives a list row, so that the name starts
/// where it did when the style drew it. See theme.cpp.
constexpr int kHorizontalPadding = 8;

/// Between the name and the first icon, and between icons. Wider than a space
/// so the icons read as a column of marks rather than as punctuation.
constexpr int kGap = 8;

/// How much of the microphone is filled when somebody is speaking at the
/// quietest level that still counts as speech. Not zero: an empty outline
/// while the row says "speaking" would look like the meter had broken.
constexpr double kQuietestFill = 0.35;

/// The one-pixel-ish stroke every icon is drawn with, in device independent
/// pixels. Scales with the row, because the icons do.
[[nodiscard]] qreal stroke_for(qreal size) {
  return std::max(1.2, size / 11.0);
}

/// A microphone: a capsule for the head, a cup under it, a stem and a foot.
/// Filled from the bottom up by `fill`, 0 to 1, in `colour`.
void draw_microphone(QPainter* painter, const QRectF& box, const QColor& colour, double fill) {
  const qreal size = box.height();
  const qreal stroke = stroke_for(size);
  painter->setPen(QPen(colour, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter->setBrush(Qt::NoBrush);

  // The head: a capsule taking the upper half, a third of the width.
  const qreal head_width = size * 0.36;
  const QRectF head(box.center().x() - (head_width / 2), box.top() + (size * 0.06), head_width,
                    size * 0.56);
  QPainterPath capsule;
  capsule.addRoundedRect(head, head_width / 2, head_width / 2);

  if (fill > 0.0) {
    // Clipped to the capsule so the fill has its rounded ends, and drawn
    // before the outline so the outline stays crisp on top of it.
    painter->save();
    painter->setClipPath(capsule);
    const qreal filled = head.height() * std::clamp(fill, 0.0, 1.0);
    painter->fillRect(QRectF(head.left(), head.bottom() - filled, head.width(), filled), colour);
    painter->restore();
  }
  painter->drawPath(capsule);

  // The cup: an arc around the lower part of the head, open at the top.
  const qreal cup_width = size * 0.64;
  const QRectF cup(box.center().x() - (cup_width / 2), box.top() + (size * 0.28), cup_width,
                   size * 0.50);
  painter->drawArc(cup, 180 * 16, 180 * 16);

  // The stem and the foot.
  const qreal stem_top = cup.bottom();
  const qreal foot_y = box.bottom() - (size * 0.04);
  painter->drawLine(QPointF(box.center().x(), stem_top), QPointF(box.center().x(), foot_y));
  painter->drawLine(QPointF(box.center().x() - (size * 0.2), foot_y),
                    QPointF(box.center().x() + (size * 0.2), foot_y));
}

/// A stroke from the top right to the bottom left, over whatever was drawn,
/// with a gap in `ground` colour under it so the icon reads as struck through
/// rather than as having an extra line.
void draw_slash(QPainter* painter, const QRectF& box, const QColor& colour, const QColor& ground) {
  const qreal size = box.height();
  const qreal stroke = stroke_for(size);
  const QPointF from(box.right() - (size * 0.12), box.top() + (size * 0.08));
  const QPointF to(box.left() + (size * 0.12), box.bottom() - (size * 0.08));
  painter->setPen(QPen(ground, stroke * 3, Qt::SolidLine, Qt::RoundCap));
  painter->drawLine(from, to);
  painter->setPen(QPen(colour, stroke, Qt::SolidLine, Qt::RoundCap));
  painter->drawLine(from, to);
}

/// A monitor: a screen with a stand under it.
void draw_monitor(QPainter* painter, const QRectF& box, const QColor& colour) {
  const qreal size = box.height();
  const qreal stroke = stroke_for(size);
  painter->setPen(QPen(colour, stroke, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
  painter->setBrush(Qt::NoBrush);

  const QRectF screen(box.left() + (size * 0.06), box.top() + (size * 0.12),
                      box.width() - (size * 0.12), size * 0.58);
  painter->drawRoundedRect(screen, size * 0.08, size * 0.08);

  // A neck and a foot, like the microphone's, so the two icons share a base
  // line and sit level beside each other.
  const qreal foot_y = box.bottom() - (size * 0.06);
  painter->drawLine(QPointF(box.center().x(), screen.bottom()), QPointF(box.center().x(), foot_y));
  painter->drawLine(QPointF(box.center().x() - (size * 0.22), foot_y),
                    QPointF(box.center().x() + (size * 0.22), foot_y));
}

/// Two arcs to the right of `box`, the mark for sound: the same shape a
/// speaker icon carries, minus the speaker.
void draw_sound_waves(QPainter* painter, const QRectF& box, const QColor& colour) {
  const qreal size = box.height();
  const qreal stroke = stroke_for(size);
  painter->setPen(QPen(colour, stroke, Qt::SolidLine, Qt::RoundCap));
  painter->setBrush(Qt::NoBrush);

  const QPointF origin(box.right() - (size * 0.05), box.center().y());
  for (const qreal radius : {size * 0.18, size * 0.34}) {
    const QRectF circle(origin.x() - radius, origin.y() - radius, radius * 2, radius * 2);
    painter->drawArc(circle, -40 * 16, 80 * 16);
  }
}

/// The width the sound waves add beyond the box they are drawn beside.
[[nodiscard]] qreal sound_waves_width(qreal size) {
  return size * 0.34;
}

/// How many icons the state draws, and how wide they are together.
[[nodiscard]] qreal icons_width(int state, qreal size) {
  qreal width = 0;
  const bool microphone =
      (state & (kParticipantMuted | kParticipantSpeaking | kParticipantAudioActive)) != 0;
  if (microphone) {
    width += size + kGap;
  }
  if ((state & kParticipantSharing) != 0) {
    width += size + kGap;
    if ((state & kParticipantSharingWithSound) != 0) {
      width += sound_waves_width(size);
    }
  }
  return width;
}

}  // namespace

QString describe_participant_state(int state) {
  QStringList words;
  if ((state & kParticipantMuted) != 0) {
    words << QStringLiteral("muted");
  } else if ((state & kParticipantSpeaking) != 0) {
    words << QStringLiteral("speaking");
  } else if ((state & kParticipantAudioActive) != 0) {
    words << QStringLiteral("connected");
  }
  if ((state & kParticipantSharing) != 0) {
    // Two words apart, because they answer different questions: whose picture
    // is on screen, and why this person's volume slider is now also the
    // volume of a film.
    words << ((state & kParticipantSharingWithSound) != 0 ? QStringLiteral("sharing with sound")
                                                          : QStringLiteral("sharing"));
  }
  return words.join(QStringLiteral(" · "));
}

ParticipantDelegate::ParticipantDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

void ParticipantDelegate::paint(QPainter* painter, const QStyleOptionViewItem& option,
                                const QModelIndex& index) const {
  QStyleOptionViewItem styled = option;
  initStyleOption(&styled, index);

  // The background, the selection and the focus rectangle still come from the
  // style; the text does not. Clearing it is what stops the style from
  // painting the name underneath the one drawn here.
  const QString name = styled.text;
  styled.text.clear();
  QStyle* style = styled.widget != nullptr ? styled.widget->style() : QApplication::style();
  style->drawControl(QStyle::CE_ItemViewItem, &styled, painter, styled.widget);

  const int state = index.data(kParticipantStateRole).toInt();
  const double level = index.data(kParticipantLevelRole).toDouble();
  const theme::Colors& colours = theme::colors();

  const QFontMetricsF metrics(styled.font);
  // The icons are as tall as a line of the name, which is what makes them
  // read as part of the row rather than as pictures dropped next to it.
  const qreal size = std::floor(metrics.height());
  const QRectF row = QRectF(styled.rect).adjusted(kHorizontalPadding, 0, -kHorizontalPadding, 0);

  // The name first, elided to what is left once the icons have their space:
  // a state is worth more than the last letters of a long name, because the
  // name is also in the tooltip and the state is what changed.
  const qreal icons = icons_width(state, size);
  const qreal name_width = std::max(0.0, row.width() - icons);
  const QString shown = metrics.elidedText(name, Qt::ElideRight, name_width);
  const qreal shown_width = metrics.horizontalAdvance(shown);

  painter->save();
  painter->setRenderHint(QPainter::Antialiasing, true);
  painter->setFont(styled.font);
  const bool selected = (styled.state & QStyle::State_Selected) != 0;
  painter->setPen(selected ? styled.palette.color(QPalette::HighlightedText) : colours.text);
  painter->drawText(QRectF(row.left(), row.top(), name_width, row.height()),
                    Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine, shown);

  qreal x = row.left() + shown_width + kGap;
  const qreal top = row.center().y() - (size / 2);
  // The colour under the slash, so it cuts through the microphone rather than
  // adding a line to it. The selection paints its own background, and the
  // slash has to match whichever is there.
  const QColor ground = selected ? colours.accent_soft : colours.surface;

  if ((state & kParticipantMuted) != 0) {
    const QRectF box(x, top, size, size);
    draw_microphone(painter, box, colours.danger, 0.0);
    draw_slash(painter, box, colours.danger, ground);
    x += size + kGap;
  } else if ((state & kParticipantSpeaking) != 0) {
    const QRectF box(x, top, size, size);
    draw_microphone(painter, box, colours.success,
                    kQuietestFill + ((1.0 - kQuietestFill) * std::clamp(level, 0.0, 1.0)));
    x += size + kGap;
  } else if ((state & kParticipantAudioActive) != 0) {
    const QRectF box(x, top, size, size);
    draw_microphone(painter, box, colours.muted, 0.0);
    x += size + kGap;
  }

  if ((state & kParticipantSharing) != 0) {
    const QRectF box(x, top, size, size);
    draw_monitor(painter, box, colours.accent);
    if ((state & kParticipantSharingWithSound) != 0) {
      draw_sound_waves(painter, box, colours.accent);
    }
  }
  painter->restore();
}

QSize ParticipantDelegate::sizeHint(const QStyleOptionViewItem& option,
                                    const QModelIndex& index) const {
  // The style's own answer, which already includes the stylesheet's padding.
  // Only the width is widened, for the icons, so a row's height stays what
  // every other list on this window has.
  QSize hint = QStyledItemDelegate::sizeHint(option, index);
  const QFontMetricsF metrics(option.font);
  const int state = index.data(kParticipantStateRole).toInt();
  hint.setWidth(hint.width() + static_cast<int>(icons_width(state, std::floor(metrics.height()))) +
                kGap);
  return hint;
}

}  // namespace dv::ui
