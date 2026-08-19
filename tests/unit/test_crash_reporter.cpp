// The crash reporter, tested by crashing.
//
// A handler for SIGSEGV cannot be tested by calling it: what has to be proven
// is that it runs when the process is already broken, writes a file from
// inside a signal handler, and then still lets the process die the way it
// would have. So each case forks, breaks the child on purpose, and reads what
// it left behind.

#include <filesystem>
#include <fstream>
#include <string>

#include <gtest/gtest.h>

#include <dv/diagnostics/crash_reporter.hpp>

#if !defined(_WIN32)
#include <csignal>
#include <cstdlib>

#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {

using dv::diagnostics::crash_reports;
using dv::diagnostics::CrashReporterOptions;
using dv::diagnostics::default_crash_directory;
using dv::diagnostics::install_crash_reporter;

class CrashReporterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    directory_ = std::filesystem::temp_directory_path() /
                 ("dv-crash-" + std::to_string(::getpid()) + "-" + std::to_string(++counter_));
    std::filesystem::remove_all(directory_);
  }

  void TearDown() override { std::filesystem::remove_all(directory_); }

  [[nodiscard]] CrashReporterOptions options() const {
    return CrashReporterOptions{.directory = directory_,
                                .application = "partyshare-test",
                                .version = "0.1.0",
                                .keep = 3};
  }

  [[nodiscard]] std::string read(const std::filesystem::path& path) const {
    std::ifstream input(path);
    return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
  }

  std::filesystem::path directory_;
  static inline int counter_ = 0;
};

TEST_F(CrashReporterTest, InstallingCreatesTheDirectory) {
  const auto installed = install_crash_reporter(options());
  ASSERT_TRUE(installed.ok()) << installed.error().message;
  EXPECT_TRUE(std::filesystem::is_directory(directory_));
  EXPECT_EQ(installed.value(), directory_);
}

TEST_F(CrashReporterTest, ADirectoryThatCannotBeCreatedIsAnError) {
  CrashReporterOptions broken = options();
  // A file where the directory should be.
  broken.directory = directory_ / "wall" / "beyond";
  std::filesystem::create_directories(directory_);
  std::ofstream(directory_ / "wall") << "not a directory";

  const auto installed = install_crash_reporter(broken);
  EXPECT_FALSE(installed.ok());
  EXPECT_EQ(installed.error().code, "io_error");
}

TEST_F(CrashReporterTest, TheDefaultDirectoryFollowsTheStateDirectory) {
  const std::filesystem::path state = default_crash_directory("partyshare");
  EXPECT_FALSE(state.empty());
  EXPECT_NE(state.string().find("partyshare"), std::string::npos);
}

TEST_F(CrashReporterTest, ReportsComeBackNewestFirst) {
  std::filesystem::create_directories(directory_);
  for (const char* name : {"100-1.txt", "300-1.txt", "200-1.txt", "notes.log"}) {
    std::ofstream(directory_ / name) << "report";
  }

  const std::vector<std::filesystem::path> found = crash_reports(directory_);
  ASSERT_EQ(found.size(), 3U) << "something other than a report was listed";
  EXPECT_EQ(found[0].filename(), "300-1.txt");
  EXPECT_EQ(found[2].filename(), "100-1.txt");
}

TEST_F(CrashReporterTest, OnlyTheMostRecentReportsAreKept) {
  // A program that crashes in a loop should not fill the disk with the
  // evidence of it.
  std::filesystem::create_directories(directory_);
  for (const char* name : {"100-1.txt", "200-1.txt", "300-1.txt", "400-1.txt", "500-1.txt"}) {
    std::ofstream(directory_ / name) << "report";
  }

  ASSERT_TRUE(install_crash_reporter(options()).ok());

  const std::vector<std::filesystem::path> found = crash_reports(directory_);
  ASSERT_EQ(found.size(), 3U);
  EXPECT_EQ(found[0].filename(), "500-1.txt");
  EXPECT_EQ(found[2].filename(), "300-1.txt");
}

#if !defined(_WIN32)

/// True when this binary was built with AddressSanitizer.
///
/// ASan installs its own SIGSEGV handler and reports the fault itself, then
/// exits without re-raising, so a case that needs a real segmentation fault to
/// reach our handler cannot run under it. The SIGABRT cases still can: ASan
/// leaves abort alone.
constexpr bool built_with_address_sanitizer() {
#if defined(__SANITIZE_ADDRESS__)
  return true;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
  return true;
#else
  return false;
#endif
#else
  return false;
#endif
}

/// Runs `crash` in a child process with the reporter installed, and returns
/// how the child died.
int crash_in_a_child(const CrashReporterOptions& options, void (*crash)()) {
  const pid_t child = ::fork();
  if (child == 0) {
    // gtest's own state is irrelevant from here: this process is going to die.
    if (!install_crash_reporter(options).ok()) {
      ::_exit(1);
    }
    crash();
    ::_exit(2);
  }

  int status = 0;
  ::waitpid(child, &status, 0);
  return status;
}

TEST_F(CrashReporterTest, ASegmentationFaultLeavesAReportBehind) {
  if (built_with_address_sanitizer()) {
    GTEST_SKIP() << "AddressSanitizer reports the fault itself and never lets the handler run";
  }

  const int status = crash_in_a_child(options(), [] {
    // Volatile so that the compiler writes through the pointer instead of
    // deciding that undefined behaviour need not happen at all.
    int* volatile nowhere = nullptr;
    *nowhere = 1;
  });

  EXPECT_TRUE(WIFSIGNALED(status)) << "the child did not die of a signal";
  EXPECT_EQ(WTERMSIG(status), SIGSEGV)
      << "the handler swallowed the crash instead of passing it on";

  const std::vector<std::filesystem::path> found = crash_reports(directory_);
  ASSERT_EQ(found.size(), 1U) << "no crash report was written";

  const std::string report = read(found.front());
  EXPECT_NE(report.find("SIGSEGV"), std::string::npos);
  EXPECT_NE(report.find("partyshare-test"), std::string::npos) << report;
  EXPECT_NE(report.find("version: 0.1.0"), std::string::npos) << report;
  EXPECT_NE(report.find("backtrace:"), std::string::npos) << report;
  // A backtrace of no frames is a report that says nothing.
  EXPECT_NE(report.find("dv_unit_tests"), std::string::npos)
      << "the backtrace named no frame of this program:\n"
      << report;
}

TEST_F(CrashReporterTest, AnAbortLeavesAReportBehind) {
  // Which is how an uncaught exception and a failed assertion both arrive.
  const int status = crash_in_a_child(options(), [] { std::abort(); });

  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGABRT);

  const std::vector<std::filesystem::path> found = crash_reports(directory_);
  ASSERT_EQ(found.size(), 1U);
  EXPECT_NE(read(found.front()).find("SIGABRT"), std::string::npos);
}

TEST_F(CrashReporterTest, TheReportSurvivesACrashInsideTheHandler) {
  if (built_with_address_sanitizer()) {
    GTEST_SKIP() << "AddressSanitizer reports the fault itself and never lets the handler run";
  }

  // The second crash must not produce a second report, and must not recurse:
  // the process has to die instead of spinning in its own handler.
  const int status = crash_in_a_child(options(), [] {
    int* volatile nowhere = nullptr;
    *nowhere = 1;
  });

  EXPECT_TRUE(WIFSIGNALED(status));
  EXPECT_LE(crash_reports(directory_).size(), 1U);
}

#endif  // _WIN32

}  // namespace
