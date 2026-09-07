// The macOS and Linux answer to app/process_usage.hpp: getrusage for the
// processor time on both, and the resident set from where each keeps it -
// task_info on macOS, /proc/self/statm on Linux.
//
// Written on Windows and compiled by the release job's macOS and Linux
// builds; the numbers were checked against the task manager on Windows only.

#include <chrono>
#include <cstdint>
#include <optional>

#include <sys/resource.h>

#include "app/process_usage.hpp"

#if defined(__APPLE__)
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>

#include <unistd.h>
#endif

namespace dv::client::app {
namespace {

[[nodiscard]] double seconds_of(const timeval& time) {
  return static_cast<double>(time.tv_sec) + (static_cast<double>(time.tv_usec) / 1.0e6);
}

/// The resident set, or empty when the platform has no way this file knows.
[[nodiscard]] std::optional<std::uint64_t> resident_bytes() {
#if defined(__APPLE__)
  mach_task_basic_info info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  // task_info wants a task_info_t, which is an int array; the struct is what
  // it fills. The cast is the API's own idiom.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &count) != KERN_SUCCESS) {
    return std::nullopt;
  }
  return static_cast<std::uint64_t>(info.resident_size);
#elif defined(__linux__)
  // Two numbers in pages: the virtual size and the resident set. The second
  // is the one wanted.
  std::ifstream statm("/proc/self/statm");
  std::uint64_t virtual_pages = 0;
  std::uint64_t resident_pages = 0;
  if (!(statm >> virtual_pages >> resident_pages)) {
    return std::nullopt;
  }
  const long page = sysconf(_SC_PAGESIZE);
  if (page <= 0) {
    return std::nullopt;
  }
  return resident_pages * static_cast<std::uint64_t>(page);
#else
  return std::nullopt;
#endif
}

}  // namespace

std::optional<ProcessSnapshot> snapshot_process() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return std::nullopt;
  }
  const std::optional<std::uint64_t> resident = resident_bytes();
  if (!resident) {
    return std::nullopt;
  }

  ProcessSnapshot snapshot;
  snapshot.cpu_seconds = seconds_of(usage.ru_utime) + seconds_of(usage.ru_stime);
  snapshot.at_seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
  snapshot.resident_bytes = *resident;
  return snapshot;
}

}  // namespace dv::client::app
