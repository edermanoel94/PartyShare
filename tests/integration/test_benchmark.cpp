// The numbers section 22 of SPEC.md asks for, measured rather than assumed.
//
// A room of five, all sending audio, one sharing a screen at 1280x720, held
// for half a minute while CPU, memory and the call statistics are sampled.
//
// A target of its own, and labelled `benchmark`, because it takes long enough
// that nobody wants it in the ordinary suite. Run it with:
//
//   ctest --test-dir build/media -L benchmark --output-on-failure
//
// The output is the point: the assertions only guard against the run being
// meaningless, and what goes into docs/11-benchmarks.md is what it prints.

#ifdef _WIN32
// clang-format off
// windows.h has to come first: psapi.h does not include it and uses its types,
// and sorting these two alphabetically is what breaks the build.
#include <windows.h>

#include <psapi.h>
// clang-format on
#elif defined(__APPLE__)
#include <mach/mach.h>
#include <sys/resource.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>

#include "media/network_impairment.hpp"
#include "media_test_client.hpp"

namespace {

/// Section 22 of SPEC.md.
constexpr std::size_t kParticipants = 5;

/// How long to hold the call. Half a minute says whether it works; the ten
/// minutes the M6 criterion asks for says whether it keeps working, and is
/// asked for by DV_BENCHMARK_SECONDS rather than paid for on every run.
[[nodiscard]] std::chrono::seconds measure_for() {
  if (const char* seconds = std::getenv("DV_BENCHMARK_SECONDS"); seconds != nullptr) {
    return std::chrono::seconds{std::max(1, std::atoi(seconds))};
  }
  return 30s;
}

// Both numbers below come from the operating system rather than from a timer,
// because what the benchmark reports is what the machine spent, not what the
// test observed. Windows has no /proc, and the Linux implementation does not
// fail there so much as answer zero, which is worse: the assertion at the end
// would report a benchmark that measured nothing as one that found nothing.
//
// macOS has no /proc either, and used to reach the Linux branch through the
// `#else` and print zeroes for half a minute of a real call. It is spelled out
// as its own case for that reason: the guard here is a list of the systems that
// have been thought about, not a Windows exception with everything else assumed
// to be Linux.
#ifdef _WIN32
/// Resident memory of this process, in mebibytes.
[[nodiscard]] double resident_mib() {
  PROCESS_MEMORY_COUNTERS counters{};
  counters.cb = sizeof(counters);
  if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0) {
    return 0;
  }
  return static_cast<double>(counters.WorkingSetSize) / (1024.0 * 1024.0);
}

/// CPU seconds this process has used, user plus system.
[[nodiscard]] double cpu_seconds() {
  FILETIME created{};
  FILETIME exited{};
  FILETIME kernel{};
  FILETIME user{};
  if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user) == 0) {
    return 0;
  }
  // Counts of 100 nanosecond intervals, split across two 32 bit halves because
  // the struct predates a 64 bit integer being something an API could return.
  const auto to_seconds = [](const FILETIME& time) {
    ULARGE_INTEGER value{};
    value.LowPart = time.dwLowDateTime;
    value.HighPart = time.dwHighDateTime;
    return static_cast<double>(value.QuadPart) / 1e7;
  };
  return to_seconds(kernel) + to_seconds(user);
}
#elif defined(__APPLE__)
/// Resident memory of this process, in mebibytes.
[[nodiscard]] double resident_mib() {
  // MACH_TASK_BASIC_INFO rather than TASK_BASIC_INFO: the older flavour holds
  // its sizes in a natural_t, which is 32 bits, and a client of five holding a
  // room is comfortably able to pass what that can count.
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &count) != KERN_SUCCESS) {
    return 0;
  }
  return static_cast<double>(info.resident_size) / (1024.0 * 1024.0);
}

/// CPU seconds this process has used, user plus system.
[[nodiscard]] double cpu_seconds() {
  // getrusage rather than task_info: the task flavours report the threads that
  // are still alive, and half a minute of a call is half a minute of encoder
  // and network threads being created and joined. Their time is exactly the
  // time this benchmark exists to count.
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    return 0;
  }
  const auto to_seconds = [](const timeval& time) {
    return static_cast<double>(time.tv_sec) + static_cast<double>(time.tv_usec) / 1e6;
  };
  return to_seconds(usage.ru_utime) + to_seconds(usage.ru_stime);
}
#else
/// Resident memory of this process, in mebibytes.
[[nodiscard]] double resident_mib() {
  std::ifstream status("/proc/self/status");
  std::string label;
  while (status >> label) {
    if (label == "VmRSS:") {
      double kib = 0;
      status >> kib;
      return kib / 1024.0;
    }
    status.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
  }
  return 0;
}

/// CPU seconds this process has used, user plus system.
[[nodiscard]] double cpu_seconds() {
  std::ifstream stat("/proc/self/stat");
  std::string field;
  for (int i = 0; i < 13 && stat >> field; ++i) {
  }
  long user = 0;
  long system = 0;
  stat >> user >> system;
  const auto ticks = static_cast<double>(sysconf(_SC_CLK_TCK));
  return static_cast<double>(user + system) / ticks;
}
#endif

class BenchmarkTest : public MediaEndToEndTest {
 protected:
  void TearDown() override {
    dv::client::media::set_network_impairment({});
    MediaEndToEndTest::TearDown();
  }

  /// Holds a full room for the measurement window and prints what it cost.
  ///
  /// The impairment, if any, is switched on only once everyone is connected
  /// and the screen is arriving, so that what is measured is a call in flow
  /// meeting a bad network rather than a call that had to be set up over one.
  void run(const char* title, const dv::client::media::NetworkImpairment& impairment);
};

