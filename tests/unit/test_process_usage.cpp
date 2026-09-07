// What this process costs the machine, as the metrics window charts it: the
// arithmetic that turns two snapshots into a share of the processors, and the
// platform reading behind it.

#include <chrono>
#include <cstdint>
#include <optional>

#include <gtest/gtest.h>

#include "app/process_usage.hpp"

namespace {

using dv::client::app::cpu_percent_between;
using dv::client::app::ProcessSnapshot;
using dv::client::app::ProcessUsage;
using dv::client::app::ProcessUsageMeter;
using dv::client::app::snapshot_process;

[[nodiscard]] ProcessSnapshot at(double at_seconds, double cpu_seconds) {
  ProcessSnapshot snapshot;
  snapshot.at_seconds = at_seconds;
  snapshot.cpu_seconds = cpu_seconds;
  return snapshot;
}

TEST(ProcessUsageTest, TheShareIsOfEveryCoreTogether) {
  // Half a second of processor time in one second of wall clock is half a
  // core, and on a four core machine that is an eighth of the machine.
  const auto share = cpu_percent_between(at(10.0, 3.0), at(11.0, 3.5), 4);
  ASSERT_TRUE(share.has_value());
  EXPECT_DOUBLE_EQ(*share, 12.5);
}

TEST(ProcessUsageTest, AWholeMachineIsAHundred) {
  const auto share = cpu_percent_between(at(0.0, 0.0), at(2.0, 8.0), 4);
  ASSERT_TRUE(share.has_value());
  EXPECT_DOUBLE_EQ(*share, 100.0);
}

TEST(ProcessUsageTest, TheAccountingRunningAheadOfTheClockIsClampedNotShown) {
  // Processor time is charged in scheduler ticks, so over a short interval it
  // can add up to more than the interval. A chart reading 104 percent looks
  // broken rather than precise.
  const auto share = cpu_percent_between(at(0.0, 0.0), at(0.1, 0.416), 4);
  ASSERT_TRUE(share.has_value());
  EXPECT_DOUBLE_EQ(*share, 100.0);
}

TEST(ProcessUsageTest, SnapshotsTooCloseToMeasureGiveNoFigure) {
  EXPECT_FALSE(cpu_percent_between(at(5.0, 1.0), at(5.0005, 1.0), 4).has_value());
}

TEST(ProcessUsageTest, SnapshotsOutOfOrderGiveNoFigure) {
  EXPECT_FALSE(cpu_percent_between(at(6.0, 1.0), at(5.0, 1.5), 4).has_value());
  // And processor time going backwards is two processes, not a negative load.
  EXPECT_FALSE(cpu_percent_between(at(5.0, 2.0), at(6.0, 1.0), 4).has_value());
}

TEST(ProcessUsageTest, NoProcessorsAreCountedAsOne) {
  const auto share = cpu_percent_between(at(0.0, 0.0), at(1.0, 0.25), 0);
  ASSERT_TRUE(share.has_value());
  EXPECT_DOUBLE_EQ(*share, 25.0);
}

TEST(ProcessUsageTest, ThePlatformAnswersForThisProcess) {
  // The one test that reaches the operating system. It cannot know what the
  // right numbers are, only what shape they have: a process that is running
  // tests has some memory resident and has used some processor time.
  const std::optional<ProcessSnapshot> snapshot = snapshot_process();
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_GT(snapshot->resident_bytes, 0U);
  EXPECT_GE(snapshot->cpu_seconds, 0.0);
  EXPECT_GT(snapshot->at_seconds, 0.0);
}

TEST(ProcessUsageTest, TheFirstReadingHasMemoryButNoShare) {
  ProcessUsageMeter meter(4);
  const std::optional<ProcessUsage> first = meter.read();
  ASSERT_TRUE(first.has_value());
  EXPECT_GT(first->resident_bytes, 0U);
  EXPECT_FALSE(first->cpu_percent.has_value());
}

TEST(ProcessUsageTest, TheSecondReadingHasAShareOfTheMachine) {
  ProcessUsageMeter meter;
  (void)meter.read();
  // Long enough that the two snapshots are certainly apart, and busy enough
  // that a processor was used, so the figure is a measurement rather than a
  // rounding of nothing.
  const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(30);
  volatile std::uint64_t sink = 0;
  while (std::chrono::steady_clock::now() < until) {
    sink = sink + 1;
  }
  const std::optional<ProcessUsage> second = meter.read();
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(second->cpu_percent.has_value());
  EXPECT_GE(*second->cpu_percent, 0.0);
  EXPECT_LE(*second->cpu_percent, 100.0);
}

TEST(ProcessUsageTest, ResetStartsTheIntervalAgain) {
  ProcessUsageMeter meter(4);
  (void)meter.read();
  meter.reset();
  const std::optional<ProcessUsage> after = meter.read();
  ASSERT_TRUE(after.has_value());
  EXPECT_FALSE(after->cpu_percent.has_value());
}

}  // namespace
