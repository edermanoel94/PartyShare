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
  std::ranges::sort(result, [](const Participant& a, const Participant& b) {
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
      signaling_(SignalingClient::Options{.url = options_.signaling_url}) {
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

  // A second attempt on a socket that is already up is a retry after a refused
  // password, not a second connection. The server does not close the socket
  // when it refuses one - section 4.1 of docs/protocol.md answers with an error
  // and leaves the connection standing - so connect() would report
  // `already_connected` and the interface would show "disconnect() before
  // connecting again", which is a sentence written for whoever wrote the call
  // and not for whoever mistyped their password. Worse, the button then did
  // nothing at all: the only way back was to close the window.
  if (signaling_.is_connected()) {
    if (auto sent =
            signaling_.send(protocol::Authenticate{.username = username, .password = password});
        !sent) {
      set_state(State::Failed, sent.error().message);
      return sent;
    }
    return std::monostate{};
  }

  if (auto connected = signaling_.connect(); !connected) {
    set_state(State::Failed, connected.error().message);
    return connected;
  }

  if (!running_.exchange(true)) {
    metrics_thread_ = std::thread([this] { metrics_loop(); });
  }
  return std::monostate{};
}

Result<std::monostate> CallSession::create_room(const std::string& room_name, bool persistent) {
  std::string user_id;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    user_id = local_user_.id;
  }
  if (user_id.empty()) {
    return Result<std::monostate>::failure("unauthorized", "authenticate before creating a room");
  }
  return signaling_.send(
      protocol::CreateRoom{.user_id = user_id, .room_name = room_name, .persistent = persistent});
}

Result<std::monostate> CallSession::join(const std::string& room_id,
                                         const std::string& display_name) {
  std::string user_id;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    user_id = local_user_.id;
    room_id_ = room_id;
    // Kept so that a reconnection can walk back into the same room without
    // asking the interface to do it again.
    display_name_ = display_name;
  }
  if (user_id.empty()) {
    return Result<std::monostate>::failure("unauthorized", "authenticate before joining a room");
  }
  return signaling_.send(
      protocol::JoinRoom{.room_id = room_id, .user_id = user_id, .display_name = display_name});
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

  return signaling_.send(protocol::LeaveRoom{.room_id = room, .user_id = user_id});
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
    return signaling_.send(protocol::Mute{.room_id = room, .user_id = user_id});
  }
  return signaling_.send(protocol::Unmute{.room_id = room, .user_id = user_id});
}

bool CallSession::muted() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return muted_;
}

Result<std::monostate> CallSession::send_chat(const std::string& text) {
  std::string user_id;
  std::string room;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    user_id = local_user_.id;
    room = room_id_;
  }
  if (room.empty()) {
    return Result<std::monostate>::failure("not_in_room", "this session is not in a room");
  }

  // Checked here as well as on the server, against the same rule in the same
  // shared function. Not a trust boundary, and the server would refuse it
  // anyway: it is so that pressing return on an empty line does nothing at all
  // rather than making a round trip to be told so.
  if (!models::is_valid_chat_text(text)) {
    return Result<std::monostate>::failure(
        "invalid_value", "a message is between 1 and " + std::to_string(models::kMaxChatTextBytes) +
                             " bytes once trimmed");
  }

  // The display name and the time are left empty: they belong to the server,
  // and what it broadcasts back is what gets displayed.
  return signaling_.send(protocol::ChatMessage{
      .message = models::ChatMessage{.room_id = room, .user_id = user_id, .text = text}});
}

Result<std::monostate> CallSession::list_chat(int limit) {
  std::string room;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    room = room_id_;
  }
  if (room.empty()) {
    return Result<std::monostate>::failure("not_in_room", "this session is not in a room");
  }
  return signaling_.send(protocol::ListChat{.room_id = room, .limit = limit});
}

