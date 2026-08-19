#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <functional>
#include <mutex>
#include <string_view>
#include <system_error>

#include <dv/diagnostics/crash_reporter.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <csignal>

#include <execinfo.h>
#include <fcntl.h>
#include <unistd.h>
#endif

namespace dv::diagnostics {
namespace {

/// Everything the signal handler is allowed to touch.
///
/// A handler runs between two instructions of a program that has already gone
/// wrong. Almost nothing is safe to call there: not malloc, not printf, not
/// anything that takes a lock the crashing thread might already hold. So
/// everything that needs allocating is prepared here, at install time, and the
/// handler only writes bytes that already exist.
///
/// POSIX calls what is allowed "async-signal-safe", and the list is short:
/// open, write, close, _exit, raise, time. This file stays inside it.
struct PreparedReport {
  static constexpr std::size_t kPathCapacity = 512;
  static constexpr std::size_t kHeaderCapacity = 512;

  std::array<char, kPathCapacity> directory{};
  std::size_t directory_length = 0;
  std::array<char, kHeaderCapacity> header{};
  std::size_t header_length = 0;
  std::atomic<bool> installed{false};
  /// One crash is enough. A handler that crashes again would recurse until the
  /// stack ran out, and the second report would describe the handler rather
  /// than the bug.
  std::atomic<bool> handled{false};
};

PreparedReport& prepared() {
  static PreparedReport report;
  return report;
}

#ifndef _WIN32

/// write(2) until it has taken everything, because a short write is not an
/// error and dropping the rest would truncate the report.
void write_all(int descriptor, const char* data, std::size_t size) {
  while (size > 0) {
    const ssize_t written = ::write(descriptor, data, size);
    if (written <= 0) {
      return;
    }
    data += written;
    size -= static_cast<std::size_t>(written);
  }
}

void write_text(int descriptor, const char* text) {
  write_all(descriptor, text, std::strlen(text));
}

/// Formats an unsigned number without touching the standard library, which is
/// not available to a signal handler.
std::size_t format_number(std::uint64_t value, char* buffer, std::size_t capacity) {
  // Enough for the largest unsigned 64 bit number, which is twenty digits.
  std::array<char, 24> digits{};
  std::size_t length = 0;
  if (value == 0) {
    digits[length++] = '0';
  }
  while (value != 0 && length < digits.size()) {
    digits[length++] = static_cast<char>('0' + (value % 10));
    value /= 10;
  }

  // Produced least significant digit first, so it comes out reversed.
  const std::size_t written = std::min(length, capacity);
  for (std::size_t i = 0; i < written; ++i) {
    buffer[i] = digits[length - 1 - i];
  }
  return written;
}

const char* name_of(int signal_number) {
  switch (signal_number) {
    case SIGSEGV:
      return "SIGSEGV, a read or write through a bad pointer";
    case SIGABRT:
      return "SIGABRT, something called abort: an assertion, or an exception nobody caught";
    case SIGBUS:
      return "SIGBUS, a misaligned or unmapped access";
    case SIGFPE:
      return "SIGFPE, an arithmetic fault";
    case SIGILL:
      return "SIGILL, an illegal instruction";
    default:
      return "an unexpected signal";
  }
}

/// Builds "<directory>/<seconds>-<pid>.txt" in place, with no allocation.
std::size_t build_path(char* buffer, std::size_t capacity) {
  const PreparedReport& report = prepared();
  std::size_t length = 0;

  const std::size_t directory = std::min(report.directory_length, capacity);
  std::memcpy(buffer, report.directory.data(), directory);
  length += directory;

  if (length < capacity) {
    buffer[length++] = '/';
  }
  length += format_number(static_cast<std::uint64_t>(::time(nullptr)), buffer + length,
                          capacity - length);
  if (length < capacity) {
    buffer[length++] = '-';
  }
  length +=
      format_number(static_cast<std::uint64_t>(::getpid()), buffer + length, capacity - length);

  static constexpr std::string_view kSuffix = ".txt";
  const std::size_t suffix = std::min(kSuffix.size(), capacity - length);
  std::memcpy(buffer + length, kSuffix.data(), suffix);
  length += suffix;

  if (length < capacity) {
    buffer[length] = '\0';
  }
  return length;
}

/// extern "C" because it is called from the C runtime, which also means the
/// name is global whatever namespace it sits in - hence the prefix, so that a
/// program with its own signal handler still links.
extern "C" void dv_diagnostics_handle_crash_signal(int signal_number) {
  PreparedReport& report = prepared();
  if (report.handled.exchange(true)) {
    // Already reporting. Let the default handler finish the job.
    ::signal(signal_number, SIG_DFL);
    ::raise(signal_number);
    return;
  }

  std::array<char, PreparedReport::kPathCapacity + 64> path{};
  build_path(path.data(), path.size() - 1);

  // open is variadic because of the mode argument, and there is no other way
  // to create a file with permissions from a signal handler.
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
  const int descriptor = ::open(path.data(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
  if (descriptor >= 0) {
    write_all(descriptor, report.header.data(), report.header_length);

    // Seconds since the epoch rather than a date: turning one into the other
    // needs localtime, which takes a lock and allocates, and neither is
    // allowed here. `date -d @<seconds>` finishes the job.
    std::array<char, 24> when{};
    const std::size_t when_length =
        format_number(static_cast<std::uint64_t>(::time(nullptr)), when.data(), when.size());
    write_text(descriptor, "when: ");
    write_all(descriptor, when.data(), when_length);
    write_text(descriptor, " seconds since the epoch, readable with: date -d @");
    write_all(descriptor, when.data(), when_length);
    write_text(descriptor, "\n");

    write_text(descriptor, "signal: ");
    write_text(descriptor, name_of(signal_number));
    write_text(descriptor, "\n\nbacktrace:\n");

    // backtrace_symbols_fd writes straight to the descriptor and allocates
    // nothing, which is the whole reason it exists next to backtrace_symbols.
    std::array<void*, 64> frames{};
    const int depth = ::backtrace(frames.data(), static_cast<int>(frames.size()));
    ::backtrace_symbols_fd(frames.data(), depth, descriptor);

    // The offset in brackets, not the absolute address: every binary here is
    // position independent, so the address depends on where the loader put it
    // and the offset does not.
    write_text(descriptor,
               "\nEach line is binary(+offset) [address]. Turn an offset into a file and a "
               "line with:\n"
               "  addr2line -Cfe <binary> <offset>\n"
               "Names from a library come out mangled, because demangling allocates and a "
               "signal handler may not. Pipe this file through c++filt.\n");
    ::close(descriptor);
  }

  // Nothing here decides the process's fate: the default handler does, so a
  // core dump still happens and the exit status still says what killed it.
  ::signal(signal_number, SIG_DFL);
  ::raise(signal_number);
}

void install_handlers() {
  for (const int signal_number : {SIGSEGV, SIGABRT, SIGBUS, SIGFPE, SIGILL}) {
    struct sigaction action = {};
    action.sa_handler = dv_diagnostics_handle_crash_signal;
    // SA_ONSTACK so that a stack overflow, which is the most common way to get
    // a SIGSEGV, still has somewhere to run the handler. SA_RESETHAND is not
    // used because the handler resets the disposition itself, after writing.
    action.sa_flags = SA_ONSTACK | SA_RESTART;
    sigemptyset(&action.sa_mask);
    ::sigaction(signal_number, &action, nullptr);
  }
}

#else  // _WIN32

void install_handlers() {
  // Windows delivers a crash as a structured exception rather than a signal,
  // and the report worth writing there is a minidump through dbghelp. Neither
  // exists here yet: this project has never been built or run on Windows, and
  // a handler written blind would be a handler nobody has seen run. The seam
  // is this function.
}

#endif  // _WIN32

/// The report's fixed part, rendered once so the handler has nothing to build.
void prepare_header(const CrashReporterOptions& options, const std::filesystem::path& directory) {
  PreparedReport& report = prepared();

  const std::string path = directory.string();
  report.directory_length = std::min(path.size(), PreparedReport::kPathCapacity - 1);
  std::memcpy(report.directory.data(), path.data(), report.directory_length);

  std::string header = "desktop-voice crash report\n";
  header += "application: " + options.application + "\n";
  header += "version: " + options.version + "\n";
  header += "built: " __DATE__ " " __TIME__ "\n";
  header += "\n";

  report.header_length = std::min(header.size(), PreparedReport::kHeaderCapacity - 1);
  std::memcpy(report.header.data(), header.data(), report.header_length);
}

/// Deletes the oldest reports beyond `keep`.
void prune(const std::filesystem::path& directory, int keep) {
  if (keep <= 0) {
    return;
  }
  const std::vector<std::filesystem::path> existing = crash_reports(directory);
  for (auto i = static_cast<std::size_t>(keep); i < existing.size(); ++i) {
    std::error_code ignored;
    std::filesystem::remove(existing[i], ignored);
  }
}

}  // namespace

Result<std::filesystem::path> install_crash_reporter(const CrashReporterOptions& options) {
  std::filesystem::path directory = options.directory;
  if (directory.empty()) {
    directory = default_crash_directory(options.application);
  }

  std::error_code error;
  std::filesystem::create_directories(directory, error);
  if (error && !std::filesystem::is_directory(directory)) {
    return Result<std::filesystem::path>::failure(
        "io_error", "could not create the crash report directory " + directory.string() + ": " +
                        error.message());
  }

  prepare_header(options, directory);
  prune(directory, options.keep);

  PreparedReport& report = prepared();
  if (!report.installed.exchange(true)) {
    install_handlers();
  }

  // An exception nobody caught is not a signal, and by the time terminate runs
  // the stack is intact and the standard library is usable, so this one can
  // afford to say what the exception was before abort turns it into a SIGABRT
  // that the handler above will report.
  static std::terminate_handler previous = nullptr;
  static std::once_flag once;
  std::call_once(once, [] {
    previous = std::set_terminate([] {
      static constexpr std::string_view kMessage =
          "\ndesktop-voice: terminating on an uncaught exception\n";
      write_all(STDERR_FILENO, kMessage.data(), kMessage.size());
      if (previous != nullptr) {
        previous();
      }
      std::abort();
    });
  });

  return directory;
}

std::vector<std::filesystem::path> crash_reports(const std::filesystem::path& directory) {
  std::vector<std::filesystem::path> found;
  std::error_code error;
  if (!std::filesystem::is_directory(directory, error)) {
    return found;
  }

  for (const auto& entry : std::filesystem::directory_iterator(directory, error)) {
    if (entry.is_regular_file() && entry.path().extension() == ".txt") {
      found.push_back(entry.path());
    }
  }

  // Newest first, by name, which is the timestamp the handler wrote.
  std::ranges::sort(found, std::greater<>());
  return found;
}

std::filesystem::path default_crash_directory(const std::string& application) {
  const auto from_environment = [](const char* name) -> std::filesystem::path {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? std::filesystem::path(value)
                                              : std::filesystem::path();
  };

#ifdef _WIN32
  std::filesystem::path base = from_environment("LOCALAPPDATA");
  if (base.empty()) {
    base = std::filesystem::temp_directory_path();
  }
  return base / application / "crashes";
#elif defined(__APPLE__)
  std::filesystem::path home = from_environment("HOME");
  if (home.empty()) {
    home = std::filesystem::temp_directory_path();
  }
  return home / "Library" / "Logs" / application;
#else
  std::filesystem::path base = from_environment("XDG_STATE_HOME");
  if (base.empty()) {
    const std::filesystem::path home = from_environment("HOME");
    base = home.empty() ? std::filesystem::temp_directory_path() : home / ".local" / "state";
  }
  return base / application / "crashes";
#endif
}

}  // namespace dv::diagnostics
