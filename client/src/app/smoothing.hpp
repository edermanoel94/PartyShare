#pragma once

#include <algorithm>
#include <cmath>

namespace dv::client::app {

/// How close is close enough to stop moving.
///
/// An exponential approach halves the remaining distance every time constant
/// and never arrives, so without a threshold every animation in the interface
/// would run for as long as the window is open. A thousandth is below one step
/// of any bar or axis this drives, which makes it invisible and final.
inline constexpr double kSettled = 0.001;

/// Moves `current` towards `target` by the share of the distance that
/// `elapsed_ms` is worth.
///
/// Exponential rather than a fixed step per frame. A step per frame ties how
/// fast the thing moves to how often the timer manages to fire, so the same
/// animation runs at one speed on an idle machine and at half of it under
/// load, which reads as the interface stuttering rather than as the timer
/// slipping.
///
/// `tau_ms` is the time constant: after that long, about 63% of the distance
/// is gone. Zero or less means arrive immediately.
[[nodiscard]] inline double approach(double current, double target, double elapsed_ms,
                                     double tau_ms) noexcept {
  if (elapsed_ms <= 0.0) {
    return current;
  }
  if (tau_ms <= 0.0) {
    return target;
  }
  const double moved = current + ((target - current) * (1.0 - std::exp(-elapsed_ms / tau_ms)));
  return std::abs(target - moved) < kSettled ? target : moved;
}

/// Where a level meter stops showing anything at all, in dBFS.
///
/// -60 is a quiet room. Below it what is left is the microphone's own hiss,
/// and a bar that shows hiss is a bar that is never empty.
inline constexpr double kMeterFloorDb = -60.0;

/// The share of a meter a linear amplitude should fill, from 0 to 1.
///
/// Logarithmic, because hearing is. On a linear scale the top six decibels
/// take up most of the bar and everything a person actually says crowds into
/// the bottom of it, barely moving.
[[nodiscard]] inline double meter_fraction(double level) noexcept {
  if (level <= 0.0) {
    return 0.0;
  }
  const double decibels = 20.0 * std::log10(level);
  if (decibels <= kMeterFloorDb) {
    return 0.0;
  }
  return std::clamp((decibels - kMeterFloorDb) / -kMeterFloorDb, 0.0, 1.0);
}

/// A level meter's ballistics: how fast the bar is allowed to move.
///
/// Measurements arrive five times a second, which is well under what the eye
/// reads as movement. Writing each one straight to the bar gives five jumps a
/// second, and a syllable that lands between two readings is either held for
/// the whole 200 ms or missed entirely.
///
/// So the drawn value is advanced every frame towards the last measurement,
/// and the two directions are deliberately not symmetric. Rising is nearly
/// immediate, because a meter that lags the start of a word is not showing
/// that word. Falling is slow, because that is what makes a peak readable: a
/// bar that drops as fast as it rose is a flicker.
///
/// No Qt here on purpose. This is arithmetic, and section 15 of SPEC.md keeps
/// arithmetic where it can be tested without opening a window.
struct LevelMeter {
  /// Time constants for the two directions, in milliseconds.
  ///
  /// The attack is set against the 200 ms between measurements rather than
  /// against what a hardware meter would do. Thirty milliseconds is what a
  /// real one uses and it is wrong here: it arrives at each new peak long
  /// before the next reading, so the bar climbs in the same five steps a
  /// second it used to and only the descents look smooth. Sixty spends a
  /// third of the gap travelling, which is still under a tenth of a second and
  /// so still reads as immediate.
  double attack_ms = 60.0;
  double release_ms = 350.0;

  /// The last measurement, from 0 to 1.
  double target = 0.0;
  /// What to draw, from 0 to 1.
  double value = 0.0;

  void observe(double fraction) noexcept { target = std::clamp(fraction, 0.0, 1.0); }

  /// Advances the drawn value and answers it.
  double advance(double elapsed_ms) noexcept {
    value = approach(value, target, elapsed_ms, target > value ? attack_ms : release_ms);
    return value;
  }

  /// True when there is nothing left to animate, which is when whatever is
  /// driving this can stop asking.
  [[nodiscard]] bool at_rest() const noexcept { return std::abs(target - value) < kSettled; }
};

}  // namespace dv::client::app
