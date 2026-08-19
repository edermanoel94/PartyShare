#include "app/call_session.hpp"

#include <algorithm>
#include <utility>

#include <dv/logging/logger.hpp>

namespace dv::client::app {
namespace {

/// Sorted by display name so the interface gets a stable order instead of
/// whatever the hash map happens to produce.
[[nodiscard]] std::vector<Participant> sorted(
    const std::unordered_map<std::string, Participant>& participants) {
  std::vector<Participant> result;
  result.reserve(participants.size());
  for (const auto& [id, participant] : participants) {
    result.push_back(participant);
  }
  std::sort(result.begin(), result.end(), [](const Participant& a, const Participant& b) {
    return a.user.display_name == b.user.display_name ? a.user.id < b.user.id
                                                      : a.user.display_name < b.user.display_name;
  });
  return result;
}

}  // namespace

std::string_view to_string(CallSession::State state) noexcept {
  switch (state) {
    case CallSession::State::Idle:
      return "idle";
    case CallSession::State::Connecting:
      return "connecting";
    case CallSession::State::Authenticated:
      return "authenticated";
    case CallSession::State::InCall:
      return "in call";
    case CallSession::State::Failed:
      return "failed";
  }
  return "unknown";
}

CallSession::CallSession(Options options)
    : CallSession(std::move(options), media::create_media_session) {}

CallSession::CallSession(Options options, MediaSessionFactory factory)
    : options_(std::move(options)),
      media_factory_(std::move(factory)),
      signaling_(SignalingClient::Options{options_.signaling_url}) {
  signaling_.on_message([this](protocol::Message message) { handle_signal(std::move(message)); });
  signaling_.on_state([this](SignalingClient::State state, const std::string& detail) {
    handle_signaling_state(state, detail);
  });
}

CallSession::~CallSession() {
  disconnect();
}

void CallSession::on_events(Callbacks callbacks) {
  const std::lock_guard<std::mutex> lock(mutex_);
  callbacks_ = std::move(callbacks);
}

void CallSession::on_room_created(std::function<void(std::string)> handler) {
  const std::lock_guard<std::mutex> lock(mutex_);
  room_created_handler_ = std::move(handler);
}

Result<std::monostate> CallSession::connect_and_authenticate(const std::string& username,
                                                             const std::string& password) {
  if (username.empty() || password.empty()) {
    return Result<std::monostate>::failure("invalid_value",
                                           "username and password are both required");
  }

  {
    const std::lock_guard<std::mutex> lock(mutex_);
    pending_username_ = username;
    pending_password_ = password;
  }

  set_state(State::Connecting, options_.signaling_url);

  if (auto connected = signaling_.connect(); !connected) {
    set_state(State::Failed, connected.error().message);
    return connected;
  }

  if (!running_.exchange(true)) {
    metrics_thread_ = std::thread([this] { metrics_loop(); });
  }
  return std::monostate{};
}

Result<std::monostate> CallSession::create_room(const std::string& room_name) {
  std::string user_id;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    user_id = local_user_.id;
  }
  if (user_id.empty()) {
    return Result<std::monostate>::failure("unauthorized", "authenticate before creating a room");
  }
  return signaling_.send(protocol::CreateRoom{user_id, room_name});
}

Result<std::monostate> CallSession::join(const std::string& room_id,
                                         const std::string& display_name) {
  std::string user_id;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    user_id = local_user_.id;
    room_id_ = room_id;
  }
  if (user_id.empty()) {
    return Result<std::monostate>::failure("unauthorized", "authenticate before joining a room");
  }
  return signaling_.send(protocol::JoinRoom{room_id, user_id, display_name});
}

Result<std::monostate> CallSession::leave() {
  std::string user_id;
  std::string room;
  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    user_id = local_user_.id;
    room = room_id_;
    room_id_.clear();
    participants_.clear();
    session.swap(audio_);
  }
  if (room.empty()) {
    return Result<std::monostate>::failure("not_in_room", "this session is not in a room");
  }

  // Closed outside the lock: it waits for media callbacks, and those take it.
  if (session) {
    session->close();
  }
  publish_participants();
  set_state(State::Authenticated, "left the room");

  return signaling_.send(protocol::LeaveRoom{room, user_id});
}

