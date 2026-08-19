#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include <dv/core/result.hpp>

/// Task 7 of M8.
///
/// A crash that leaves nothing behind is a bug report that says "it closed".
/// What this writes is the smallest thing that turns that into something
/// actionable: which build, which signal, and where in the code it was.
namespace dv::diagnostics {

struct CrashReporterOptions {
  /// Where the reports go. Created if it does not exist.
  std::filesystem::path directory;
  /// Which program crashed, since the client and the server write into
  /// directories of their own but a user may well send either.
  std::string application;
  std::string version;
  /// How many reports to keep. The oldest are deleted on install, so a program
  /// that crashes in a loop does not fill the disk with the evidence.
  int keep = 10;
};

/// Installs the handlers for the signals a crash arrives as, and for an
/// exception nobody caught.
///
/// Safe to call more than once; the last call wins. The handlers do not
/// swallow the crash: they write the report and then let the default handler
/// run, so the process still dies the way it would have, and a core dump is
/// still produced where one was configured.
///
/// Fails with `io_error` when the directory cannot be created, and installs
/// nothing in that case.
[[nodiscard]] Result<std::filesystem::path> install_crash_reporter(
    const CrashReporterOptions& options);

/// The reports in `directory`, newest first.
[[nodiscard]] std::vector<std::filesystem::path> crash_reports(
    const std::filesystem::path& directory);

/// Where reports go when the configuration does not say: the platform's own
/// place for state that is neither cache nor configuration.
///
/// `$XDG_STATE_HOME/partyshare/crashes` on Linux, `~/Library/Logs` on
/// macOS, `%LOCALAPPDATA%` on Windows.
[[nodiscard]] std::filesystem::path default_crash_directory(const std::string& application);

}  // namespace dv::diagnostics
