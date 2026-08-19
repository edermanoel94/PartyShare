#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include <dv/config/config.hpp>
#include <dv/logging/logger.hpp>

#include "signaling/server.hpp"

namespace {

std::atomic<bool> g_stop_requested{false};

extern "C" void handle_signal(int) {
  g_stop_requested = true;
}

/// Loads the development account list.
///
/// This is a stopgap so the MVP has users at all. It keeps passwords in plain
/// text on disk, which section 17 of SPEC.md rules out for anything real, and
/// it has to be replaced by a proper user store before deployment.
int load_users(dv::server::SignalingServer& server, const std::string& path) {
  std::ifstream input(path);
  if (!input.is_open()) {
    DV_LOG_WARN("No users file at {}, nobody will be able to authenticate", path);
    return 0;
  }

  std::ostringstream buffer;
  buffer << input.rdbuf();
  const nlohmann::json root = nlohmann::json::parse(buffer.str(), nullptr, false);
  if (root.is_discarded() || !root.is_array()) {
    DV_LOG_ERROR("Users file {} is not a JSON array", path);
    return 0;
  }

  int loaded = 0;
  for (const auto& entry : root) {
    if (!entry.is_object() || !entry.contains("username") || !entry.contains("password")) {
      DV_LOG_WARN("Skipping a malformed entry in {}", path);
      continue;
    }
    const auto username = entry.at("username").get<std::string>();
    const auto password = entry.at("password").get<std::string>();
    const auto display_name = entry.contains("display_name")
                                  ? entry.at("display_name").get<std::string>()
                                  : std::string{};

    auto user = server.add_user(username, password, display_name);
    if (!user) {
      DV_LOG_WARN("Could not register '{}': {}", username, user.error().message);
      continue;
    }
    ++loaded;
  }

  DV_LOG_WARN(
      "Loaded {} development account(s) from {} with plain text passwords. "
      "Replace this with a real user store before deploying.",
      loaded, path);
  return loaded;
}

/// libdatachannel takes credentials inside the URL, as turn:user:password@host.
/// Splitting them out in the configuration keeps the password out of a field
/// that ends up in logs.
[[nodiscard]] std::string build_turn_url(const dv::config::NetworkConfig& network) {
  if (network.turn_username.empty()) {
    return network.turn_url;
  }

  const std::size_t scheme_end = network.turn_url.find("://");
  const std::string scheme =
      scheme_end == std::string::npos ? "turn://" : network.turn_url.substr(0, scheme_end + 3);
  const std::string host =
      scheme_end == std::string::npos ? network.turn_url : network.turn_url.substr(scheme_end + 3);

  return scheme + network.turn_username + ":" + network.turn_password + "@" + host;
}

}  // namespace

int main(int argc, char* argv[]) {
  auto config_result = dv::config::load(argc, argv);
  if (!config_result) {
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

  DV_LOG_INFO("Voice Desktop signaling server starting");

  dv::server::SignalingServer::Options options;
  options.bind_address = config.server.bind_address;
  options.port = config.server.port;
  options.hub.max_participants_per_room = config.server.max_participants_per_room;
  options.hub.heartbeat_interval = std::chrono::milliseconds(config.server.heartbeat_interval_ms);
  options.hub.heartbeat_timeout = std::chrono::milliseconds(config.server.heartbeat_timeout_ms);

  // ICE for the SFU's own connections. TURN only matters once a participant is
  // behind a NAT that STUN cannot get through, which is why it is optional.
  options.sfu.ice_servers = config.network.stun_servers;
  if (!config.network.turn_url.empty()) {
    options.sfu.ice_servers.push_back(build_turn_url(config.network));
  }

  dv::server::SignalingServer server(options);

  if (!config.server.users_file.empty()) {
    (void)load_users(server, config.server.users_file);
  } else {
    DV_LOG_WARN("server.users_file is not set, nobody will be able to authenticate");
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  server.start();
  DV_LOG_INFO("Max participants per room: {}", config.server.max_participants_per_room);
  DV_LOG_INFO("Media routing: {}, {} ICE server(s)", options.enable_sfu ? "on" : "off",
              options.sfu.ice_servers.size());
  DV_LOG_INFO("Press Ctrl+C to stop");

  while (!g_stop_requested) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  DV_LOG_INFO("Shutdown requested");
  server.stop();
  dv::log::shutdown();
  return 0;
}
