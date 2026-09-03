#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

#include <rtc/rtc.hpp>

#include "sfu/bandwidth_estimator.hpp"
#include "sfu/loss_repair.hpp"

namespace dv::server::sfu {

/// Everything the SFU tells the sender of a screen share about the stream it
/// is sending, which is two things: send that packet again, and send less.
///
/// Both come from the same observation, the sequence numbers arriving on the
/// track, and both go back on the same RTCP path, which is why they are one
/// handler rather than two that would each have to track the stream.
///
/// # Send that packet again
///
/// That half is sfu::LossRepair, which this handler drives and which the
/// audio installs on its own. The measurement that led to it is in
/// docs/11-benchmarks.md: a viewer that misses a packet of a screen share can
/// only ask for a new intra frame, and on a 5% loss link almost none of those
/// arrive whole.
///
/// # Send less
///
/// Retransmission repairs a link that loses a little. A link that loses a lot
/// is one being asked for more than it can carry, and the repair makes it
/// worse. What fixes that is the sender producing less, and the only party
/// that can tell it so is the SFU: see sfu/bandwidth_estimator.hpp.
///
/// The number travels as REMB, which is what `a=rtcp-fb:96 goog-remb` in the
/// offer negotiates, and libwebrtc treats it as a ceiling on what its own
/// congestion controller may aim for. Sent once a second, and only while a
/// stream is actually arriving.
class VideoFeedback final : public rtc::MediaHandler {
 public:
  struct Options {
    /// The repair's own knobs. See sfu::LossRepair::Options for each.
    std::uint16_t max_gap = 256;
    int max_requests = 3;
    std::chrono::milliseconds retry_after{40};
    std::chrono::milliseconds give_up_after{500};

    /// How often the sender is told what to aim for. One second is what REMB
    /// implementations use: often enough to follow a link that turns bad,
    /// rarely enough that the feedback is not itself traffic.
    std::chrono::milliseconds bandwidth_interval{1000};
    BandwidthEstimator::Options bandwidth;
  };

  VideoFeedback() : VideoFeedback(Options{}) {}
  explicit VideoFeedback(Options options)
      : options_(options),
        repair_(LossRepair::Options{.max_gap = options.max_gap,
                                    .max_requests = options.max_requests,
                                    .retry_after = options.retry_after,
                                    .give_up_after = options.give_up_after}),
        bandwidth_(options.bandwidth) {}

  void incoming(rtc::message_vector& messages, const rtc::message_callback& send) override;

  /// Caps what the sharer is asked for, which is how the slowest viewer limits
  /// what everybody gets. Zero lifts the cap.
  void set_bandwidth_ceiling(int kbps);

  /// What the sender is currently being asked to aim for, in kbps.
  [[nodiscard]] int target_kbps() const;

  /// How many REMB packets went back to the sender.
  [[nodiscard]] std::uint64_t bandwidth_reports_sent() const {
    return bandwidth_reports_sent_.load(std::memory_order_relaxed);
  }

  /// The repair's counters, see sfu::LossRepair.
  [[nodiscard]] std::uint64_t requests_sent() const { return repair_.requests_sent(); }
  [[nodiscard]] std::uint64_t packets_missing() const { return repair_.packets_missing(); }
  [[nodiscard]] std::uint64_t packets_repaired() const { return repair_.packets_repaired(); }

 private:
  using Clock = std::chrono::steady_clock;

  /// Once per interval, turns what the last interval looked like into a target
  /// and sends it.
  void report_bandwidth(const rtc::message_callback& send, Clock::time_point now);

  Options options_;
  LossRepair repair_;

  /// Guards everything the bandwidth estimate touches.
  std::mutex mutex_;
  BandwidthEstimator bandwidth_;
  /// Default constructed, which is the epoch, and that is what marks "no
  /// interval has started yet" in report_bandwidth.
  Clock::time_point last_bandwidth_report_;
  /// The repair's readings at the last report, so that a total becomes an
  /// interval.
  std::uint64_t packets_seen_at_report_ = 0;
  std::uint64_t packets_missing_at_report_ = 0;

  std::atomic<std::uint64_t> bandwidth_reports_sent_{0};
  std::atomic<int> target_kbps_{0};
};

}  // namespace dv::server::sfu
