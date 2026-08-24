#pragma once

#include <utility>
#include <vector>

#include <QColor>
#include <QPointF>
#include <QString>
#include <QWidget>

namespace dv::ui {

/// One measurement over time, drawn by hand.
///
/// Qt Charts is not a dependency of this project and is not worth becoming one
/// for four line graphs. It is a separate module to find, install and ship on
/// three platforms, it brings Qt Graphics View with it, and what it would draw
/// here is a polyline with a wash under it. QPainter draws that in a couple of
/// hundred lines and adds nothing to any installer.
///
/// The widget keeps no clock of its own. Points arrive stamped and advance()
/// is called with the time the whole dialog agrees on, so four charts on one
/// screen scroll together instead of each drifting on its own timer.
class MetricsChart : public QWidget {
  Q_OBJECT

 public:
  /// `unit` goes after the readings in the header. It is deliberately not put
  /// on the axis labels: the gutter they are written in is under fifty pixels
  /// wide, and three copies of "ms" down one edge crowd out the numbers they
  /// belong to. The unit goes in the title as well, which is where somebody
  /// reading the axis is looking anyway.
  MetricsChart(QString title, QString unit, QWidget* parent = nullptr);

  /// Names the lines and fixes their colours.
  ///
  /// One line for a measurement, two for a pair that shares an axis: the two
  /// directions of a bitrate belong on one scale, or neither can be read
  /// against the other.
  void set_lines(const std::vector<std::pair<QString, QColor>>& lines);

  /// Where this measurement stops being good, and where it stops being
  /// acceptable.
  ///
  /// The numbers come from app::quality_of, and being passed rather than
  /// written here is the point: this chart and the indicator in the status bar
  /// are the same judgement, and a second copy of a threshold is how the two
  /// come to disagree.
  ///
  /// Only meaningful with a single line. A chart with thresholds draws its
  /// reading green, amber or red; one without draws it in the line's colour.
  void set_thresholds(double fair_above, double poor_above);

  /// How much history the width of the widget covers, in milliseconds.
  void set_window(double window_ms);

  /// Replaces every line's points. `points[i]` belongs to line i, with x in
  /// the same milliseconds advance() is given.
  void set_points(std::vector<std::vector<QPointF>> points);

  /// Moves the drawing on by `elapsed_ms` and repaints.
  ///
  /// Two jobs, and both are what makes this look alive rather than a picture
  /// replaced five times a second. `now_ms` slides the right edge, so the line
  /// drifts left between readings instead of standing still and then jumping.
  /// `elapsed_ms` advances the eased axis, readings and colour, so a scale
  /// that has to grow grows into place rather than snapping.
  void advance(double now_ms, double elapsed_ms);

  /// Forgets the points and the eased state, so nothing of the last call is
  /// left on screen to be read as belonging to the next one.
  void clear();

 protected:
  void paintEvent(QPaintEvent* event) override;

  // Not redundant: the section above is `protected:`, for an override, and
  // these are the widget's own members.
  // NOLINTNEXTLINE(readability-redundant-access-specifiers)
 private:
  struct Line {
    QString name;
    QColor colour;
    std::vector<QPointF> points;
    /// The newest value, eased. A figure that jumps five times a second is
    /// read as noise rather than as a measurement.
    double reading = 0.0;
    bool has_reading = false;
  };

  /// The colour the first line and its reading are drawn in: the eased verdict
  /// colour where there are thresholds, the line's own where there are not.
  [[nodiscard]] QColor leading_colour() const;

  /// How long the time axis takes to carry the trace one pixel left.
  ///
  /// The floor on how often this is worth repainting when nothing else is
  /// moving. See advance().
  [[nodiscard]] double scroll_step_ms() const;

  QString title_;
  QString unit_;
  std::vector<Line> lines_;

  double window_ms_ = 60000.0;
  double now_ms_ = 0.0;
  /// The time the drawing on screen was made for. What advance() compares
  /// against to decide whether the trace has moved far enough to be worth
  /// drawing again.
  double drawn_at_ms_ = 0.0;

  /// Where the axis stops, and where it is on its way to. Eased, because a
  /// scale that resizes the instant a peak arrives moves the whole line under
  /// the eye trying to read it.
  double axis_top_ = 1.0;
  double axis_target_ = 1.0;

  /// The verdict colour on its way to the current verdict. Interpolated rather
  /// than switched, so a call sliding from good to poor looks like sliding
  /// rather than like a light being turned on.
  QColor drawn_colour_;
  bool coloured_ = false;

  double fair_above_ = 0.0;
  double poor_above_ = 0.0;
  bool has_thresholds_ = false;
};

}  // namespace dv::ui