Result<std::monostate> CallSession::start_screen_share(const std::string& monitor_id) {
  std::string user_id;
  std::string room;
  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    user_id = local_user_.id;
    room = room_id_;
    session = audio_;
    if (!screen_sharer_.empty() && screen_sharer_ != user_id) {
      return Result<std::monostate>::failure(
          "screen_share_busy", screen_sharer_ + " is already sharing a screen in this room");
    }
  }

  if (room.empty()) {
    return Result<std::monostate>::failure("not_in_room",
                                           "there is no room to share a screen with");
  }
  if (!session) {
    return Result<std::monostate>::failure("media_unavailable",
                                           "there is no media session to send a screen on");
  }

  // Capture first, announcement second. The other way round would tell the
  // room about a share that a refused permission is about to cancel.
  if (auto started = session->start_screen_share(monitor_id); !started) {
    return started;
  }

  if (auto sent =
          signaling_.send(protocol::ScreenShareStarted{.room_id = room, .user_id = user_id});
      !sent) {
    session->stop_screen_share();
    return sent;
  }
  return std::monostate{};
}

Result<std::monostate> CallSession::stop_screen_share() {
  std::string user_id;
  std::string room;
  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    user_id = local_user_.id;
    room = room_id_;
    session = audio_;
  }

  if (session) {
    session->stop_screen_share();
  }
  if (room.empty()) {
    return std::monostate{};
  }
  return signaling_.send(protocol::ScreenShareStopped{.room_id = room, .user_id = user_id});
}

bool CallSession::sharing_screen() const {
  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    session = audio_;
  }
  return session && session->sharing_screen();
}

std::string CallSession::screen_sharer() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return screen_sharer_;
}

// Reads nothing from the session today. It stays a member because listing the
// monitors is something the interface asks the session for, and a future one
// may well consult the call's state to answer.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
Result<std::vector<video::Monitor>> CallSession::monitors() const {
  return video::monitors();
}

Result<std::monostate> CallSession::set_video_bitrate(int min_kbps, int max_kbps) {
  if (min_kbps <= 0 || max_kbps < min_kbps) {
    return Result<std::monostate>::failure(
        "invalid_value",
        "the bitrate range has to be positive and the maximum at least the minimum");
  }

  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    options_.media.video_min_bitrate_kbps = min_kbps;
    options_.media.video_max_bitrate_kbps = max_kbps;
    session = audio_;
  }
  if (session) {
    return session->set_video_bitrate(min_kbps, max_kbps);
  }
  // Chosen before there was a session to tell. It is kept in the options and
  // applied when one is created.
  return std::monostate{};
}

std::pair<int, int> CallSession::video_bitrate() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return {options_.media.video_min_bitrate_kbps, options_.media.video_max_bitrate_kbps};
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
    options_.media.input_device = device_id;
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
    options_.media.output_device = device_id;
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

media::VideoStats CallSession::video_stats() const {
  std::shared_ptr<media::MediaSession> session;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    session = audio_;
  }
  return session ? session->video_stats() : media::VideoStats{};
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
        if (auto sent =
                signaling_.send(protocol::Authenticate{.username = username, .password = password});
            !sent) {
          report(sent.error());
        }
      }
      break;
    }
    case SignalingClient::State::Failed:
      set_state(State::Failed, detail);
      break;
    case SignalingClient::State::Reconnecting:
      // Not Failed: the connection is coming back on its own, and the
      // interface has something different to say while it does. The room and
      // the identity are kept, and walked back into once the socket is up.
      set_state(State::Connecting, detail);
      break;
    case SignalingClient::State::Disconnected:
    case SignalingClient::State::Connecting:
      break;
  }
}

// --- administration ----------------------------------------------------------
//
// Each one is a message and nothing more. The server decides whether it is
// allowed, performs it, and announces the result, which arrives through
// handle_signal like everything else. That is why none of them returns an
// answer: there is nothing to return yet when they come back.

