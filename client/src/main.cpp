#include <cstdio>

#include <dv/config/config.hpp>
#include <dv/logging/logger.hpp>

#include <QApplication>

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

  dv::ui::MainWindow window;
  window.show();

  const int exit_code = QApplication::exec();

  DV_LOG_INFO("Voice Desktop client stopped with code {}", exit_code);
  dv::log::shutdown();
  return exit_code;
}
