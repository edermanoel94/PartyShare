#include <cstdio>

#include <dv/config/config.hpp>
#include <dv/logging/logger.hpp>

#include <QApplication>

#include "app/call_session.hpp"
#include "audio/audio_session.hpp"
#include "ui/main_window.hpp"

int main(int argc, char* argv[]) {
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

  DV_LOG_INFO("Voice Desktop client starting");
  DV_LOG_INFO("Signaling server: {}", config.network.signaling_url);
  DV_LOG_INFO("Video: {}x{} @ {} FPS, codec {}", config.video.width, config.video.height,
              config.video.fps, config.video.codec);
  DV_LOG_INFO("Audio: {} Hz, {} channel(s), {} ms frames", config.audio.sample_rate_hz,
              config.audio.channels, config.audio.frame_duration_ms);

  QApplication application(argc, argv);
  QApplication::setApplicationName(QStringLiteral("Voice Desktop"));
  QApplication::setApplicationVersion(QStringLiteral("0.1.0"));

  dv::client::app::CallSession::Options session_options;
  session_options.signaling_url = config.network.signaling_url;
  session_options.audio.ice_servers = config.network.stun_servers;
  if (!config.network.turn_url.empty()) {
    session_options.audio.ice_servers.push_back(config.network.turn_url);
    session_options.audio.turn_username = config.network.turn_username;
    session_options.audio.turn_password = config.network.turn_password;
  }
  session_options.audio.sample_rate_hz = config.audio.sample_rate_hz;
  session_options.audio.channels = config.audio.channels;
  session_options.audio.frame_duration_ms = config.audio.frame_duration_ms;
  session_options.audio.echo_cancellation = config.audio.echo_cancellation;
  session_options.audio.noise_suppression = config.audio.noise_suppression;
  session_options.audio.automatic_gain_control = config.audio.automatic_gain_control;
  session_options.audio.input_device = config.audio.input_device;
  session_options.audio.output_device = config.audio.output_device;

  if (!dv::client::audio::media_is_available()) {
    DV_LOG_WARN(
        "This build has no media layer, so calls will have no audio. "
        "Rebuild with -DDV_BUILD_CLIENT_MEDIA=ON, see docs/build.md.");
  }

  // Declared before the window and destroyed after it: the window installs
  // callbacks into the session, and removes them in its destructor.
  dv::client::app::CallSession session(session_options);

  dv::ui::MainWindow window(session);
  window.show();

  const int exit_code = QApplication::exec();

  DV_LOG_INFO("Voice Desktop client stopped with code {}", exit_code);
  dv::log::shutdown();
  return exit_code;
}