Result<std::monostate> CallSession::kick(const std::string& user_id, const std::string& reason) {
  std::string room;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    room = room_id_;
  }
  if (room.empty()) {
    return Result<std::monostate>::failure("not_in_room", "this session is not in a room");
  }
  return signaling_.send(protocol::KickUser{.room_id = room, .user_id = user_id, .reason = reason});
}

Result<std::monostate> CallSession::force_mute(const std::string& user_id, bool muted) {
  std::string room;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    room = room_id_;
  }
  if (room.empty()) {
    return Result<std::monostate>::failure("not_in_room", "this session is not in a room");
  }
  return signaling_.send(protocol::ForceMute{.room_id = room, .user_id = user_id, .muted = muted});
}

Result<std::monostate> CallSession::list_users() {
  return signaling_.send(protocol::ListUsers{});
}

Result<std::monostate> CallSession::create_user(const std::string& username,
                                                const std::string& password,
                                                const std::string& display_name,
                                                models::Role role) {
  return signaling_.send(protocol::CreateUser{
      .username = username, .password = password, .display_name = display_name, .role = role});
}

Result<std::monostate> CallSession::update_user(const protocol::UpdateUser& change) {
  return signaling_.send(change);
}

Result<std::monostate> CallSession::delete_user(const std::string& user_id) {
  return signaling_.send(protocol::DeleteUser{.user_id = user_id});
}

Result<std::monostate> CallSession::list_rooms() {
  return signaling_.send(protocol::ListRooms{});
}

Result<std::monostate> CallSession::delete_room(const std::string& room_id) {
  return signaling_.send(protocol::DeleteRoom{.room_id = room_id});
}

Result<std::monostate> CallSession::list_audit(int limit, const std::string& actor_id) {
  return signaling_.send(protocol::ListAudit{.limit = limit, .actor_id = actor_id});
}

models::Role CallSession::role() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return local_user_.role;
}

bool CallSession::is_admin() const {
  return role() == models::Role::Admin;
}

