// The network quality indicator, which is arithmetic over the call statistics.

#include <gtest/gtest.h>

#include "app/network_quality.hpp"

namespace {

using dv::client::app::NetworkQuality;
using dv::client::app::quality_of;
using dv::client::media::AudioStats;

[[nodiscard]] AudioStats healthy() {
  AudioStats stats;
  stats.packets_received = 1000;
  stats.packets_sent = 1000;
  stats.round_trip_time_ms = 20;
  stats.jitter_ms = 2;
  stats.packets_lost = 0;
  return stats;
}

TEST(NetworkQualityTest, ACallWithNoNumbersYetIsUnknownRatherThanGood) {
  // A fresh call has zero of everything. Calling that good would light the
  // indicator green before a single packet had arrived.
  EXPECT_EQ(quality_of(AudioStats{}), NetworkQuality::Unknown);
}

TEST(NetworkQualityTest, ALocalCallIsGood) {
  EXPECT_EQ(quality_of(healthy()), NetworkQuality::Good);
}

TEST(NetworkQualityTest, LatencyAloneCanSpoilIt) {
  AudioStats stats = healthy();
  stats.round_trip_time_ms = 200;
  EXPECT_EQ(quality_of(stats), NetworkQuality::Fair);

  stats.round_trip_time_ms = 400;
  EXPECT_EQ(quality_of(stats), NetworkQuality::Poor);
}

TEST(NetworkQualityTest, JitterAloneCanSpoilIt) {
  AudioStats stats = healthy();
  stats.jitter_ms = 20;
  EXPECT_EQ(quality_of(stats), NetworkQuality::Fair);

  stats.jitter_ms = 50;
  EXPECT_EQ(quality_of(stats), NetworkQuality::Poor);
}

TEST(NetworkQualityTest, LossAloneCanSpoilIt) {
  AudioStats stats = healthy();
  stats.packets_lost = 20;  // 20 of 1020
  EXPECT_EQ(quality_of(stats), NetworkQuality::Fair);

  stats.packets_lost = 200;  // 200 of 1200
  EXPECT_EQ(quality_of(stats), NetworkQuality::Poor);
}

TEST(NetworkQualityTest, TheWorstOfTheThreeDecides) {
  // A call with perfect latency and a fifth of its packets missing is not a
  // good call, however good the other two numbers look.
  AudioStats stats = healthy();
  stats.round_trip_time_ms = 1;
  stats.jitter_ms = 0;
  stats.packets_lost = 250;
  EXPECT_EQ(quality_of(stats), NetworkQuality::Poor);
}

TEST(NetworkQualityTest, LossIsMeasuredAgainstWhatWasExpected) {
  // Ten lost out of ten thousand is nothing; ten lost out of twenty is a call
  // nobody can hold. The same absolute number, two different answers.
  AudioStats small = healthy();
  small.packets_received = 10;
  small.packets_lost = 10;
  EXPECT_EQ(quality_of(small), NetworkQuality::Poor);

  AudioStats large = healthy();
  large.packets_received = 10000;
  large.packets_lost = 10;
  EXPECT_EQ(quality_of(large), NetworkQuality::Good);
}

TEST(NetworkQualityTest, SendingWithNothingComingBackIsStillMeasured) {
  // One participant alone in a room sends and receives nothing. That is not
  // "no data yet", and the indicator should not sit on Unknown forever.
  AudioStats stats;
  stats.packets_sent = 500;
  EXPECT_NE(quality_of(stats), NetworkQuality::Unknown);
}

}  // namespace