Result<std::monostate> CallSession::set_muted(bool muted) {
  std::string user_id;
  std::string room;
  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    user_id = local_user_.id;
    room = room_id_;
    muted_ = muted;
    session = audio_;
  }

  if (session) {
    session->set_microphone_muted(muted);
  }
  if (room.empty()) {
    // Muted before joining. The state is kept and announced on the way in.
    return std::monostate{};
  }

  // The server confirms by broadcasting it back, and only then does the
  // interface change: everyone has to agree about who is muted.
  if (muted) {
    return signaling_.send(protocol::Mute{room, user_id});
  }
  return signaling_.send(protocol::Unmute{room, user_id});
}

bool CallSession::muted() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return muted_;
}

Result<std::monostate> CallSession::set_participant_volume(const std::string& user_id,
                                                           double volume) {
  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    volumes_[user_id] = volume;
    session = audio_;
  }
  if (!session) {
    // Kept for when the call starts, which is what makes the setting usable
    // before anyone has spoken.
    return std::monostate{};
  }
  return session->set_participant_volume(user_id, volume);
}

Result<std::monostate> CallSession::set_input_device(const std::string& device_id) {
  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    options_.audio.input_device = device_id;
    session = audio_;
  }
  if (!session) {
    return std::monostate{};
  }
  return session->set_input_device(device_id);
}

Result<std::monostate> CallSession::set_output_device(const std::string& device_id) {
  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    options_.audio.output_device = device_id;
    session = audio_;
  }
  if (!session) {
    return std::monostate{};
  }
  return session->set_output_device(device_id);
}

void CallSession::disconnect() {
  if (running_.exchange(false) && metrics_thread_.joinable()) {
    metrics_thread_.join();
  }

  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    session.swap(audio_);
    participants_.clear();
    room_id_.clear();
  }
  if (session) {
    session->close();
  }

  signaling_.disconnect();
  set_state(State::Idle, "disconnected");
}

CallSession::State CallSession::state() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return state_;
}

models::User CallSession::local_user() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return local_user_;
}

std::string CallSession::room_id() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return room_id_;
}

std::vector<Participant> CallSession::participants() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return sorted(participants_);
}

media::AudioStats CallSession::stats() const {
  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    session = audio_;
  }
  return session ? session->stats() : media::AudioStats{};
}

void CallSession::handle_signaling_state(SignalingClient::State state, const std::string& detail) {
  switch (state) {
    case SignalingClient::State::Connected: {
      std::string username;
      std::string password;
      {
        const std::lock_guard<std::mutex> lock(mutex_);
        username = pending_username_;
        password = pending_password_;
      }
      if (!username.empty()) {
        if (auto sent = signaling_.send(protocol::Authenticate{username, password}); !sent) {
          report(sent.error());
        }
      }
      break;
    }
    case SignalingClient::State::Failed:
      set_state(State::Failed, detail);
      break;
    case SignalingClient::State::Disconnected:
    case SignalingClient::State::Connecting:
      break;
  }
}

void CallSession::handle_signal(protocol::Message message) {
  if (const auto* authenticated = std::get_if<protocol::Authenticated>(&message)) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      local_user_ = authenticated->user;
      // The password is not needed again, and there is no reason to keep it
      // in memory for the rest of the call.
      pending_password_.clear();
    }
    set_state(State::Authenticated, authenticated->user.id);
    return;
  }

  if (const auto* created = std::get_if<protocol::RoomCreated>(&message)) {
    std::function<void(std::string)> handler;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handler = room_created_handler_;
    }
    if (handler) {
      handler(created->room_id);
    }
    return;
  }

  if (const auto* joined = std::get_if<protocol::UserJoined>(&message)) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      Participant& participant = participants_[joined->user.id];
      participant.user = joined->user;
    }
    publish_participants();
    return;
  }

  if (const auto* left = std::get_if<protocol::UserLeft>(&message)) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      participants_.erase(left->user_id);
    }
    publish_participants();
    return;
  }

  if (const auto* muted = std::get_if<protocol::Mute>(&message)) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = participants_.find(muted->user_id); it != participants_.end()) {
        it->second.muted = true;
      }
      if (muted->user_id == local_user_.id) {
        muted_ = true;
      }
    }
    publish_participants();
    return;
  }

  if (const auto* unmuted = std::get_if<protocol::Unmute>(&message)) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = participants_.find(unmuted->user_id); it != participants_.end()) {
        it->second.muted = false;
      }
      if (unmuted->user_id == local_user_.id) {
        muted_ = false;
      }
    }
    publish_participants();
    return;
  }

  if (const auto* sharing = std::get_if<protocol::ScreenShareStarted>(&message)) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = participants_.find(sharing->user_id); it != participants_.end()) {
        it->second.sharing_screen = true;
      }
    }
    publish_participants();
    return;
  }

  if (const auto* stopped = std::get_if<protocol::ScreenShareStopped>(&message)) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = participants_.find(stopped->user_id); it != participants_.end()) {
        it->second.sharing_screen = false;
      }
    }
    publish_participants();
    return;
  }

  if (const auto* offer = std::get_if<protocol::Offer>(&message)) {
    handle_offer(*offer);
    return;
  }

  if (const auto* candidate = std::get_if<protocol::IceCandidate>(&message)) {
    handle_ice_candidate(*candidate);
    return;
  }

  if (const auto* error = std::get_if<protocol::ErrorMessage>(&message)) {
    report(Error{error->code, error->message});
    return;
  }
}

