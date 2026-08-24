#include "ui/metrics_chart.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <utility>
#include <vector>

#include <QBrush>
#include <QFont>
#include <QFontMetricsF>
#include <QLinearGradient>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRectF>

#include "app/metrics_history.hpp"
#include "app/smoothing.hpp"
#include "ui/theme.hpp"

namespace dv::ui {
namespace {

/// The band across the top holding the title and the readings.
constexpr double kHeaderHeight = 26.0;
/// The column on the left the axis labels are written in.
constexpr double kGutter = 48.0;
constexpr double kPadding = 10.0;
/// Under this the plot has no room for a line between its own grid lines.
constexpr double kMinimumPlotHeight = 36.0;

/// How fast the eased parts of the drawing catch up, in milliseconds.
constexpr double kAxisTau = 320.0;
constexpr double kReadingTau = 180.0;
constexpr double kColourTau = 260.0;

/// Air above the tallest reading, so a peak is not drawn against the top edge
/// where it cannot be told from one that went off the chart.
constexpr double kHeadroom = 1.15;

/// Enough of the threshold to keep its guide line on the chart even while
/// every reading is far below it. A dashed line at 150 ms is what gives an
/// axis of milliseconds a sense of scale.
constexpr double kThresholdHeadroom = 1.2;

[[nodiscard]] QColor with_alpha(QColor colour, int alpha) {
  colour.setAlpha(alpha);
  return colour;
}

/// True when an eased value has arrived and there is nothing left to draw.
///
/// app::approach lands exactly on its target rather than halving the distance
/// forever, so this is asking whether the last frame has already been drawn.
[[nodiscard]] bool settled(double value, double target) {
  return std::abs(target - value) < dv::client::app::kSettled;
}

[[nodiscard]] int approach_channel(int from, int to, double elapsed_ms) {
  return static_cast<int>(std::lround(client::app::approach(from, to, elapsed_ms, kColourTau)));
}

/// Eases one colour towards another, channel by channel.
[[nodiscard]] QColor approach_colour(const QColor& from, const QColor& to, double elapsed_ms) {
  return {approach_channel(from.red(), to.red(), elapsed_ms),
          approach_channel(from.green(), to.green(), elapsed_ms),
          approach_channel(from.blue(), to.blue(), elapsed_ms)};
}

/// A number with as many decimals as it is worth reading.
///
/// Two decimals on a round trip time of 43 milliseconds is precision the
/// measurement does not have; none at all on a packet loss of 0.4% is the
/// difference between a call losing packets and a call that is not.
[[nodiscard]] QString format_value(double value) {
  const double magnitude = std::abs(value);
  // Nothing, written as nothing. "0.00 ms" is not more accurate than "0", and
  // it is what all four of these read while nobody is speaking, so it is the
  // resting state of the whole window.
  if (magnitude < 0.005) {
    return QStringLiteral("0");
  }
  if (magnitude >= 100.0) {
    return QString::number(value, 'f', 0);
  }
  // Two decimals below one, and one above it. A round trip time of 8.5 ms does
  // not know its own hundredths, while a packet loss of 0.42% is a different
  // call from one of 0.04% and rounding it to a tenth loses which.
  return QString::number(value, 'f', magnitude >= 1.0 ? 1 : 2);
}

/// An axis label, which is a different question from a reading.
///
/// The decimals follow the top of the axis rather than the label's own value,
/// or a scale of 0 to 200 would be labelled "0.00", "100" and "200" - three
/// numbers written to three different precisions down one edge. The floor is
/// always a bare zero: "0.00 ms" is not more accurate than "0", it is only
/// longer, in the narrowest column on the widget.
[[nodiscard]] QString format_axis(double value, double top) {
  if (value <= 0.0) {
    return QStringLiteral("0");
  }
  int decimals = 2;
  if (top >= 10.0) {
    decimals = 0;
  } else if (top >= 1.0) {
    decimals = 1;
  }
  return QString::number(value, 'f', decimals);
}

/// The colour a reading has earned against its two thresholds.
[[nodiscard]] QColor verdict_for(double reading, double fair_above, double poor_above,
                                 const theme::Colors& colours) {
  if (reading > poor_above) {
    return colours.danger;
  }
  if (reading > fair_above) {
    return colours.warn;
  }
  return colours.success;
}

/// Under this many pixels apart, two readings are joined with a straight line.
///
/// A curve needs room to be a curve. Qt flattens every cubic into a run of
/// short line segments before it rasterises it, so a curve spanning three
/// pixels is several segments drawing what one would draw, and the smoothing
/// it buys is smaller than a pixel. A minute of readings at five a second is
/// three hundred of them across four hundred pixels.
constexpr double kStraightBelow = 6.0;

/// A smooth line through the points, and no further.
///
/// The control points sit level with the ends of each segment rather than
/// tangent to the neighbouring ones. A spline that reads its neighbours'
/// slopes overshoots between two readings far apart, and an overshoot on a
/// chart of packet loss draws loss that was never measured.
[[nodiscard]] QPainterPath curve_through(const std::vector<QPointF>& screen) {
  QPainterPath path;
  if (screen.empty()) {
    return path;
  }
  path.moveTo(screen.front());
  for (std::size_t index = 1; index < screen.size(); ++index) {
    const QPointF& from = screen[index - 1];
    const QPointF& to = screen[index];
    const double span = to.x() - from.x();
    if (span < kStraightBelow) {
      path.lineTo(to);
      continue;
    }
    const double half = span / 2.0;
    path.cubicTo(QPointF(from.x() + half, from.y()), QPointF(to.x() - half, to.y()), to);
  }
  return path;
}

/// The widget's font, a size down, for labels and legends.
[[nodiscard]] QFont smaller_than(const QFont& base) {
  QFont small = base;
  if (small.pointSizeF() > 0.0) {
    small.setPointSizeF(std::max(7.0, small.pointSizeF() - 1.5));
  } else if (small.pixelSize() > 0) {
    small.setPixelSize(std::max(9, small.pixelSize() - 2));
  }
  return small;
}

}  // namespace

MetricsChart::MetricsChart(QString title, QString unit, QWidget* parent)
    : QWidget(parent), title_(std::move(title)), unit_(std::move(unit)) {
  // Tall enough for the header, three grid lines and a line between them.
  setMinimumHeight(104);
  setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
  // Every pixel is painted, card and all, so Qt does not have to clear it
  // first. A clear of the whole widget before every repaint is not free.
  setAttribute(Qt::WA_OpaquePaintEvent);
}

void MetricsChart::set_lines(const std::vector<std::pair<QString, QColor>>& lines) {
  lines_.clear();
  lines_.reserve(lines.size());
  for (const auto& [name, colour] : lines) {
    lines_.push_back(Line{.name = name, .colour = colour});
  }
  update();
}

void MetricsChart::set_thresholds(double fair_above, double poor_above) {
  fair_above_ = fair_above;
  poor_above_ = poor_above;
  has_thresholds_ = true;
}

void MetricsChart::set_window(double window_ms) {
  window_ms_ = window_ms > 0.0 ? window_ms : 1.0;
}

void MetricsChart::set_points(std::vector<std::vector<QPointF>> points) {
  double tallest = 0.0;
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    if (index < points.size()) {
      lines_[index].points = std::move(points[index]);
    } else {
      lines_[index].points.clear();
    }
    for (const QPointF& point : lines_[index].points) {
      tallest = std::max(tallest, point.y());
    }
  }