void CallSession::handle_signal(protocol::Message message) {
  if (const auto* authenticated = std::get_if<protocol::Authenticated>(&message)) {
    std::string rejoin_room;
    std::string rejoin_name;
    std::string user_id;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      local_user_ = authenticated->user;
      user_id = authenticated->user.id;
      // The password stays. It is the only credential the protocol has, and
      // reconnecting after the server restarts means authenticating again
      // without asking the user to type it a second time. A resume token
      // would let this be dropped, and that belongs with the rest of the
      // authentication work in M8.
      rejoin_room = room_id_;
      rejoin_name = display_name_;
      // Whoever was in the room is about to be announced again from scratch.
      participants_.clear();
      screen_sharer_.clear();
    }
    set_state(State::Authenticated, authenticated->user.id);

    // Authenticating with a room already set means this is a reconnection.
    // Walking back in is what makes a server restart look like a pause rather
    // than the end of the call.
    if (!rejoin_room.empty()) {
      DV_LOG_INFO("Call session: rejoining room {} after reconnecting", rejoin_room);
      if (auto sent = signaling_.send(protocol::JoinRoom{
              .room_id = rejoin_room, .user_id = user_id, .display_name = rejoin_name});
          !sent) {
        report(sent.error());
      }
    }
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
    Callbacks handlers;
    std::shared_ptr<media::MediaSession> session;
    bool is_us = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = participants_.find(muted->user_id); it != participants_.end()) {
        it->second.muted = true;
      }
      is_us = muted->user_id == local_user_.id;
      if (is_us) {
        muted_ = true;
        session = audio_;
      }
      handlers = callbacks_;
    }

    // The microphone itself, and not only the flag the interface reads. When
    // this message is the echo of our own set_muted the capture is already
    // stopped and this changes nothing; when it came from an administrator's
    // force_mute it is the whole point, and without it a muted participant
    // keeps talking into the room while every client shows them silent.
    //
    // Outside the lock: the media session takes its own.
    if (session) {
      session->set_microphone_muted(true);
    }
    // Only when somebody else did it. A microphone that turned itself off
    // needs no explanation; one that was turned off for you does.
    if (!muted->by_user_id.empty() && handlers.on_forced_mute) {
      handlers.on_forced_mute(muted->user_id, muted->by_user_id, true);
    }
    publish_participants();
    return;
  }

  if (const auto* unmuted = std::get_if<protocol::Unmute>(&message)) {
    Callbacks handlers;
    std::shared_ptr<media::MediaSession> session;
    bool is_us = false;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = participants_.find(unmuted->user_id); it != participants_.end()) {
        it->second.muted = false;
      }
      is_us = unmuted->user_id == local_user_.id;
      if (is_us) {
        muted_ = false;
        session = audio_;
      }
      handlers = callbacks_;
    }

    // The same in reverse: an administrator releasing a forced mute has to
    // give the microphone back, not merely say that it was given back.
    if (session) {
      session->set_microphone_muted(false);
    }
    if (!unmuted->by_user_id.empty() && handlers.on_forced_mute) {
      handlers.on_forced_mute(unmuted->user_id, unmuted->by_user_id, false);
    }
    publish_participants();
    return;
  }

  if (const auto* kicked = std::get_if<protocol::UserKicked>(&message)) {
    Callbacks handlers;
    bool is_us = false;
    bool floor_freed = false;
    std::shared_ptr<media::MediaSession> session;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      participants_.erase(kicked->user_id);
      is_us = kicked->user_id == local_user_.id;
      handlers = callbacks_;

      // Somebody removed while holding the screen share floor releases it.
      // The server says so too, with screen_share_stopped, but not when the
      // room empties in the same breath: the last participant out has nobody
      // left to be told by. Without this, that client would go on believing
      // the floor is taken and refuse its own next share.
      if (kicked->user_id == screen_sharer_) {
        screen_sharer_.clear();
        floor_freed = true;
      }
      if (is_us) {
        // The same teardown leave() does, minus the message to the server:
        // the server has already removed us, and sending leave_room now would
        // only be answered with not_in_room.
        room_id_.clear();
        display_name_.clear();
        participants_.clear();
        screen_sharer_.clear();
        session.swap(audio_);
      }
    }

    if (session) {
      // Closed outside the lock: it waits for media callbacks, and those take
      // it.
      session->close();
    }
    if (floor_freed && handlers.on_screen_share) {
      handlers.on_screen_share({});
    }
    if (is_us) {
      set_state(State::Authenticated, "removed from the room");
      if (handlers.on_kicked) {
        handlers.on_kicked(kicked->reason);
      }
    }
    publish_participants();
    return;
  }

  if (const auto* users = std::get_if<protocol::UserList>(&message)) {
    Callbacks handlers;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handlers = callbacks_;
    }
    if (handlers.on_user_list) {
      handlers.on_user_list(users->users);
    }
    return;
  }

  if (const auto* rooms = std::get_if<protocol::RoomList>(&message)) {
    Callbacks handlers;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handlers = callbacks_;
    }
    if (handlers.on_room_list) {
      handlers.on_room_list(rooms->rooms);
    }
    return;
  }

  if (const auto* entries = std::get_if<protocol::AuditList>(&message)) {
    Callbacks handlers;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handlers = callbacks_;
    }
    if (handlers.on_audit_list) {
      handlers.on_audit_list(entries->entries);
    }
    return;
  }

  if (const auto* sharing = std::get_if<protocol::ScreenShareStarted>(&message)) {
    Callbacks handlers;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = participants_.find(sharing->user_id); it != participants_.end()) {
        it->second.sharing_screen = true;
      }
      screen_sharer_ = sharing->user_id;
      handlers = callbacks_;
    }
    if (handlers.on_screen_share) {
      handlers.on_screen_share(sharing->user_id);
    }
    publish_participants();
    return;
  }

  if (const auto* stopped = std::get_if<protocol::ScreenShareStopped>(&message)) {
    Callbacks handlers;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = participants_.find(stopped->user_id); it != participants_.end()) {
        it->second.sharing_screen = false;
      }
      if (screen_sharer_ == stopped->user_id) {
        screen_sharer_.clear();
      }
      handlers = callbacks_;
    }
    if (handlers.on_screen_share) {
      handlers.on_screen_share(std::string{});
    }
    publish_participants();
    return;
  }

  if (const auto* chat = std::get_if<protocol::ChatMessage>(&message)) {
    Callbacks handlers;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handlers = callbacks_;
    }
    if (handlers.on_chat_message) {
      handlers.on_chat_message(chat->message);
    }
    return;
  }

  if (const auto* history = std::get_if<protocol::ChatHistory>(&message)) {
    Callbacks handlers;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handlers = callbacks_;
    }
    if (handlers.on_chat_history) {
      handlers.on_chat_history(history->messages);
    }
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
    report(Error{.code = error->code, .message = error->message});
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

  const media::IceCandidate value{.candidate = candidate.candidate,
                                  .sdp_mid = candidate.sdp_mid,
                                  .sdp_mline_index = candidate.sdp_mline_index};
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
    if (auto sent =
            signaling_.send(protocol::Answer{.room_id = room,
                                             .from_user_id = user_id,
                                             .to_user_id = std::string(protocol::kSfuUserId),
                                             .sdp = std::move(sdp)});
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
    if (auto sent =
            signaling_.send(protocol::IceCandidate{.room_id = room,
                                                   .from_user_id = user_id,
                                                   .to_user_id = std::string(protocol::kSfuUserId),
                                                   .candidate = std::move(candidate.candidate),
                                                   .sdp_mid = std::move(candidate.sdp_mid),
                                                   .sdp_mline_index = candidate.sdp_mline_index});
        !sent) {
      report(sent.error());
    }
  };

  callbacks.on_remote_audio = [this](const std::string& user_id, bool active) {
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      if (const auto it = participants_.find(user_id); it != participants_.end()) {
        it->second.audio_active = active;
      }
    }
    publish_participants();
  };

  callbacks.on_levels = [this](const std::vector<media::AudioLevel>& levels) {
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

  callbacks.on_remote_video = [this](video::VideoFrame frame) {
    Callbacks handlers;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handlers = callbacks_;
    }
    if (handlers.on_remote_video) {
      handlers.on_remote_video(std::move(frame));
    }
  };

  callbacks.on_screen_share_ended = [this](Error reason) {
    // The capture stopped by itself, so the room still thinks a share is on.
    // Telling it is what keeps every client's view of who holds the floor
    // right, and the local user gets the reason.
    (void)stop_screen_share();

    Callbacks handlers;
    {
      const std::lock_guard<std::mutex> lock(mutex_);
      handlers = callbacks_;
    }
    if (handlers.on_error) {
      handlers.on_error(std::move(reason));
    }
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

  auto created = media_factory_(options_.media, std::move(callbacks));
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
    input_device = options_.media.input_device;
    output_device = options_.media.output_device;
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

    // The screen share is only worth a line while there is one, and then the
    // number that matters is what the network is willing to carry against what
    // is being sent: those two apart is what a picture falling behind looks
    // like from here.
    if (const media::VideoStats video = session->video_stats(); video.frames_sent > 0) {
      DV_LOG_INFO(
          "Video: {}x{} at {:.1f} fps, up {:.0f} kbps, estimate {:.0f} kbps, {} frames dropped, "
          "encoder {}",
          video.send_width, video.send_height, video.send_fps, video.send_bitrate_kbps,
          video.available_send_bitrate_kbps, video.frames_dropped,
          video.encoder.empty() ? "starting" : video.encoder);
    }

    if (callbacks.on_metrics) {
      callbacks.on_metrics(stats);
    }
  }
}

}  // namespace dv::client::app
