#include <chrono>
#include <cstdio>

#include <dv/config/config.hpp>
#include <dv/diagnostics/crash_reporter.hpp>
#include <dv/logging/logger.hpp>

#include <QApplication>

#include "app/call_session.hpp"
#include "media/media_session.hpp"
#include "ui/main_window.hpp"

int main(int argc, char* argv[]) {
  // Section 22 of SPEC.md asks for a startup under three seconds. Measuring it
  // from the first line of main is the closest this can get to what a person
  // experiences: everything before it belongs to the loader.
  const auto started_at = std::chrono::steady_clock::now();

  auto config_result = dv::config::load(argc, argv);
  if (!config_result) {
    // Logging is not up yet, so this is the one place that writes to stderr
    // directly.
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

  // Before anything that could crash, and after logging so that a failure to
  // install has somewhere to be said.
  if (config.logging.crash_reports) {
    const auto installed = dv::diagnostics::install_crash_reporter({
        .directory = config.logging.crash_directory,
        .application = "desktop-voice",
        .version = DV_VERSION,
    });
    if (installed.ok()) {
      DV_LOG_INFO("Crash reports: {}", installed.value().string());
    } else {
      DV_LOG_WARN("Crash reports are off: {}", installed.error().message);
    }
  }

  DV_LOG_INFO("Voice Desktop client starting");
  DV_LOG_INFO("Signaling server: {}", config.network.signaling_url);
  DV_LOG_INFO("Video: {}x{} @ {} FPS, codec {}", config.video.width, config.video.height,
              config.video.fps, config.video.codec);
  DV_LOG_INFO("Audio: {} Hz, {} channel(s), {} ms frames", config.audio.sample_rate_hz,
              config.audio.channels, config.audio.frame_duration_ms);

  QApplication application(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Voice Desktop"));
  QApplication::setApplicationVersion(QStringLiteral(DV_VERSION));

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
  session_options.media.capture.max_size = {config.video.width, config.video.height};
  session_options.media.capture.max_fps = config.video.fps;
  session_options.media.video_min_bitrate_kbps = config.video.min_bitrate_kbps;
  session_options.media.video_max_bitrate_kbps = config.video.max_bitrate_kbps;
  session_options.media.video_floor_bitrate_kbps = config.video.floor_bitrate_kbps;

  if (!dv::client::media::media_is_available()) {
    DV_LOG_WARN(
        "This build has no media layer, so calls will have no audio. "
        "Rebuild with -DDV_BUILD_CLIENT_MEDIA=ON, see docs/build.md.");
  }

  // Declared before the window and destroyed after it: the window installs
  // callbacks into the session, and removes them in its destructor.
  dv::client::app::CallSession session(session_options);

  dv::ui::MainWindow window(session);
  window.show();

  const auto startup_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_at);
  DV_LOG_INFO("Voice Desktop client ready in {} ms", startup_ms.count());

  const int exit_code = QApplication::exec();

  DV_LOG_INFO("Voice Desktop client stopped with code {}", exit_code);
  dv::log::shutdown();
  return exit_code;
}
