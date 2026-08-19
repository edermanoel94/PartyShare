#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <dv/core/result.hpp>
#include <dv/models/user.hpp>
#include <dv/protocol/message.hpp>

#include "media/media_session.hpp"
#include "network/signaling_client.hpp"
#include "video/screen_capturer.hpp"

namespace dv::client::app {

/// A participant as the interface needs to show them.
struct Participant {
  models::User user;
  bool muted = false;
  bool sharing_screen = false;
  /// Their audio is arriving on a track of ours.
  bool audio_active = false;
  /// How loud they are right now, from 0 to 1, and whether that counts as
  /// speaking. Both come from the media layer several times a second.
  double level = 0;
  bool speaking = false;
};

/// Drives one call, from logging in to hearing the other participants.
///
/// This is the application core of section 15 of SPEC.md: it owns the session
/// state and the order of operations, and it knows nothing about Qt or about
/// libwebrtc. The interface observes it, and the media layer is reached only
/// through media::MediaSession, which is what lets the whole flow be tested
/// against a real server with a stand-in for the media.
///
/// Every callback runs on a networking or media thread. The interface has to
/// hop to its own thread, which in Qt means a queued connection.
class CallSession {
 public:
  enum class State : std::uint8_t {
    Idle,
    Connecting,
    /// Signaling is up and the identity is established.
    Authenticated,
    /// In a room, with media negotiated or being negotiated.
    InCall,
    Failed,
  };

  struct Options {
    std::string signaling_url;
    media::MediaSessionOptions audio;
    /// Section 22 of SPEC.md wants these numbers, and M4 wants them in the log.
    std::chrono::milliseconds metrics_interval{5000};
  };

  struct Callbacks {
    std::function<void(State state, std::string detail)> on_state;
    /// The room's participants, whenever the list changes.
    std::function<void(std::vector<Participant>)> on_participants;
    std::function<void(media::AudioStats)> on_metrics;
    /// The local microphone level, from 0 to 1, several times a second. The
    /// levels of the other participants arrive through `on_participants`.
    std::function<void(double level, bool speaking)> on_local_level;
    std::function<void(Error error)> on_error;

    /// A decoded frame of the screen somebody else is sharing. Arrives on a
    /// media thread, several times a second, and must not be blocked on.
    std::function<void(video::VideoFrame frame)> on_remote_video;
    /// Who is sharing a screen in this room, or an empty id when nobody is.
    /// Comes from signaling rather than from the track, because the video
    /// track carries whoever holds the floor.
    std::function<void(std::string user_id)> on_screen_share;
  };

  /// Creates the media session. Injectable so that the tests can drive the
  /// whole flow without libwebrtc.
  using MediaSessionFactory = std::function<Result<std::unique_ptr<media::MediaSession>>(
      const media::MediaSessionOptions&, media::MediaSession::Callbacks)>;

  explicit CallSession(Options options);
  CallSession(Options options, MediaSessionFactory factory);
  ~CallSession();

  CallSession(const CallSession&) = delete;
  CallSession& operator=(const CallSession&) = delete;
  CallSession(CallSession&&) = delete;
  CallSession& operator=(CallSession&&) = delete;

  /// Install before connecting: a handler added later misses what already
  /// happened.
  void on_events(Callbacks callbacks);

  /// Connects and authenticates as soon as the socket is up. Returns once the
  /// attempt has started, not once it has succeeded: progress arrives through
  /// the state callback.
  [[nodiscard]] Result<std::monostate> connect_and_authenticate(const std::string& username,
                                                                const std::string& password);

  /// Creates a room and reports its identifier through `on_room_created`.
  [[nodiscard]] Result<std::monostate> create_room(const std::string& room_name);
  void on_room_created(std::function<void(std::string room_id)> handler);

  [[nodiscard]] Result<std::monostate> join(const std::string& room_id,
                                            const std::string& display_name);
  [[nodiscard]] Result<std::monostate> leave();

  [[nodiscard]] Result<std::monostate> set_muted(bool muted);
  [[nodiscard]] bool muted() const;

  /// Starts sharing `monitor_id`, or the primary monitor when empty.
  ///
  /// Two things have to happen and both can fail: the capture has to start,
  /// and the room has to be told. The capture goes first, so that a refused
  /// permission does not announce a share that is not happening.
  ///
  /// Fails with `screen_share_busy` when somebody else already holds the
  /// floor, and with what the capture layer reports otherwise.
  [[nodiscard]] Result<std::monostate> start_screen_share(const std::string& monitor_id = {});
  [[nodiscard]] Result<std::monostate> stop_screen_share();
  [[nodiscard]] bool sharing_screen() const;
  /// Who is sharing right now, empty when nobody is.
  [[nodiscard]] std::string screen_sharer() const;

  /// The monitors this machine can share.
  [[nodiscard]] Result<std::vector<video::Monitor>> monitors() const;

  /// Playback volume for one participant, from 0 to 1, with up to 10 allowed
  /// as amplification. Remembered and reapplied if their audio arrives later.
  [[nodiscard]] Result<std::monostate> set_participant_volume(const std::string& user_id,
                                                              double volume);

  /// Switches capture or playback device without ending the call. Without a
  /// media session yet, the choice is kept and applied when one is created.
  [[nodiscard]] Result<std::monostate> set_input_device(const std::string& device_id);
  [[nodiscard]] Result<std::monostate> set_output_device(const std::string& device_id);

  void disconnect();

  [[nodiscard]] State state() const;
  [[nodiscard]] models::User local_user() const;
  [[nodiscard]] std::string room_id() const;
  [[nodiscard]] std::vector<Participant> participants() const;
  [[nodiscard]] media::AudioStats stats() const;

 private:
  void handle_signal(protocol::Message message);
  void handle_signaling_state(SignalingClient::State state, const std::string& detail);

  void handle_offer(const protocol::Offer& offer);
  void handle_ice_candidate(const protocol::IceCandidate& candidate);

  /// Builds the media session on the first offer. It cannot be built earlier:
  /// before joining there is nothing to negotiate.
  [[nodiscard]] Result<std::monostate> ensure_media_session();

  void set_state(State state, const std::string& detail);
  void publish_participants();
  void report(const Error& error);
  void metrics_loop();

  Options options_;
  MediaSessionFactory media_factory_;

  mutable std::mutex mutex_;
  SignalingClient signaling_;
  /// Shared rather than owned outright: a media callback can be running while
  /// leave() drops the session, and the callback has to stay on solid ground.
  std::shared_ptr<media::MediaSession> audio_;

  State state_ = State::Idle;
  models::User local_user_;
  std::string room_id_;
  bool muted_ = false;
  /// Who holds the screen share floor in this room, empty when nobody does.
  /// Kept from the ScreenShareStarted and ScreenShareStopped the server
  /// broadcasts, so every client agrees on it.
  std::string screen_sharer_;
  std::unordered_map<std::string, Participant> participants_;
  /// Per participant playback volume, kept here so that a choice made before
  /// the call survives into it.
  std::unordered_map<std::string, double> volumes_;

  /// Held until the socket is open, because the server only accepts
  /// `authenticate` on a connection that exists.
  std::string pending_username_;
  std::string pending_password_;

  Callbacks callbacks_;
  std::function<void(std::string)> room_created_handler_;

  std::atomic<bool> running_{false};
  std::thread metrics_thread_;
};

[[nodiscard]] std::string_view to_string(CallSession::State state) noexcept;

}  // namespace dv::client::app
