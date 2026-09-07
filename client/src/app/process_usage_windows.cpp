// The Windows answer to app/process_usage.hpp: GetProcessTimes for the
// processor time and GetProcessMemoryInfo for the working set. Both are
// kernel32 and psapi, which every Windows has; nothing is downloaded for this.

#include "app/process_usage.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <chrono>
#include <cstdint>
#include <optional>

// windows.h first: psapi.h uses its types and does not include it. Regrouped
// and sorted, clang-format would put psapi.h ahead, so it is told to leave
// these two alone.
// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on

namespace dv::client::app {
namespace {

/// A FILETIME as the 64-bit count of hundred-nanosecond units it is.
[[nodiscard]] std::uint64_t ticks_of(const FILETIME& time) {
  ULARGE_INTEGER value;
  value.LowPart = time.dwLowDateTime;
  value.HighPart = time.dwHighDateTime;
  return value.QuadPart;
}

/// Hundred-nanosecond units in a second.
constexpr double kTicksPerSecond = 1.0e7;

}  // namespace

std::optional<ProcessSnapshot> snapshot_process() {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == 0) {
    return std::nullopt;
  }

  // The plain counters and not the _EX ones: the working set is in both, and
  // the plain struct is what the function is declared to take, so nothing has
  // to be cast on the way in.
  PROCESS_MEMORY_COUNTERS memory{};
  memory.cb = sizeof(memory);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &memory, sizeof(memory)) == 0) {
    return std::nullopt;
  }

  ProcessSnapshot snapshot;
  snapshot.cpu_seconds = static_cast<double>(ticks_of(kernel) + ticks_of(user)) / kTicksPerSecond;
  snapshot.at_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  snapshot.resident_bytes = static_cast<std::uint64_t>(memory.WorkingSetSize);
  return snapshot;
}

}  // namespace dv::client::app
