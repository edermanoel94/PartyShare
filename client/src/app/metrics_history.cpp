#include "app/metrics_history.hpp"

#include <cmath>

namespace dv::client::app {

MetricsHistory::MetricsHistory(double window_ms) : window_ms_(window_ms > 0.0 ? window_ms : 1.0) {}

void MetricsHistory::observe(const media::AudioStats& stats, double at_ms) {
  MetricsSample sample;
  sample.at_ms = at_ms;
  sample.round_trip_time_ms = stats.round_trip_time_ms;
  sample.jitter_ms = stats.jitter_ms;
  sample.send_kbps = stats.send_bitrate_kbps;
  sample.receive_kbps = stats.receive_bitrate_kbps;

  // Counters that went backwards are a second call in the same window, not a
  // negative loss. WebRTC starts them again from zero for a new peer
  // connection, and subtracting across that boundary produces a figure with an
  // exponent in it.
  const bool restarted =
      stats.packets_lost < packets_lost_ || stats.packets_received < packets_received_;

  if (measured_ && !restarted) {
    const std::uint64_t lost = stats.packets_lost - packets_lost_;
    const std::uint64_t received = stats.packets_received - packets_received_;
    if (const std::uint64_t expected = lost + received; expected > 0) {
      sample.loss_percent = 100.0 * static_cast<double>(lost) / static_cast<double>(expected);
    }
  }

  packets_lost_ = stats.packets_lost;
  packets_received_ = stats.packets_received;
  measured_ = true;

  samples_.push_back(sample);
  while (!samples_.empty() && samples_.front().at_ms < at_ms - window_ms_) {
    samples_.pop_front();
  }
}

void MetricsHistory::clear() noexcept {
  samples_.clear();
  measured_ = false;
  packets_lost_ = 0;
  packets_received_ = 0;
}

double nice_ceiling(double value) noexcept {
  // Not zero, and not negative either: every number this is asked about is a
  // duration, a rate or a percentage. An axis from zero to zero has no height
  // to plot anything in and divides by nothing when it tries.
  //
  // Written as a negated comparison rather than `value <= 0.0` so that a NaN,
  // which loses every comparison it is given, lands here instead of reaching
  // std::log10 and turning the whole axis into one.
  if (!(value > 0.0)) {
    return 1.0;
  }

  const double magnitude = std::pow(10.0, std::floor(std::log10(value)));
  const double scaled = value / magnitude;
  double step = 10.0;
  if (scaled <= 1.0) {
    step = 1.0;
  } else if (scaled <= 2.0) {
    step = 2.0;
  } else if (scaled <= 5.0) {
    step = 5.0;
  }
  return step * magnitude;
}

}  // namespace dv::client::app