void CallSession::handle_offer(const protocol::Offer& offer) {
  if (offer.from_user_id != protocol::kSfuUserId) {
    // Media goes through the SFU, so an offer from a participant means the
    // other side is speaking a topology this client does not implement.
    DV_LOG_WARN("Ignoring an offer from {}, only the SFU negotiates media", offer.from_user_id);
    return;
  }

  if (auto ready = ensure_media_session(); !ready) {
    report(ready.error());
    return;
  }

  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    session = audio_;
  }
  if (!session) {
    return;
  }

  // Applied outside the lock: the answer comes back on a media thread and
  // reaches straight back into this object.
  if (auto applied = session->apply_remote_offer(offer.sdp); !applied) {
    report(applied.error());
  }
}

void CallSession::handle_ice_candidate(const protocol::IceCandidate& candidate) {
  if (candidate.from_user_id != protocol::kSfuUserId) {
    return;
  }

  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    session = audio_;
  }
  if (!session) {
    // A candidate before the offer is applied. The SFU trickles them, so this
    // is normal on a fast connection, and ICE recovers through the ones that
    // follow.
    DV_LOG_DEBUG("Dropping an ICE candidate that arrived before the media session existed");
    return;
  }

  const media::IceCandidate value{candidate.candidate, candidate.sdp_mid,
                                  candidate.sdp_mline_index};
  if (auto added = session->add_remote_candidate(value); !added) {
    report(added.error());
  }
}

