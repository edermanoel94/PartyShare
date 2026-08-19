// What the SFU asks a screen share to aim for.
//
// The arithmetic is separated from the RTCP so that it can be tested by
// stepping it through intervals, which is the only way to say anything about a
// control loop: one reading proves nothing, and what matters is whether the
// sequence converges, how fast it backs off, and whether it settles.

#include <cstdint>

#include <gtest/gtest.h>

#include "sfu/bandwidth_estimator.hpp"

namespace {

using dv::server::sfu::BandwidthEstimator;

/// One second of a 30 FPS screen share at the given loss, as packet counts.
struct Interval {
  std::uint64_t received;
  std::uint64_t lost;
};

[[nodiscard]] Interval with_loss(double fraction, std::uint64_t total = 200) {
  const auto lost = static_cast<std::uint64_t>(static_cast<double>(total) * fraction);
  return Interval{total - lost, lost};
}

[[nodiscard]] int after(BandwidthEstimator& estimator, const Interval& interval, int seconds) {
  int target = estimator.target_kbps();
  for (int second = 0; second < seconds; ++second) {
    target = estimator.update(interval.received, interval.lost);
  }
  return target;
}

TEST(BandwidthEstimatorTest, ItStartsWhereItWasToldTo) {
  const BandwidthEstimator estimator{{.start_kbps = 1500}};
  EXPECT_EQ(estimator.target_kbps(), 1500);
}

TEST(BandwidthEstimatorTest, AStartOutsideTheRangeIsPulledIntoIt) {
  EXPECT_EQ(BandwidthEstimator({.start_kbps = 9000, .max_kbps = 3000}).target_kbps(), 3000);
  EXPECT_EQ(BandwidthEstimator({.start_kbps = 10, .min_kbps = 300}).target_kbps(), 300);
}

TEST(BandwidthEstimatorTest, ACleanLinkGrowsToTheCeilingAndStops) {
  BandwidthEstimator estimator{{.start_kbps = 300, .min_kbps = 300, .max_kbps = 3000}};
  // Eight percent a second from 300 kbps reaches 3 Mbps in about thirty
  // seconds, which is the probing behaviour this is meant to have: quick
  // enough that a screen share sharpens while somebody is still looking at it.
  EXPECT_GT(after(estimator, with_loss(0.0), 30), 2900);
  EXPECT_EQ(after(estimator, with_loss(0.0), 10), 3000) << "it grew past its ceiling";
}

TEST(BandwidthEstimatorTest, HeavyLossBringsTheTargetDown) {
  BandwidthEstimator estimator{{.start_kbps = 3000, .min_kbps = 300, .max_kbps = 3000}};
  const int after_one = estimator.update(with_loss(0.20).received, with_loss(0.20).lost);
  EXPECT_LT(after_one, 3000);
  // Half the loss fraction: 20% loss takes a tenth off.
  EXPECT_NEAR(after_one, 2700, 30);
}

TEST(BandwidthEstimatorTest, SustainedLossConvergesOnTheFloorRatherThanZero) {
  BandwidthEstimator estimator{{.start_kbps = 3000, .min_kbps = 300, .max_kbps = 3000}};
  EXPECT_EQ(after(estimator, with_loss(0.50), 100), 300);
}

TEST(BandwidthEstimatorTest, LossInTheMiddleBandChangesNothing) {
  // Between 2% and 10% the link is neither congested nor clearly free, and a
  // controller that reacted there would oscillate on noise.
  BandwidthEstimator estimator{{.start_kbps = 1500}};
  EXPECT_EQ(after(estimator, with_loss(0.05), 20), 1500);
}

TEST(BandwidthEstimatorTest, BackingOffIsFasterThanProbing) {
  // The asymmetry is the whole point: congestion has to be answered quickly
  // and space has to be reclaimed slowly, or the loop spends its life
  // overshooting.
  BandwidthEstimator down{{.start_kbps = 3000, .min_kbps = 300, .max_kbps = 3000}};
  const int fell_in_five = 3000 - after(down, with_loss(0.30), 5);

  BandwidthEstimator up{{.start_kbps = 3000 - fell_in_five, .min_kbps = 300, .max_kbps = 3000}};
  const int rose_in_five = after(up, with_loss(0.0), 5) - (3000 - fell_in_five);

  EXPECT_GT(fell_in_five, rose_in_five);
}

TEST(BandwidthEstimatorTest, AnIntervalWithNoPacketsLeavesTheTargetAlone) {
  // A share that stopped says nothing about the link it stopped on, and
  // treating it as a clean interval would grow the target while no evidence
  // was arriving.
  BandwidthEstimator estimator{{.start_kbps = 1500}};
  EXPECT_EQ(after(estimator, Interval{0, 0}, 10), 1500);
}

TEST(BandwidthEstimatorTest, TheSlowestViewerCapsWhatTheSharerIsAskedFor) {
  BandwidthEstimator estimator{{.start_kbps = 3000, .min_kbps = 300, .max_kbps = 3000}};
  estimator.set_ceiling(800);
  EXPECT_EQ(estimator.target_kbps(), 800);

  // The estimate underneath is untouched, so lifting the cap restores it
  // rather than making it climb again from where the cap was.
  estimator.set_ceiling(0);
  EXPECT_EQ(estimator.target_kbps(), 3000);
}

TEST(BandwidthEstimatorTest, ACeilingBelowTheFloorIsStillTheFloor) {
  // A viewer that says it can take 50 kbps is a viewer that cannot watch a
  // screen share at all, and starving everybody else does not help them.
  BandwidthEstimator estimator{{.start_kbps = 1500, .min_kbps = 300}};
  estimator.set_ceiling(50);
  EXPECT_EQ(estimator.target_kbps(), 300);
}

}  // namespace