  double wanted = tallest * kHeadroom;
  if (has_thresholds_) {
    wanted = std::max(wanted, fair_above_ * kThresholdHeadroom);
  }
  axis_target_ = client::app::nice_ceiling(wanted);
}

double MetricsChart::scroll_step_ms() const {
  // How much time has to pass before the trace has moved one pixel left.
  const double plot_width = static_cast<double>(width()) - kGutter - kPadding;
  return plot_width <= 1.0 ? window_ms_ : window_ms_ / plot_width;
}

void MetricsChart::advance(double now_ms, double elapsed_ms) {
  now_ms_ = now_ms;

  // What is still easing has to be drawn every frame, because that is what
  // easing is for. Each is asked before it is advanced: approach() lands
  // exactly on its target, and the frame that lands is the last one worth
  // drawing.
  bool settling = !settled(axis_top_, axis_target_);
  axis_top_ = client::app::approach(axis_top_, axis_target_, elapsed_ms, kAxisTau);

  for (Line& line : lines_) {
    if (line.points.empty()) {
      continue;
    }
    const double target = line.points.back().y();
    settling = settling || !settled(line.reading, target);
    line.reading = client::app::approach(line.reading, target, elapsed_ms, kReadingTau);
    line.has_reading = true;
  }

  if (has_thresholds_ && !lines_.empty()) {
    const QColor verdict =
        verdict_for(lines_.front().reading, fair_above_, poor_above_, theme::colors());
    settling = settling || (coloured_ && drawn_colour_ != verdict);
    // The first verdict is taken rather than eased into. Fading up from
    // whatever colour the widget was built with would make the chart's opening
    // half second a statement about the call that nothing had measured.
    drawn_colour_ = coloured_ ? approach_colour(drawn_colour_, verdict, elapsed_ms) : verdict;
    coloured_ = true;
  }

  // With nothing easing, the only thing moving is the time axis, and it carries
  // the trace the width of the widget over the length of the window: a minute
  // across four hundred pixels is one pixel every eighth of a second. Redrawing
  // in between is a curve of three hundred antialiased segments and a gradient
  // under it, to put every pixel back where it already was.
  //
  // Measured, not guessed. Four of these repainting on every frame of the
  // dialog's timer cost twenty-two percent of a core, against two and a half
  // for the whole rest of the call.
  if (!settling && now_ms_ - drawn_at_ms_ < scroll_step_ms()) {
    return;
  }
  drawn_at_ms_ = now_ms_;
  update();
}