Result<std::monostate> CallSession::ensure_media_session() {
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (audio_) {
      return std::monostate{};
    }
  }

  media::MediaSession::Callbacks callbacks;

  callbacks.on_local_answer = [this](std::string sdp) {
    std::string room;
    std::string user_id;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      room = room_id_;
      user_id = local_user_.id;
    }
    if (auto sent = signaling_.send(
            protocol::Answer{room, user_id, std::string(protocol::kSfuUserId), std::move(sdp)});
        !sent) {
      report(sent.error());
    }
  };

  callbacks.on_local_candidate = [this](media::IceCandidate candidate) {
    std::string room;
    std::string user_id;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      room = room_id_;
      user_id = local_user_.id;
    }
    if (auto sent = signaling_.send(protocol::IceCandidate{
            room, user_id, std::string(protocol::kSfuUserId), std::move(candidate.candidate),
            std::move(candidate.sdp_mid), candidate.sdp_mline_index});
        !sent) {
      report(sent.error());
    }
  };

  callbacks.on_remote_audio = [this](std::string user_id, bool active) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = participants_.find(user_id); it != participants_.end()) {
        it->second.audio_active = active;
      }
    }
    publish_participants();
  };

  callbacks.on_levels = [this](std::vector<media::AudioLevel> levels) {
    Callbacks handlers;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      for (const media::AudioLevel& level : levels) {
        // An empty user id is the local microphone. It is reported on its own
        // below, for the level meter, and also lands on the local participant:
        // a room where everyone is marked as speaking except you reads as a
        // bug even though the meter is right there.
        const std::string& user_id = level.user_id.empty() ? local_user_.id : level.user_id;
        if (const auto it = participants_.find(user_id); it != participants_.end()) {
          it->second.level = level.level;
          it->second.speaking = level.speaking;
        }
      }
      handlers = callbacks_;
    }

    for (const media::AudioLevel& level : levels) {
      if (level.user_id.empty() && handlers.on_local_level) {
        handlers.on_local_level(level.level, level.speaking);
      }
    }
    publish_participants();
  };

  callbacks.on_state = [this](media::MediaState state) {
    switch (state) {
      case media::MediaState::Connected:
        set_state(State::InCall, "media connected");
        break;
      case media::MediaState::Failed:
        set_state(State::Failed, "the media connection failed");
        break;
      case media::MediaState::New:
      case media::MediaState::Connecting:
      case media::MediaState::Disconnected:
      case media::MediaState::Closed:
        break;
    }
  };

  auto created = media_factory_(options_.audio, std::move(callbacks));
  if (!created) {
    return Result<std::monostate>::failure(created.error());
  }

  std::shared_ptr<media::MediaSession> session = std::move(created).take();

  bool muted = false;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    audio_ = session;
    muted = muted_;
  }

  // A participant who muted before joining stays muted, and so do the volumes
  // and devices chosen before the call.
  session->set_microphone_muted(muted);

  std::unordered_map<std::string, double> volumes;
  std::string input_device;
  std::string output_device;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    volumes = volumes_;
    input_device = options_.audio.input_device;
    output_device = options_.audio.output_device;
  }

  for (const auto& [user_id, volume] : volumes) {
    // Failing here only means that participant has no track yet; the media
    // layer keeps the value and applies it when one arrives.
    (void)session->set_participant_volume(user_id, volume);
  }
  if (!input_device.empty()) {
    if (const auto applied = session->set_input_device(input_device); !applied) {
      report(applied.error());
    }
  }
  if (!output_device.empty()) {
    if (const auto applied = session->set_output_device(output_device); !applied) {
      report(applied.error());
    }
  }

  return std::monostate{};
}

void CallSession::set_state(State state, const std::string& detail) {
  Callbacks callbacks;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (state_ == state) {
      return;
    }
    state_ = state;
    callbacks = callbacks_;
  }

  DV_LOG_INFO("Call session: {} ({})", to_string(state), detail);
  if (callbacks.on_state) {
    callbacks.on_state(state, detail);
  }
}

void CallSession::publish_participants() {
  Callbacks callbacks;
  std::vector<Participant> list;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    callbacks = callbacks_;
    list = sorted(participants_);
  }
  if (callbacks.on_participants) {
    callbacks.on_participants(std::move(list));
  }
}

void CallSession::report(const Error& error) {
  Callbacks callbacks;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    callbacks = callbacks_;
  }

  DV_LOG_WARN("Call session error [{}]: {}", error.code, error.message);
  if (callbacks.on_error) {
    callbacks.on_error(error);
  }
}

void CallSession::metrics_loop() {
  // Woken more often than the interval so that disconnect() does not have to
  // wait a whole period for this thread to notice.
  constexpr auto kPollInterval = std::chrono::milliseconds(100);
  auto next_report = std::chrono::steady_clock::now() + options_.metrics_interval;

  while (running_) {
    std::this_thread::sleep_for(kPollInterval);
    if (std::chrono::steady_clock::now() < next_report) {
      continue;
    }
    next_report = std::chrono::steady_clock::now() + options_.metrics_interval;

    std::shared_ptr<media::MediaSession> session;
    Callbacks callbacks;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      session = audio_;
      callbacks = callbacks_;
    }
    if (!session) {
      continue;
    }

    const media::AudioStats stats = session->stats();
    DV_LOG_INFO(
        "Audio: rtt {:.0f} ms, jitter {:.1f} ms, lost {} packets, up {:.0f} kbps, down {:.0f} kbps",
        stats.round_trip_time_ms, stats.jitter_ms, stats.packets_lost, stats.send_bitrate_kbps,
        stats.receive_bitrate_kbps);

    if (callbacks.on_metrics) {
      callbacks.on_metrics(stats);
    }
  }
}

}  // namespace dv::client::app
