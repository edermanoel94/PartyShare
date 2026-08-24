#pragma once

#include <string_view>

#include "media/media_session.hpp"

namespace dv::client::app {

/// How the call is holding up, for the indicator section 19 of SPEC.md asks
/// for.
enum class NetworkQuality : std::uint8_t {
  /// No numbers yet. Not the same as bad.
  Unknown,
  Good,
  Fair,
  Poor,
};

[[nodiscard]] constexpr std::string_view to_string(NetworkQuality quality) noexcept {
  switch (quality) {
    case NetworkQuality::Unknown:
      return "unknown";
    case NetworkQuality::Good:
      return "good";
    case NetworkQuality::Fair:
      return "fair";
    case NetworkQuality::Poor:
      return "poor";
  }
  return "unknown";
}

/// Where each of the three measurements stops being good, and where it stops
/// being acceptable.
///
/// They come from what voice actually needs rather than from what looks tidy.
/// Round trip time above 300 ms is where people start talking over each other;
/// jitter above 30 ms is more than a normal buffer absorbs; and one packet in
/// twenty lost is where Opus stops hiding it.
///
/// Named rather than written into the comparisons below, because they are read
/// in two places now. ui::MetricsDialog draws a guide line at each of them, and
/// a chart whose amber line sits somewhere other than where the indicator turns
/// amber is worse than a chart with no guide at all.
inline constexpr double kFairRoundTripMs = 150.0;
inline constexpr double kPoorRoundTripMs = 300.0;
inline constexpr double kFairJitterMs = 15.0;
inline constexpr double kPoorJitterMs = 30.0;
inline constexpr double kFairLossPercent = 1.0;
inline constexpr double kPoorLossPercent = 5.0;

/// Turns the measurements of section 22 of SPEC.md into one word.
///
/// The worst of the three decides. A call with perfect latency and a fifth of
/// its packets missing is not a good call.
[[nodiscard]] constexpr NetworkQuality quality_of(const media::AudioStats& stats) noexcept {
  // Nothing has been measured yet. A fresh call has zero of everything, and
  // calling that good would light the indicator green before a single packet
  // has arrived.
  if (stats.packets_received == 0 && stats.packets_sent == 0) {
    return NetworkQuality::Unknown;
  }

  const std::uint64_t expected = stats.packets_received + stats.packets_lost;
  const double loss = expected == 0 ? 0.0
                                    : 100.0 * static_cast<double>(stats.packets_lost) /
                                          static_cast<double>(expected);

  if (stats.round_trip_time_ms > kPoorRoundTripMs || stats.jitter_ms > kPoorJitterMs ||
      loss > kPoorLossPercent) {
    return NetworkQuality::Poor;
  }
  if (stats.round_trip_time_ms > kFairRoundTripMs || stats.jitter_ms > kFairJitterMs ||
      loss > kFairLossPercent) {
    return NetworkQuality::Fair;
  }
  return NetworkQuality::Good;
}

}  // namespace dv::client::app
