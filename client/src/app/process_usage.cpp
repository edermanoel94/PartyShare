#include "app/process_usage.hpp"

#include <algorithm>
#include <thread>

namespace dv::client::app {
namespace {

/// Under this, two snapshots are the same moment as far as the operating
/// system's processor accounting is concerned. Windows charges processor time
/// in units of a scheduler tick, and dividing a tick by a fraction of a
/// millisecond gives a percentage in the thousands.
constexpr double kShortestIntervalSeconds = 0.001;

}  // namespace

unsigned processor_count() {
  return std::max(1U, std::thread::hardware_concurrency());
}

std::optional<double> cpu_percent_between(const ProcessSnapshot& earlier,
                                          const ProcessSnapshot& later, unsigned processors) {
  const double interval = later.at_seconds - earlier.at_seconds;
  if (!(interval >= kShortestIntervalSeconds)) {
    return std::nullopt;
  }
  // Not clamped below zero: processor time is a running total and cannot go
  // down, so a negative difference is two snapshots from two processes, which
  // is a caller's mistake and not a reading of zero.
  const double busy = later.cpu_seconds - earlier.cpu_seconds;
  if (!(busy >= 0.0)) {
    return std::nullopt;
  }
  const double share = 100.0 * busy / (interval * static_cast<double>(std::max(1U, processors)));
  return std::min(100.0, share);
}

ProcessUsageMeter::ProcessUsageMeter(unsigned processors) : processors_(std::max(1U, processors)) {}

std::optional<ProcessUsage> ProcessUsageMeter::read() {
  const std::optional<ProcessSnapshot> now = snapshot_process();
  if (!now) {
    return std::nullopt;
  }

  ProcessUsage usage;
  usage.resident_bytes = now->resident_bytes;
  if (last_) {
    usage.cpu_percent = cpu_percent_between(*last_, *now, processors_);
  }
  // Kept as the earlier snapshot only when it was far enough from the last
  // one to be measured against. Two readings inside a millisecond would
  // otherwise slide the interval's start forward without ever producing a
  // figure, and a caller polling fast could go a long time with no CPU
  // reading at all.
  if (!last_ || usage.cpu_percent) {
    last_ = now;
  }
  return usage;
}

void ProcessUsageMeter::reset() noexcept {
  last_.reset();
}

}  // namespace dv::client::app
