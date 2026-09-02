#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <system_error>

#include <dv/config/config.hpp>
#include <dv/diagnostics/crash_reporter.hpp>
#include <dv/logging/logger.hpp>

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QIcon>

#include "app/call_session.hpp"
#include "audio/loopback_capturer.hpp"
#include "media/media_session.hpp"
#include "ui/chimes.hpp"
#include "ui/main_window.hpp"
#include "ui/theme.hpp"
#include "video/screen_quality.hpp"

namespace {

/// Whether the arguments ask for the usage text.
///
/// Checked before the configuration is loaded, so that --help works on a
/// machine where the configuration is broken. It cannot go through the normal
/// parser either: that one only understands --key=value, and a bare --help
/// would be dropped without a word.
///
/// The array is what main() is handed, and there is no other shape for it.
/// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
[[nodiscard]] bool wants_help(int argc, const char* const argv[]) {
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    if (argument == "--help" || argument == "-h") {
      return true;
    }
  }
  return false;
}

/// Prints what the client accepts.
///
/// Only the options that reach the client are listed. The parser is shared with
/// the server and understands more, but naming an option here that does nothing
/// to a client is worse than not naming it.
void print_usage() {
  // Straight to stdout: help is what was asked for, not a diagnostic, and the
  // logger is not configured yet.
  std::fputs("partyshare " DV_VERSION
             "\n"
             "The PartyShare desktop client.\n"
             "\n"
             "Usage:\n"
             "  partyshare [--key=value ...]\n"
             "\n"
             "Every option takes the --key=value form. A bare --key is ignored.\n"
             "\n"
             "Options:\n"
             "  --config=PATH            Configuration file, .ini or .json. Replaces the\n"
             "                           two below rather than joining them.\n"
             "  --signaling-url=URL      Signaling server to connect to\n"
             "  --input-device=NAME      Microphone to capture from\n"
             "  --output-device=NAME     Speakers to play through\n"
             "  --codec=NAME             Video codec for the screen share\n"
             "  --fps=N                  Screen capture rate, 1 to 120\n"
             "  --log-level=LEVEL        trace, debug, info, warn, error, fatal or off\n"
             "  --log-file=PATH          Also write the log to this file\n"
             "  -h, --help               Print this text and exit\n"
             "\n"
             "Qt reads its own options from this same command line, so anything not\n"
             "listed here is passed on to Qt rather than refused.\n"
             "\n"
             "Configuration precedence, lowest first: built-in defaults, config.ini\n"
             "beside this executable, config.ini under the user's own configuration\n"
             "directory, environment variables prefixed with DV_, then these options.\n"
             "\n"
             "The two files are found without being named, and neither has to exist.\n"
             "The first is the machine's, written by whoever installed it; the second\n"
             "is this user's, and needs no administrator to edit. Where they are on\n"
             "this machine is printed at startup, with --log-level=info.\n"
             "\n"
             "To point the client at a server, one line is enough:\n"
             "\n"
             "  [network]\n"
             "  signaling_url = ws://192.168.1.10:8080\n",
             stdout);
}

/// Puts a config.ini under this user's own configuration directory, the first
/// time the client ever runs.
///
/// So that the file exists before anybody goes looking for it. Every line in it
/// is commented out, so it changes nothing on its own; what it does is turn
/// "where do I put the address of the server" into a file that is already there
/// with the answer written above the line to uncomment.
///
/// This one and not the one beside the executable, which is where an installer
/// puts its copy: that directory is Program Files, a /usr prefix or the inside
/// of a signed .app, and on all three a client running as the person at the
/// keyboard cannot write to it. This one it can, which is also why it is where
/// the settings dialog saves the audio devices.
///
/// Returns what to say about it, or nothing when the file was already there.
/// It runs before the logger exists, because the configuration it creates has
/// to be in place before the configuration is read.
[[nodiscard]] std::string ensure_user_config() {
  const std::filesystem::path file = dv::config::user_config_file();
  if (file.empty()) {
    return "This system does not say where a user's configuration belongs, so none was created.";
  }

  std::error_code failed;
  if (std::filesystem::exists(file, failed)) {
    return {};
  }

  // From the executable's own copy of assets/config.ini rather than from the
  // installed one, which may be anywhere or nowhere.
  QFile source(QStringLiteral(":/config.ini"));
  if (!source.open(QIODevice::ReadOnly)) {
    return "Could not read the built in configuration template, so " + file.string() +
           " was not created.";
  }
  const QByteArray text = source.readAll();

  std::filesystem::create_directories(file.parent_path(), failed);
  if (failed) {
    return "Could not create " + file.parent_path().string() + ": " + failed.message();
  }

  std::ofstream output(file, std::ios::binary);
  output.write(text.constData(), static_cast<std::streamsize>(text.size()));
  output.flush();
  if (!output) {
    return "Could not write " + file.string();
  }
  return "Created " + file.string();
}

