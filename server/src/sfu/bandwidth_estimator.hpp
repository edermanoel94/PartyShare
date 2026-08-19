#pragma once

#include <algorithm>
#include <cstdint>

namespace dv::server::sfu {

/// Decides how much the sender of a screen share should be sending.
///
/// The SFU is the only place in the system that can decide this. The sender
/// cannot: what it sees is its own outgoing link. A viewer cannot either: what
/// it sees is its own incoming one. The SFU sees the loss on the way in from
/// the sharer and, through REMB, what every viewer is telling it about the way
/// out, and the number it produces is the smallest of those truths.
///
/// The rule is the loss based half of Google's congestion control, which is
/// what REMB was designed to carry:
///
/// - above 10% loss the link is congested, and the target comes down in
///   proportion to how bad it is;
/// - below 2% the link has room, and the target grows by a few percent;
/// - in between nothing happens, because reacting to noise is how a rate
///   oscillates instead of settling.
///
/// Multiplicative decrease and multiplicative increase, with the decrease much
/// sharper than the increase: backing off has to be faster than probing, or
/// congestion lasts longer than it needs to.
///
/// Pure arithmetic, with no libdatachannel and no clock in it, so that the
/// behaviour can be tested rather than observed.
class BandwidthEstimator {
 public:
  struct Options {
    /// Where it starts, before anything is known about the link. Section 6 of
    /// SPEC.md puts the screen share between 1.5 and 3 Mbps.
    int start_kbps = 1500;
    /// The lowest it will ask for. Below this the picture is not worth the
    /// bandwidth, and the answer is a slow screen rather than a broken one.
    int min_kbps = 300;
    int max_kbps = 3000;

    /// Loss above this is congestion.
    double back_off_above = 0.10;
    /// Loss below this is a link with room to spare.
    double grow_below = 0.02;
    /// How fast to probe upwards. Eight percent a second reaches 3 Mbps from
    /// 300 kbps in about half a minute.
    double growth = 1.08;
  };

  BandwidthEstimator() : BandwidthEstimator(Options{}) {}
  explicit BandwidthEstimator(Options options)
      : options_(options),
        target_kbps_(std::clamp(options.start_kbps, options.min_kbps, options.max_kbps)) {}

  /// Takes one interval's worth of observation and returns the new target.
  ///
  /// An interval with no packets at all leaves the target alone: a stream that
  /// stopped says nothing about the link it stopped on.
  int update(std::uint64_t packets_received, std::uint64_t packets_lost) {
    const std::uint64_t total = packets_received + packets_lost;
    if (total == 0) {
      return target_kbps_;
    }

    const double fraction = static_cast<double>(packets_lost) / static_cast<double>(total);
    double target = target_kbps_;
    if (fraction > options_.back_off_above) {
      // Half the loss, which is the usual gain: at 20% loss the target drops
      // by a tenth each second, so it converges in seconds rather than in one
      // jump that overshoots.
      target *= 1.0 - (0.5 * fraction);
    } else if (fraction < options_.grow_below) {
      target *= options_.growth;
    }

    target_kbps_ = std::clamp(static_cast<int>(target), options_.min_kbps, options_.max_kbps);
    return target_kbps_;
  }

  /// Caps the target at what somebody else can take, which is how a viewer on
  /// a slow link limits what the sharer produces for everybody. Zero means no
  /// cap.
  void set_ceiling(int kbps) { ceiling_kbps_ = kbps; }

  /// What to ask the sender for: the estimate, capped.
  [[nodiscard]] int target_kbps() const {
    if (ceiling_kbps_ <= 0) {
      return target_kbps_;
    }
    return std::max(options_.min_kbps, std::min(target_kbps_, ceiling_kbps_));
  }

  [[nodiscard]] const Options& options() const { return options_; }

 private:
  Options options_;
  int target_kbps_;
  int ceiling_kbps_ = 0;
};

}  // namespace dv::server::sfu
