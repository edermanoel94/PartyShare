#pragma once

#include <cstdint>
#include <deque>

#include "media/media_session.hpp"

namespace dv::client::app {

/// One reading of the call, reduced to the numbers a chart draws.
struct MetricsSample {
  /// When it was taken, on whatever clock the caller is keeping. Milliseconds.
  double at_ms = 0.0;
  double round_trip_time_ms = 0.0;
  double jitter_ms = 0.0;
  /// Packets lost over the interval since the reading before this one, as a
  /// percentage of what was expected in that interval.
  ///
  /// Deliberately not the loss of the whole call, which is what the counter in
  /// media::AudioStats holds. A running total only ever goes up, so a chart of
  /// it is a staircase that says a call is getting worse for as long as it
  /// lasts, and a bad thirty seconds at the start stays on screen as a step
  /// nobody can date. What somebody watching this wants to know is whether
  /// packets are going missing *now*.
  double loss_percent = 0.0;
  double send_kbps = 0.0;
  double receive_kbps = 0.0;
};

/// The readings of the last window, and the counters needed to turn the
/// running totals into rates.
///
/// Free of Qt, so the arithmetic that decides what the chart says can be
/// tested without a window. Section 15 of SPEC.md is the rule; this is the
/// half of ui::MetricsChart that is worth a test.
class MetricsHistory {
 public:
  /// `window_ms` is how much history is kept. Readings older than that are
  /// dropped as new ones arrive.
  explicit MetricsHistory(double window_ms);

  /// Adds one reading, taken at `at_ms`.
  ///
  /// Time is the caller's to supply rather than read here, because the caller
  /// is the one that has to plot it on an axis, and two clocks read a
  /// microsecond apart put the newest point a microsecond off the right edge.
  void observe(const media::AudioStats& stats, double at_ms);

  [[nodiscard]] const std::deque<MetricsSample>& samples() const noexcept { return samples_; }
  [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }
  [[nodiscard]] double window_ms() const noexcept { return window_ms_; }

  /// Forgets everything, including the counters the next loss figure would be
  /// measured against.
  void clear() noexcept;

 private:
  double window_ms_;
  std::deque<MetricsSample> samples_;

  /// False until the first reading. The first one has no interval behind it,
  /// so it has no loss rate either, and inventing one from the running total
  /// would open every chart with the whole call's loss painted onto its first
  /// point.
  bool measured_ = false;
  std::uint64_t packets_lost_ = 0;
  std::uint64_t packets_received_ = 0;
};

/// The round number an axis should stop at in order to hold `value`.
///
/// One, two or five times a power of ten. An axis that stops at exactly the
/// tallest reading redraws its labels on every reading and leaves the peak
/// touching the top edge, where it cannot be told from a peak that went off
/// the chart.
[[nodiscard]] double nice_ceiling(double value) noexcept;

}  // namespace dv::client::app