TEST_F(BenchmarkTest, FiveParticipantsWithOneSharingAScreen) {
  run("five participants, one sharing 1280x720", {});
}

TEST_F(BenchmarkTest, FiveParticipantsOnALinkLosingFivePercentOfPackets) {
  // The acceptance criterion of section 22 of SPEC.md, at the size the section
  // sizes a room at. The loss is injected into every client in this process,
  // which is harsher than one participant on a bad connection: here everybody
  // is on one.
  run("five participants on a link losing 5% of packets", {.loss = 0.05});
}

void BenchmarkTest::run(const char* title, const dv::client::media::NetworkImpairment& impairment) {
  if (!dv::client::video::screen_capture_is_available()) {
    GTEST_SKIP() << "no display server attached, so there is no screen to share";
  }

  const std::array<const char*, kParticipants> names{"ana", "bruno", "carla", "diego", "elena"};

  Client& host = add(names[0]);
  ASSERT_TRUE(host.login());
  const std::string room = host.create_room();
  ASSERT_FALSE(room.empty());
  ASSERT_TRUE(host.join(room));
  ASSERT_TRUE(host.wait_until_in_call());

  std::vector<Client*> everyone{&host};
  for (std::size_t i = 1; i < kParticipants; ++i) {
    Client& guest = add(names[i]);
    ASSERT_TRUE(guest.login()) << names[i];
    ASSERT_TRUE(guest.join(room)) << names[i];
    ASSERT_TRUE(guest.wait_until_in_call()) << names[i];
    everyone.push_back(&guest);
  }

  auto* router = server_->media_router();
  ASSERT_TRUE(wait_until([&] { return router->session_count() == kParticipants; }));
  for (Client* client : everyone) {
    ASSERT_TRUE(wait_until([&] {
      return router->outbound_track_count(client->session().local_user().id) == kParticipants - 1;
    })) << "a participant is missing tracks";
  }

  ASSERT_TRUE(host.session().start_screen_share("").ok());

  // Everyone else has to be receiving the screen before the clock starts, so
  // that what is measured is a call in full flow.
  for (std::size_t i = 1; i < kParticipants; ++i) {
    ASSERT_TRUE(wait_until([&] { return everyone[i]->remote_frames() > 0; }, 30000ms))
        << names[i] << " never saw the shared screen";
  }

  dv::client::media::reset_network_impairment_counters();
  dv::client::media::set_network_impairment(impairment);

  const double cpu_before = cpu_seconds();
  const auto started_at = std::chrono::steady_clock::now();
  std::vector<std::uint64_t> frames_before;
  frames_before.reserve(everyone.size());
  for (Client* client : everyone) {
    frames_before.push_back(client->remote_frames());
  }

  std::this_thread::sleep_for(measure_for());

  const double elapsed =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - started_at).count();
  const double cpu = cpu_seconds() - cpu_before;
  const double rss = resident_mib();

  std::printf("\n--- %s ---\n", title);
  std::printf("measured over            %.1f s\n", elapsed);
  std::printf("CPU, all %zu clients      %.1f%% of one core\n", kParticipants,
              100.0 * cpu / elapsed);
  std::printf("CPU, per client          %.1f%% of one core\n",
              100.0 * cpu / elapsed / static_cast<double>(kParticipants));
  std::printf("memory, all %zu clients   %.0f MiB\n", kParticipants, rss);
  std::printf("memory, per client       %.0f MiB\n", rss / static_cast<double>(kParticipants));

  for (std::size_t i = 0; i < everyone.size(); ++i) {
    const dv::client::media::AudioStats audio = everyone[i]->session().stats();
    const std::uint64_t frames = everyone[i]->remote_frames() - frames_before[i];
    const dv::client::video::Size size = everyone[i]->remote_frame_size();
    std::printf("%-6s rtt %4.0f ms  jitter %4.1f ms  perdidos %-5llu  %5.1f fps  %dx%d\n", names[i],
                audio.round_trip_time_ms, audio.jitter_ms,
                static_cast<unsigned long long>(audio.packets_lost),
                static_cast<double>(frames) / elapsed, size.width, size.height);
  }
  std::printf("SFU: %llu audio packets forwarded, %llu video\n",
              static_cast<unsigned long long>(router->audio_packets_forwarded()),
              static_cast<unsigned long long>(router->video_packets_forwarded()));

  if (!impairment.inert()) {
    const auto injected = dv::client::media::network_impairment_counters();
    const auto repair = router->video_repair_stats();
    std::printf("injected: %llu of %llu packets dropped out, %llu of %llu in\n",
                static_cast<unsigned long long>(injected.packets_dropped_outbound),
                static_cast<unsigned long long>(injected.packets_sent),
                static_cast<unsigned long long>(injected.packets_dropped_inbound),
                static_cast<unsigned long long>(injected.packets_received));
    std::printf("repair:   %llu requests, %llu video packets missing, %llu recovered\n",
                static_cast<unsigned long long>(repair.requests_sent),
                static_cast<unsigned long long>(repair.packets_missing),
                static_cast<unsigned long long>(repair.packets_repaired));
  }
  std::fflush(stdout);

  // The assertions guard against a run that measured nothing, not against the
  // numbers themselves. Judging a target belongs in docs/11-benchmarks.md, where
  // the machine it ran on is written down next to it.
  for (std::size_t i = 1; i < kParticipants; ++i) {
    const std::uint64_t frames = everyone[i]->remote_frames() - frames_before[i];
    EXPECT_GT(frames, 0U) << names[i] << " stopped receiving the screen during the measurement";
  }
  EXPECT_GT(router->audio_packets_forwarded(), 0U);
  EXPECT_GT(rss, 0.0);
}

}  // namespace