/// The whole of the client, so that main() can be nothing but the guard that
/// stops an exception from escaping into std::terminate.
///
/// The array is what main() is handed, and there is no other shape for it.
/// NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
int run(int argc, char* argv[]) {
  // Section 22 of SPEC.md asks for a startup under three seconds. Measuring it
  // from the first line of main is the closest this can get to what a person
  // experiences: everything before it belongs to the loader.
  const auto started_at = std::chrono::steady_clock::now();

  if (wants_help(argc, argv)) {
    print_usage();
    return 0;
  }

  // Before the configuration is loaded rather than after, so that the file this
  // creates is one of the files that get read, and the log below says "read"
  // about a file that was.
  const std::string created_config = ensure_user_config();

  // Ignore, not Reject: Qt takes its own options from this command line, so an
  // argument this parser does not know is not an argument nobody knows.
  auto config_result = dv::config::load(argc, argv, dv::config::UnknownOptions::Ignore);
  if (!config_result) {
    // Logging is not up yet, so this is the one place that writes to stderr
    // directly.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    std::fprintf(stderr, "configuration error [%s]: %s\n", config_result.error().code.c_str(),
                 config_result.error().message.c_str());
    return 1;
  }
  const dv::config::Config config = std::move(config_result).take();

  dv::log::init({
      .level = dv::log::level_from_string(config.logging.level),
      .file_path = config.logging.file_path,
      .log_to_console = config.logging.log_to_console,
  });

  // Which files were and were not read, every time.
  //
  // This is the one thing that turns "I put the address in and it still
  // connects to localhost" from a support conversation into a glance at the
  // log. The path is printed whether or not the file is there, because half of
  // those reports are a file written one directory away from the one that is
  // read, and a path that says "missing" answers that immediately.
  //
  // config_files and not default_config_paths: an explicit --config replaces
  // the cascade, and a log naming files that were not read would send the
  // reader to edit the wrong one.
  if (!created_config.empty()) {
    DV_LOG_INFO("{}", created_config);
  }
  for (const std::filesystem::path& path : dv::config::config_files(argc, argv)) {
    std::error_code failed;
    const bool present = std::filesystem::is_regular_file(path, failed);
    DV_LOG_INFO("Configuration file {}: {}", present ? "read" : "missing", path.string());
  }
  // What the files added up to is logged a few lines below, with the rest of
  // the effective configuration. Saying it twice would only invite the two
  // lines to disagree one day.

  // Before anything that could crash, and after logging so that a failure to
  // install has somewhere to be said.
  if (config.logging.crash_reports) {
    const auto installed = dv::diagnostics::install_crash_reporter({
        .directory = config.logging.crash_directory,
        .application = "partyshare",
        .version = DV_VERSION,
    });
    if (installed.ok()) {
      DV_LOG_INFO("Crash reports: {}", installed.value().string());
    } else {
      DV_LOG_WARN("Crash reports are off: {}", installed.error().message);
    }
  }

  DV_LOG_INFO("PartyShare client starting");
  DV_LOG_INFO("Signaling server: {}", config.network.signaling_url);
  DV_LOG_INFO("Video: {}x{} @ {} FPS, codec {}", config.video.width, config.video.height,
              config.video.fps, config.video.codec);
  // Worked out here and used everywhere below, rather than read from the
  // config in each place that wants it. In automatic mode the range is a
  // function of the size and rate rather than a value in the file, and deriving
  // it once is what stops the log and the session from disagreeing about which
  // range is in force.
  const dv::client::video::BitrateRange video_bitrate =
      config.video.auto_bitrate
          ? dv::client::video::recommended_bitrate_kbps(
                {.width = config.video.width, .height = config.video.height}, config.video.fps,
                config.video.floor_bitrate_kbps)
          : dv::client::video::BitrateRange{.min_kbps = config.video.min_bitrate_kbps,
                                            .max_kbps = config.video.max_bitrate_kbps};

  // The bitrate belongs in the log for the same reason the devices below do:
  // the settings dialog writes it to config.ini, so this is the line that says
  // whether what was chosen came back. Which mode produced it is on the same
  // line, because "the bitrate I set is not the one in the log" has exactly one
  // answer and this is it.
  DV_LOG_INFO("Video bitrate: {} to {} kbps, floor {} kbps, {}", video_bitrate.min_kbps,
              video_bitrate.max_kbps, config.video.floor_bitrate_kbps,
              config.video.auto_bitrate ? "automatic from the size and rate above" : "as chosen");
  DV_LOG_INFO("Audio: {} Hz, {} channel(s), {} ms frames", config.audio.sample_rate_hz,
              config.audio.channels, config.audio.frame_duration_ms);
  // Which devices, and not only the format. This is what the settings dialog
  // writes to config.ini, so it is also the line that answers "it is not using
  // the microphone I picked" without anybody having to find the file first.
  DV_LOG_INFO(
      "Audio devices: capture {}, playback {}",
      config.audio.input_device.empty() ? "the system default" : config.audio.input_device,
      config.audio.output_device.empty() ? "the system default" : config.audio.output_device);

  QApplication application(argc, argv);
  QApplication::setApplicationName(QStringLiteral("PartyShare"));
  QApplication::setApplicationVersion(QStringLiteral(DV_VERSION));

  // From the resources rather than from the executable's own icon. On Windows
  // the .rc gives Explorer and the taskbar a picture without any of this, but
  // that one is not reachable as a QIcon - and ui::Notifier needs one, because
  // the icon it puts in the notification area is what its balloon hangs off.
  // Four sizes, so that neither the tray nor a title bar has to scale a
  // drawing meant for the other.
  QIcon icon;
  for (const int size : {16, 32, 48, 256}) {
    icon.addFile(QStringLiteral(":/partyshare-%1.png").arg(size));
  }
  QApplication::setWindowIcon(icon);

  // Before the first widget exists. A palette installed afterwards reaches
  // every widget only because Qt re-polishes them, and the ones that read a
  // colour in their constructor would already have read the wrong one.
  dv::ui::theme::apply(application);

  // Before the first room can be joined, which is the only thing that would
  // ring it. The settings dialog moves this again while the program runs; the
  // file is only where it starts.
  dv::ui::set_chimes_enabled(config.ui.room_sounds);

  dv::client::app::CallSession::Options session_options;
  session_options.signaling_url = config.network.signaling_url;
  session_options.media.ice_servers = config.network.stun_servers;
  if (!config.network.turn_url.empty()) {
    session_options.media.ice_servers.push_back(config.network.turn_url);
    session_options.media.turn_username = config.network.turn_username;
    session_options.media.turn_password = config.network.turn_password;
  }
  session_options.media.sample_rate_hz = config.audio.sample_rate_hz;
  session_options.media.channels = config.audio.channels;
  session_options.media.frame_duration_ms = config.audio.frame_duration_ms;
  session_options.media.echo_cancellation = config.audio.echo_cancellation;
  session_options.media.noise_suppression = config.audio.noise_suppression;
  session_options.media.automatic_gain_control = config.audio.automatic_gain_control;
  session_options.media.input_device = config.audio.input_device;
  session_options.media.output_device = config.audio.output_device;
  // The video section of the configuration, which until M8 was parsed, logged
  // and then ignored: the session used its own defaults, so setting a bitrate
  // or a resolution in the file changed nothing.
  session_options.media.capture.max_size = {.width = config.video.width,
                                            .height = config.video.height};
  session_options.media.capture.max_fps = config.video.fps;
  session_options.auto_bitrate = config.video.auto_bitrate;
  session_options.media.video_min_bitrate_kbps = video_bitrate.min_kbps;
  session_options.media.video_max_bitrate_kbps = video_bitrate.max_kbps;
  session_options.media.video_floor_bitrate_kbps = config.video.floor_bitrate_kbps;
  // The configuration says what to share; this says what the machine can. The
  // default in ScreenAudioConfig is "system", and it is the right default for
  // the platform that has the feature, but only Windows does: see
  // docs/09-screen-audio.md, section 8. Carried unchanged onto a Mac it made
  // every single share end with "Sharing without sound: capturing what an
  // application is playing is only implemented on Windows" in a status bar
  // that fits about a third of that sentence - a warning about a feature the
  // settings dialog has already greyed out, in a message nobody can finish
  // reading.
  const bool screen_sound_possible = dv::client::audio::loopback_capture_is_available();
  session_options.screen_audio_mode =
      screen_sound_possible ? dv::client::app::screen_audio_mode_from(config.screen_audio.mode)
                            : dv::client::app::ScreenAudio::Mode::None;
  session_options.media.screen_audio_volume_percent = config.screen_audio.volume_percent;

  // Said once at startup rather than on every share, and only to somebody who
  // asked for sound: a file that already says "none" is getting what it asked
  // for and has nothing to be told.
  if (!screen_sound_possible && dv::client::app::screen_audio_mode_from(config.screen_audio.mode) !=
                                    dv::client::app::ScreenAudio::Mode::None) {
    DV_LOG_INFO(
        "Screen sound: this system cannot capture what an application is playing, so a shared "
        "screen carries the microphone only. See docs/09-screen-audio.md, section 8.");
  }

  if (!dv::client::media::media_is_available()) {
    DV_LOG_WARN(
        "This build has no media layer, so calls will have no audio. "
        "Rebuild with -DDV_BUILD_CLIENT_MEDIA=ON, see docs/02-build.md.");
  }

  // Declared before the window and destroyed after it, which is what lets
  // ~MainWindow take its handlers back and stop the session's threads while
  // there is still a session to stop. The other order would destroy the
  // session first and leave the window's destructor talking to nothing.
  dv::client::app::CallSession session(session_options);

  dv::ui::MainWindow window(session);
  window.show();

  const auto startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_at);
  DV_LOG_INFO("PartyShare client ready in {} ms", startup_ms.count());

  const int exit_code = QApplication::exec();

  DV_LOG_INFO("PartyShare client stopped with code {}", exit_code);
  dv::log::shutdown();
  return exit_code;
}

}  // namespace

int main(int argc, char* argv[]) {
  // Nothing is allowed to leave main. An exception escaping here is a
  // std::terminate with no message, and a client that vanishes without one
  // tells the person nothing about what happened.
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    // The logger may not exist yet, or may be the thing that failed.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-vararg)
    std::fprintf(stderr, "fatal: %s\n", error.what());
    return 1;
  }
}
