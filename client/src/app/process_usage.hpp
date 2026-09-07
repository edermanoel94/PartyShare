#pragma once

#include <cstdint>
#include <optional>

namespace dv::client::app {

/// What the operating system knows about this process at one moment.
///
/// Two of the three fields are running totals and only mean something as the
/// difference between two snapshots; the third is a level and means something
/// on its own. cpu_percent_between does the subtraction.
struct ProcessSnapshot {
  /// The processor time this process has consumed since it started, every
  /// thread on every core added together, in seconds. Kernel and user time
  /// both, because the audio threads spend a good part of theirs in the
  /// kernel and a figure that left that out would say a call costs less than
  /// the task manager says it does.
  double cpu_seconds = 0.0;
  /// A steady clock at the moment of the reading, in seconds. Steady rather
  /// than the wall clock, for the reason every interval in this program is:
  /// a clock that can be set back turns a rate negative.
  double at_seconds = 0.0;
  /// The memory of this process that is resident in RAM, in bytes: the
  /// working set on Windows and the resident set elsewhere. This is the figure
  /// the operating system's own monitors show first, and the one somebody
  /// means when they ask how much memory a program is using.
  std::uint64_t resident_bytes = 0;
};

/// Reads a snapshot from the platform. Empty when it will not say, which is a
/// platform this has not been written for rather than a condition that comes
/// and goes.
///
/// One implementation per platform, chosen by the build:
/// process_usage_windows.cpp and process_usage_posix.cpp.
[[nodiscard]] std::optional<ProcessSnapshot> snapshot_process();

/// How many processors the machine offers, which is what a CPU share is a
/// share of. Never zero: a machine that will not say is counted as one.
[[nodiscard]] unsigned processor_count();

/// The share of the machine's processing this process took between two
/// snapshots, in percent of every core together.
///
/// Every core together, and not one core, because that is how the task
/// managers on all three platforms count it and this figure will be read
/// beside theirs. A process holding one of eight cores reads 12.5 here and in
/// the task manager, rather than 100 here and 12.5 there.
///
/// Empty when the two snapshots are too close together to measure - under a
/// millisecond apart, which is inside the operating system's own rounding of
/// processor time - or when `later` is not later. Clamped to a hundred at the
/// top, because the accounting can run a hair ahead of the clock and a chart
/// that briefly reads 101 percent looks broken rather than precise.
[[nodiscard]] std::optional<double> cpu_percent_between(const ProcessSnapshot& earlier,
                                                        const ProcessSnapshot& later,
                                                        unsigned processors);

/// One reading of what this process costs, as the charts want it.
struct ProcessUsage {
  /// Empty on the first reading, which has no interval behind it. A reading
  /// taken against the process's whole life would open the chart with an
  /// average of everything since launch painted onto its first point.
  std::optional<double> cpu_percent;
  std::uint64_t resident_bytes = 0;
};

/// Takes snapshots and turns each pair into a CPU share.
///
/// Not a static: the dialog that owns one is opened and closed during a call,
/// and a meter that outlived it would measure the first reading of the next
/// opening against the last of the previous one.
class ProcessUsageMeter {
 public:
  explicit ProcessUsageMeter(unsigned processors = processor_count());

  /// One reading. Empty when the platform has no answer at all; otherwise the
  /// memory is always there and the CPU share is there from the second
  /// reading on.
  [[nodiscard]] std::optional<ProcessUsage> read();

  /// Forgets the last snapshot, so the next reading starts a fresh interval.
  void reset() noexcept;

 private:
  unsigned processors_;
  std::optional<ProcessSnapshot> last_;
};

}  // namespace dv::client::app
