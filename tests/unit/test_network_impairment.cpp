// The fault injector's arithmetic, tested without a network.
//
// What the sampler decides is the whole of the impairment: everything else in
// client/src/webrtc/impaired_socket_factory.cpp is plumbing that acts on these
// two answers. A drop rate that is quietly wrong would make every measurement
// taken under it wrong too, and nothing in a call would look unusual.

#include <chrono>
#include <cstdint>

#include <gtest/gtest.h>

#include "media/network_impairment.hpp"

namespace {

using dv::client::media::ImpairmentSampler;
using dv::client::media::NetworkImpairment;
using namespace std::chrono_literals;

constexpr int kDraws = 200000;

[[nodiscard]] double drop_rate(double probability, std::uint64_t seed = 1) {
  ImpairmentSampler sampler{seed};
  int dropped = 0;
  for (int i = 0; i < kDraws; ++i) {
    if (sampler.drops(probability)) {
      ++dropped;
    }
  }
  return static_cast<double>(dropped) / kDraws;
}

TEST(ImpairmentSamplerTest, NoLossMeansNoLoss) {
  EXPECT_EQ(drop_rate(0.0), 0.0);
  EXPECT_EQ(drop_rate(-1.0), 0.0);
}

TEST(ImpairmentSamplerTest, TotalLossMeansEveryPacket) {
  EXPECT_EQ(drop_rate(1.0), 1.0);
  EXPECT_EQ(drop_rate(2.0), 1.0);
}

TEST(ImpairmentSamplerTest, FivePercentIsFivePercent) {
  // The number section 22 of SPEC.md names. Two hundred thousand draws put the
  // standard error near 0.05%, so a tenth of a percent is a wide margin and a
  // real bias would still be caught.
  EXPECT_NEAR(drop_rate(0.05), 0.05, 0.001);
}

TEST(ImpairmentSamplerTest, TheRateHoldsAcrossTheRange) {
  for (const double probability : {0.01, 0.1, 0.25, 0.5, 0.9}) {
    EXPECT_NEAR(drop_rate(probability), probability, 0.005) << "at " << probability;
  }
}

TEST(ImpairmentSamplerTest, TwoSocketsDoNotLoseTheSamePackets) {
  // Each socket seeds its own sampler. Were they to share a sequence, the
  // audio and the video of one call would lose packets in step, which is not
  // what a network does.
  ImpairmentSampler first{1};
  ImpairmentSampler second{2};
  int agreements = 0;
  for (int i = 0; i < 1000; ++i) {
    if (first.drops(0.5) == second.drops(0.5)) {
      ++agreements;
    }
  }
  EXPECT_GT(agreements, 400);
  EXPECT_LT(agreements, 600);
}

TEST(ImpairmentSamplerTest, TheSameSeedRepeatsTheSameRun) {
  // Determinism is what makes a failure under impairment reproducible.
  EXPECT_EQ(drop_rate(0.05, 7), drop_rate(0.05, 7));
  EXPECT_NE(drop_rate(0.05, 7), drop_rate(0.05, 8));
}

TEST(ImpairmentSamplerTest, WithoutJitterTheDelayIsExact) {
  ImpairmentSampler sampler{1};
  const NetworkImpairment impairment{.delay = 40ms};
  for (int i = 0; i < 100; ++i) {
    EXPECT_EQ(sampler.holds(impairment), 40ms);
  }
}

TEST(ImpairmentSamplerTest, JitterStaysInsideItsBand) {
  ImpairmentSampler sampler{1};
  const NetworkImpairment impairment{.delay = 40ms, .jitter = 10ms};

  std::chrono::milliseconds total{0};
  std::chrono::milliseconds lowest = 1h;
  std::chrono::milliseconds highest{0};
  for (int i = 0; i < 10000; ++i) {
    const auto held = sampler.holds(impairment);
    ASSERT_GE(held, 30ms);
    ASSERT_LE(held, 50ms);
    total += held;
    lowest = std::min(lowest, held);
    highest = std::max(highest, held);
  }

  // Uniform around the delay, so the mean is the delay and both ends of the
  // band are reached.
  EXPECT_NEAR(static_cast<double>(total.count()) / 10000.0, 40.0, 0.5);
  EXPECT_LE(lowest, 31ms);
  EXPECT_GE(highest, 49ms);
}

TEST(ImpairmentSamplerTest, JitterNeverHoldsAPacketForLessThanNothing) {
  // Jitter wider than the delay is a legitimate setting, and the answer to
  // "deliver this five milliseconds ago" is to deliver it now.
  ImpairmentSampler sampler{1};
  const NetworkImpairment impairment{.delay = 5ms, .jitter = 50ms};
  for (int i = 0; i < 10000; ++i) {
    ASSERT_GE(sampler.holds(impairment), 0ms);
  }
}

TEST(NetworkImpairmentTest, TheDefaultIsInert) {
  EXPECT_TRUE(NetworkImpairment{}.inert());
  EXPECT_FALSE(NetworkImpairment{.loss = 0.01}.inert());
  EXPECT_FALSE(NetworkImpairment{.delay = 1ms}.inert());
  EXPECT_FALSE(NetworkImpairment{.jitter = 1ms}.inert());
}

TEST(NetworkImpairmentTest, WhatIsSetIsWhatIsRead) {
  dv::client::media::set_network_impairment({.loss = 0.05, .delay = 30ms, .jitter = 7ms});
  const NetworkImpairment read = dv::client::media::network_impairment();
  EXPECT_DOUBLE_EQ(read.loss, 0.05);
  EXPECT_EQ(read.delay, 30ms);
  EXPECT_EQ(read.jitter, 7ms);
  EXPECT_FALSE(read.inert());

  dv::client::media::set_network_impairment({});
  EXPECT_TRUE(dv::client::media::network_impairment().inert());
}

}  // namespace
