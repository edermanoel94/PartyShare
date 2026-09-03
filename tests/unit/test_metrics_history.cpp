// The model behind the metrics charts: the window of readings, and the step
// that turns WebRTC's running totals into the rate a chart can draw.

#include <gtest/gtest.h>

#include "app/metrics_history.hpp"

namespace {

using dv::client::app::MetricsHistory;
using dv::client::app::nice_ceiling;
using dv::client::media::AudioStats;

[[nodiscard]] AudioStats reading(std::uint64_t received, std::uint64_t lost) {
  AudioStats stats;
  stats.packets_received = received;
  stats.packets_lost = lost;
  return stats;
}

TEST(MetricsHistoryTest, AFreshHistoryHasNothingInIt) {
  const MetricsHistory history(60000.0);
  EXPECT_TRUE(history.empty());
}

TEST(MetricsHistoryTest, TheFirstReadingHasNoLossRate) {
  // There is no interval behind it. Deriving one from the running total would
  // open every chart with the whole call's loss painted onto its first point,
  // dated to the moment somebody happened to press the button.
  MetricsHistory history(60000.0);
  history.observe(reading(900, 100), 0.0);

  ASSERT_EQ(history.samples().size(), 1U);
  EXPECT_DOUBLE_EQ(history.samples().front().loss_percent, 0.0);
}

TEST(MetricsHistoryTest, LossIsMeasuredOverTheIntervalRatherThanTheCall) {
  MetricsHistory history(60000.0);
  // A bad first minute: a fifth of everything lost.
  history.observe(reading(800, 200), 0.0);
  // And then a clean interval: a hundred more arrived and none went missing.
  history.observe(reading(900, 200), 200.0);

  ASSERT_EQ(history.samples().size(), 2U);
  EXPECT_DOUBLE_EQ(history.samples().back().loss_percent, 0.0);
}

TEST(MetricsHistoryTest, LossIsTheShareOfWhatTheIntervalExpected) {
  MetricsHistory history(60000.0);
  history.observe(reading(1000, 0), 0.0);
  // Ninety arrived and ten did not, out of the hundred that were expected.
  history.observe(reading(1090, 10), 200.0);

  EXPECT_DOUBLE_EQ(history.samples().back().loss_percent, 10.0);
}

[[nodiscard]] AudioStats heard(std::uint64_t played, std::uint64_t concealed, double buffer_seconds,
                               std::uint64_t emitted) {
  AudioStats stats;
  stats.total_samples_received = played;
  stats.concealed_samples = concealed;
  stats.jitter_buffer_delay_seconds = buffer_seconds;
  stats.jitter_buffer_emitted_count = emitted;
  return stats;
}

TEST(MetricsHistoryTest, ConcealmentIsTheShareOfTheIntervalThatWasInvented) {
  // docs/16-audio-plan.md, step 9. A bad first stretch, then a clean one: the
  // figure is the interval's, not the call's, like the loss above.
  MetricsHistory history(60000.0);
  history.observe(heard(48000, 4800, 1.0, 48000), 0.0);
  EXPECT_DOUBLE_EQ(history.samples().back().concealment_percent, 0.0)
      << "the first reading has nothing to compare against";

  // 480 more invented out of 48000 more played: one percent.
  history.observe(heard(96000, 5280, 2.0, 96000), 1000.0);
  EXPECT_DOUBLE_EQ(history.samples().back().concealment_percent, 1.0);

  // Nothing invented in the next stretch.
  history.observe(heard(144000, 5280, 3.0, 144000), 2000.0);
  EXPECT_DOUBLE_EQ(history.samples().back().concealment_percent, 0.0);
}

TEST(MetricsHistoryTest, TheJitterBufferDepthIsTheIntervalsAverageWait) {
  MetricsHistory history(60000.0);
  history.observe(heard(48000, 0, 1.0, 48000), 0.0);
  // libwebrtc adds every emitted sample's own wait to the total, so 48000
  // samples that each waited 50 ms add 2400 seconds to it.
  history.observe(heard(96000, 0, 1.0 + 2400.0, 96000), 1000.0);
  EXPECT_DOUBLE_EQ(history.samples().back().jitter_buffer_ms, 50.0);
}

TEST(MetricsHistoryTest, TheJitterBufferCountersGoingBackwardsAreASecondCallToo) {
  MetricsHistory history(60000.0);
  history.observe(heard(96000, 960, 2.0, 96000), 0.0);
  // A new peer connection starts every counter from zero again.
  history.observe(heard(48000, 100, 0.5, 48000), 1000.0);
  EXPECT_DOUBLE_EQ(history.samples().back().concealment_percent, 0.0);
  EXPECT_DOUBLE_EQ(history.samples().back().jitter_buffer_ms, 0.0);
}

TEST(MetricsHistoryTest, AnIntervalWithNoPacketsInItIsNotLoss) {
  // Nobody spoke, or the call was still setting up. Zero of zero expected is
  // not a hundred percent lost, and it is not a division either.
  MetricsHistory history(60000.0);
  history.observe(reading(500, 0), 0.0);
  history.observe(reading(500, 0), 200.0);

  EXPECT_DOUBLE_EQ(history.samples().back().loss_percent, 0.0);
}

TEST(MetricsHistoryTest, CountersGoingBackwardsAreASecondCallAndNotNegativeLoss) {
  // WebRTC starts its counters again from zero for a new peer connection.
  // Subtracting across that boundary on unsigned counters does not give a
  // negative number, it gives a figure with an exponent in it.
  MetricsHistory history(60000.0);
  history.observe(reading(5000, 50), 0.0);
  history.observe(reading(10, 0), 200.0);

  EXPECT_DOUBLE_EQ(history.samples().back().loss_percent, 0.0);
}

TEST(MetricsHistoryTest, ReadingsOlderThanTheWindowAreDropped) {
  MetricsHistory history(1000.0);
  history.observe(reading(100, 0), 0.0);
  history.observe(reading(200, 0), 500.0);
  history.observe(reading(300, 0), 1600.0);

  // The first is 1600 ms behind the newest, which is outside a window of one
  // second. The second is 1100 ms behind it and goes as well.
  ASSERT_EQ(history.samples().size(), 1U);
  EXPECT_DOUBLE_EQ(history.samples().front().at_ms, 1600.0);
}

TEST(MetricsHistoryTest, TheMeasurementsAreCarriedThroughUntouched) {
  MetricsHistory history(60000.0);
  AudioStats stats = reading(100, 0);
  stats.round_trip_time_ms = 42.0;
  stats.jitter_ms = 6.5;
  stats.send_bitrate_kbps = 48.0;
  stats.receive_bitrate_kbps = 44.0;
  history.observe(stats, 0.0);

  const auto& sample = history.samples().front();
  EXPECT_DOUBLE_EQ(sample.round_trip_time_ms, 42.0);
  EXPECT_DOUBLE_EQ(sample.jitter_ms, 6.5);
  EXPECT_DOUBLE_EQ(sample.send_kbps, 48.0);
  EXPECT_DOUBLE_EQ(sample.receive_kbps, 44.0);
}

TEST(MetricsHistoryTest, ClearingForgetsTheCountersAsWellAsThePoints) {
  // Not just the points. Leaving the counters behind would measure the first
  // reading of the next call against the last reading of the previous one.
  MetricsHistory history(60000.0);
  history.observe(reading(1000, 0), 0.0);
  history.clear();
  EXPECT_TRUE(history.empty());

  history.observe(reading(50, 10), 200.0);
  EXPECT_DOUBLE_EQ(history.samples().front().loss_percent, 0.0);
}

TEST(NiceCeilingTest, StopsAtOneTwoOrFiveTimesAPowerOfTen) {
  EXPECT_DOUBLE_EQ(nice_ceiling(0.4), 0.5);
  EXPECT_DOUBLE_EQ(nice_ceiling(7.0), 10.0);
  EXPECT_DOUBLE_EQ(nice_ceiling(42.0), 50.0);
  EXPECT_DOUBLE_EQ(nice_ceiling(180.0), 200.0);
  EXPECT_DOUBLE_EQ(nice_ceiling(1500.0), 2000.0);
}

TEST(NiceCeilingTest, AlwaysHoldsTheValueItWasGiven) {
  for (const double value : {0.01, 0.3, 1.0, 9.9, 63.0, 640.0, 12345.0}) {
    EXPECT_GE(nice_ceiling(value), value) << "at " << value;
  }
}

TEST(NiceCeilingTest, NothingMeasuredStillLeavesAnAxisToDrawOn) {
  // An axis from zero to zero has no height to plot in, and dividing by it is
  // how a chart of a silent call becomes a chart of infinities.
  EXPECT_GT(nice_ceiling(0.0), 0.0);
  EXPECT_GT(nice_ceiling(-5.0), 0.0);
}

}  // namespace