void MetricsChart::clear() {
  for (Line& line : lines_) {
    line.points.clear();
    line.reading = 0.0;
    line.has_reading = false;
  }
  axis_top_ = 1.0;
  axis_target_ = 1.0;
  coloured_ = false;
  drawn_at_ms_ = 0.0;
  update();
}

QColor MetricsChart::leading_colour() const {
  if (lines_.empty()) {
    return theme::colors().muted;
  }
  return has_thresholds_ && coloured_ ? drawn_colour_ : lines_.front().colour;
}

void MetricsChart::paintEvent(QPaintEvent* /*event*/) {
  QPainter painter(this);
  painter.setRenderHint(QPainter::Antialiasing, true);

  const theme::Colors& colours = theme::colors();
  const QRectF whole(rect());

  // The card. WA_OpaquePaintEvent promises every pixel, so what falls outside
  // the rounded corners is filled with the window colour by hand rather than
  // left to Qt to clear.
  painter.fillRect(whole, colours.window);
  QPainterPath card;
  card.addRoundedRect(whole, theme::kControlRadius, theme::kControlRadius);
  painter.fillPath(card, colours.surface);

  const QFont small = smaller_than(font());
  const QFontMetricsF measured(small);

  painter.setFont(small);
  painter.setPen(colours.muted);
  const QRectF header(kPadding, 0.0, whole.width() - (2.0 * kPadding), kHeaderHeight);
  painter.drawText(header, Qt::AlignLeft | Qt::AlignVCenter, title_);

  // The readings, right aligned in the header. One line gets a single figure
  // in the verdict colour; two get a dot, a name and a figure each, because
  // "240 kbps" with no arrow beside it does not say which direction it is.
  if (lines_.size() == 1 && lines_.front().has_reading) {
    QFont strong = font();
    strong.setBold(true);
    painter.setFont(strong);
    painter.setPen(leading_colour());
    painter.drawText(header, Qt::AlignRight | Qt::AlignVCenter,
                     QStringLiteral("%1 %2").arg(format_value(lines_.front().reading), unit_));
  } else if (lines_.size() > 1) {
    // Right to left, because that is the direction the space runs out in: each
    // reading is placed against the edge left by the one after it, and the
    // last line named is the one nearest the corner.
    double right = whole.width() - kPadding;
    for (const Line& line : std::views::reverse(lines_)) {
      if (!line.has_reading) {
        continue;
      }
      const QString text =
          QStringLiteral("%1 %2 %3").arg(line.name, format_value(line.reading), unit_);
      const double width = measured.horizontalAdvance(text);
      painter.setPen(line.colour);
      painter.drawText(QRectF(right - width, 0.0, width, kHeaderHeight),
                       Qt::AlignRight | Qt::AlignVCenter, text);
      right -= width + 14.0;
    }
  }

  const QRectF plot(kGutter, kHeaderHeight, whole.width() - kGutter - kPadding,
                    whole.height() - kHeaderHeight - kPadding);
  if (plot.width() <= 0.0 || plot.height() < kMinimumPlotHeight || axis_top_ <= 0.0) {
    return;
  }

  // The grid: the floor, the middle and the ceiling, labelled. Three, because
  // this is a hundred pixels tall and a denser grid is a grey block with a
  // line somewhere in it.
  painter.setFont(small);
  const std::array<double, 3> levels{0.0, 0.5, 1.0};
  for (const double level : levels) {
    const double y = plot.bottom() - (level * plot.height());
    painter.setPen(QPen(with_alpha(colours.border, 150), 1.0));
    painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    painter.setPen(colours.muted);
    painter.drawText(QRectF(0.0, y - (kHeaderHeight / 2.0), kGutter - 8.0, kHeaderHeight),
                     Qt::AlignRight | Qt::AlignVCenter, format_axis(level * axis_top_, axis_top_));
  }

  if (has_thresholds_) {
    const std::array<std::pair<double, QColor>, 2> guides{
        std::pair<double, QColor>{fair_above_, colours.warn},
        std::pair<double, QColor>{poor_above_, colours.danger}};
    for (const auto& [at, colour] : guides) {
      if (at <= 0.0 || at > axis_top_) {
        continue;
      }
      const double y = plot.bottom() - ((at / axis_top_) * plot.height());
      painter.setPen(QPen(with_alpha(colour, 110), 1.0, Qt::DashLine));
      painter.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
  }

  const bool anything = std::any_of(lines_.begin(), lines_.end(),
                                    [](const Line& line) { return !line.points.empty(); });
  if (!anything) {
    painter.setPen(colours.muted);
    painter.drawText(plot, Qt::AlignCenter, QStringLiteral("waiting for the first reading"));
    return;
  }

  painter.save();
  painter.setClipRect(plot);
  for (std::size_t index = 0; index < lines_.size(); ++index) {
    const Line& line = lines_[index];
    if (line.points.empty()) {
      continue;
    }
    const QColor colour = index == 0 ? leading_colour() : line.colour;

    std::vector<QPointF> screen;
    screen.reserve(line.points.size());
    for (const QPointF& point : line.points) {
      const double x = plot.right() - (((now_ms_ - point.x()) / window_ms_) * plot.width());
      const double y =
          plot.bottom() - ((std::clamp(point.y(), 0.0, axis_top_) / axis_top_) * plot.height());
      screen.emplace_back(x, y);
    }

    const QPainterPath path = curve_through(screen);

    // The wash under the line, and only on a chart with one line. Two of them
    // overlapping is a third colour that means nothing.
    if (lines_.size() == 1) {
      QPainterPath area = path;
      area.lineTo(screen.back().x(), plot.bottom());
      area.lineTo(screen.front().x(), plot.bottom());
      area.closeSubpath();
      QLinearGradient wash(QPointF(0.0, plot.top()), QPointF(0.0, plot.bottom()));
      wash.setColorAt(0.0, with_alpha(colour, 90));
      wash.setColorAt(1.0, with_alpha(colour, 0));
      painter.fillPath(area, wash);
    }

    painter.setBrush(Qt::NoBrush);
    painter.setPen(QPen(colour, 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    painter.drawPath(path);

    // The newest reading, with a halo, so the eye has somewhere to rest on a
    // line that is moving.
    painter.setPen(Qt::NoPen);
    painter.setBrush(with_alpha(colour, 70));
    painter.drawEllipse(screen.back(), 6.0, 6.0);
    painter.setBrush(colour);
    painter.drawEllipse(screen.back(), 3.0, 3.0);
  }
  painter.restore();
}

}  // namespace dv::ui
