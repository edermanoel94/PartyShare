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
#include <utility>
#include <vector>

#include <dv/core/result.hpp>
#include <dv/models/chat.hpp>
#include <dv/models/user.hpp>
#include <dv/protocol/message.hpp>

#include "audio/audio_sources.hpp"
#include "media/media_session.hpp"
#include "network/signaling_client.hpp"
#include "video/screen_capturer.hpp"

namespace dv::client::app {

/// What a screen share should carry besides the picture.
struct ScreenAudio {
  enum class Mode : std::uint8_t {
    /// The share is silent, which is what every share was until now.
    None,
    /// Everything the machine plays except this process. The exception is the
    /// whole of the feedback protection - see
    /// docs/audio-da-tela-compartilhada.md, section 6.
    System,
    /// One application and the processes below it.
    Application,
  };

  Mode mode = Mode::None;
  /// A process id, read only in `Application` mode.
  std::uint32_t source_id = 0;
};

/// The names configuration and the settings dialog use for the modes. Inline
/// so that the mapping lives in one place and neither of them invents its own.
[[nodiscard]] constexpr std::string_view to_string(ScreenAudio::Mode mode) noexcept {
  switch (mode) {
    case ScreenAudio::Mode::None:
      return "none";
    case ScreenAudio::Mode::System:
      return "system";
    case ScreenAudio::Mode::Application:
      return "process";
  }
  return "none";
}

/// Anything unrecognised reads as None. A share that is silent because the
/// configuration says something this build does not understand is a great deal
/// better than one that captures the machine because of it.
[[nodiscard]] constexpr ScreenAudio::Mode screen_audio_mode_from(std::string_view name) noexcept {
  if (name == "system") {
    return ScreenAudio::Mode::System;
  }
  if (name == "process") {
    return ScreenAudio::Mode::Application;
  }
  return ScreenAudio::Mode::None;
}

/// A participant as the interface needs to show them.
struct Participant {
  models::User user;
  bool muted = false;
  bool sharing_screen = false;
  /// Whether that share is carrying the sound of their machine, which is also
  /// why the volume below now controls two things at once for them.
  bool sharing_audio = false;
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
    media::MediaSessionOptions media;
    /// What the share dialog opens on. Remembered here for the same reason the
    /// bitrate is: a choice made before a call has to survive into it.
    ScreenAudio::Mode screen_audio_mode = ScreenAudio::Mode::System;
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

    /// One line of the room's conversation, as the server confirmed it. This
    /// is also how a message this session sent comes back: nothing is
    /// displayed until the server has agreed to it, so every participant reads
    /// the same messages in the same order.
    std::function<void(models::ChatMessage message)> on_chat_message;

    /// What was said in the room before now: on joining, and again as the
    /// answer to `list_chat`.
    ///
    /// It replaces whatever is on screen rather than adding to it. It arrives
    /// once per join, and after a reconnection the same messages would
    /// otherwise be appended a second time.
    std::function<void(std::vector<models::ChatMessage> messages)> on_chat_history;

    // --- administration ---
    //
    // Only ever called on a session that authenticated as an administrator,
    // because the server refuses the requests that produce them otherwise.

    /// The answer to list_users, and to every change that alters that list.
    std::function<void(std::vector<protocol::UserSummary>)> on_user_list;
    std::function<void(std::vector<protocol::RoomSummary>)> on_room_list;
    std::function<void(std::vector<models::AuditEntry>)> on_audit_list;

    /// This session was removed from its room by an administrator, with
    /// whatever reason they gave. The room has already been left by the time
    /// this runs, so the interface has only to say so and go back.
    std::function<void(std::string reason)> on_kicked;

    /// Somebody in the room was muted or unmuted by an administrator, named
    /// by `by_user_id`. A participant muting themselves does not come through
    /// here: that is an ordinary participant update.
    std::function<void(std::string user_id, std::string by_user_id, bool muted)> on_forced_mute;

    /// An administrator changed what an account may do, for longer than this
    /// room lasts. See models::Restrictions.
    ///
    /// It arrives for anybody in the room and for this session's own account
    /// whether or not it is in one, which is the case that matters: somebody
    /// told they may no longer share a screen can stop reaching for the
    /// button, and somebody not told is left clicking one that does nothing.
    ///
    /// The participant list and `local_user()` are already up to date by the
    /// time this runs. What is left for the interface is to say so.
    std::function<void(std::string user_id, models::Restrictions restrictions,
                       std::string by_user_id, std::string reason)>
        on_restrictions_changed;
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
  ///
  /// `persistent` asks for a room that outlives its last participant, which
  /// the server accepts only from an administrator.
  [[nodiscard]] Result<std::monostate> create_room(const std::string& room_name,
                                                   bool persistent = false);
  void on_room_created(std::function<void(std::string room_id)> handler);

  [[nodiscard]] Result<std::monostate> join(const std::string& room_id,
                                            const std::string& display_name);
  [[nodiscard]] Result<std::monostate> leave();

  [[nodiscard]] Result<std::monostate> set_muted(bool muted);
  [[nodiscard]] bool muted() const;

  /// Says something in the room this session is in.
  ///
  /// Reports nothing back beyond having sent it: the message reaches the
  /// interface through `on_chat_message`, once the server has stored it and
  /// broadcast it to everybody. Fails locally with `not_in_room` when there is
  /// no room, and with `invalid_value` for text the server would refuse
  /// anyway, so an empty line does not become a round trip.
  [[nodiscard]] Result<std::monostate> send_chat(const std::string& text);

  /// Asks for the room's conversation again, up to `limit` messages, answered
  /// through `on_chat_history`. Zero asks for the server's default.
  ///
  /// Not needed to see the conversation on joining, which arrives on its own.
  [[nodiscard]] Result<std::monostate> list_chat(int limit = 0);

  /// Starts sharing `monitor_id`, or the primary monitor when empty.
  ///
  /// Two things have to happen and both can fail: the capture has to start,
  /// and the room has to be told. The capture goes first, so that a refused
  /// permission does not announce a share that is not happening.
  ///
  /// Fails with `screen_share_busy` when somebody else already holds the
  /// floor, and with what the capture layer reports otherwise.
  ///
  /// `audio` asks for the machine's sound to go with the picture. It is the one
  /// part that does **not** fail the share: a Windows too old to capture per
  /// process, or an application that closed between the menu and the click,
  /// gives a share that is silent rather than no share at all. Ask
  /// `screen_audio_active()` for what actually happened, and
  /// `screen_audio_failure()` for why.
  [[nodiscard]] Result<std::monostate> start_screen_share(const std::string& monitor_id = {},
                                                          ScreenAudio audio = {});
  [[nodiscard]] Result<std::monostate> stop_screen_share();
  [[nodiscard]] bool sharing_screen() const;

  /// True while this participant's share is carrying their machine's sound.
  [[nodiscard]] bool screen_audio_active() const;

  /// Why the sound is not going out, when it was asked for and did not start.
  /// Empty code when nothing went wrong.
  [[nodiscard]] Error screen_audio_failure() const;

  /// The applications this machine can be heard playing, for the share dialog.
  [[nodiscard]] Result<std::vector<audio::AudioSource>> audio_sources() const;

  /// What the next share will carry unless the dialog is told otherwise.
  [[nodiscard]] ScreenAudio::Mode screen_audio_mode() const;
  /// Who is sharing right now, empty when nobody is.
  [[nodiscard]] std::string screen_sharer() const;

  /// The monitors this machine can share.
  [[nodiscard]] Result<std::vector<video::Monitor>> monitors() const;

  /// The bitrate range the screen encoder may use, in kbps. Remembered, so a
  /// choice made before a call survives into it.
  [[nodiscard]] Result<std::monostate> set_video_bitrate(int min_kbps, int max_kbps);
  [[nodiscard]] std::pair<int, int> video_bitrate() const;

  /// The lowest congestion control may squeeze the screen share to, which is
  /// also the lowest the minimum above is allowed to be.
  ///
  /// Read only: it is a property of what the link is worth carrying rather than
  /// a preference, and dv::config::validate refuses a configuration whose floor
  /// sits above its minimum. The settings dialog needs it so that the range it
  /// offers cannot produce that configuration.
  [[nodiscard]] int video_floor_bitrate_kbps() const;

  /// The size and rate the screen is sent at. Remembered the same way the
  /// bitrate is, so choosing it outside a call carries into the next one.
  ///
  /// `size` is a ceiling, not the size sent: a monitor is fitted inside it with
  /// its aspect ratio kept, and one smaller than the box is sent as it is. See
  /// video::fit_within.
  ///
  /// During a share this restarts the capture, so the picture stutters for a
  /// moment. Fails with `invalid_value` for a size that is not positive or a
  /// rate below one, and leaves the previous choice in place.
  [[nodiscard]] Result<std::monostate> set_video_quality(video::Size size, int fps);
  [[nodiscard]] video::ScreenCaptureOptions video_quality() const;

  /// Playback volume for one participant, from 0 to 1, with up to 10 allowed
  /// as amplification. Remembered and reapplied if their audio arrives later.
  [[nodiscard]] Result<std::monostate> set_participant_volume(const std::string& user_id,
                                                              double volume);

  /// Switches capture or playback device without ending the call. Without a
  /// media session yet, the choice is kept and applied when one is created.
  [[nodiscard]] Result<std::monostate> set_input_device(const std::string& device_id);
  [[nodiscard]] Result<std::monostate> set_output_device(const std::string& device_id);

  /// The devices in use, or empty for the system's own choice.
  ///
  /// What the configuration file asked for until somebody changes it, which is
  /// what lets the settings dialog open showing the microphone that is actually
  /// being captured rather than whichever one the system listed first.
  [[nodiscard]] std::string input_device() const;
  [[nodiscard]] std::string output_device() const;

  void disconnect();

  // --- administration --------------------------------------------------------
  //
  // Every one of these is refused by the server unless this session
  // authenticated as an administrator. The interface asks `is_admin` before
  // offering them, but that is presentation: the decision is the server's, and
  // a client that asks anyway is answered with `forbidden`.
  //
  // They report through the callbacks above rather than returning an answer,
  // because the reply arrives on the signaling thread like everything else.

  [[nodiscard]] Result<std::monostate> kick(const std::string& user_id,
                                            const std::string& reason = {});
  [[nodiscard]] Result<std::monostate> force_mute(const std::string& user_id, bool muted);

  /// Takes something away from an account, or gives it back, for longer than
  /// the room lasts. Absent fields are left as they are, see
  /// protocol::RestrictUser.
  ///
  /// Unlike kick and force_mute, this needs no room: it is about the account
  /// and can be applied from the administration panel to somebody who is not
  /// connected at all.
  [[nodiscard]] Result<std::monostate> restrict_user(const protocol::RestrictUser& change);

  [[nodiscard]] Result<std::monostate> list_users();
  [[nodiscard]] Result<std::monostate> create_user(const std::string& username,
                                                   const std::string& password,
                                                   const std::string& display_name,
                                                   models::Role role);
  /// Absent fields are left as they are, see protocol::UpdateUser.
  [[nodiscard]] Result<std::monostate> update_user(const protocol::UpdateUser& change);
  [[nodiscard]] Result<std::monostate> delete_user(const std::string& user_id);

  [[nodiscard]] Result<std::monostate> list_rooms();
  [[nodiscard]] Result<std::monostate> delete_room(const std::string& room_id);

  [[nodiscard]] Result<std::monostate> list_audit(int limit = 0, const std::string& actor_id = {});

  [[nodiscard]] State state() const;
  [[nodiscard]] models::User local_user() const;
  /// The role this session authenticated as, as the server reported it.
  [[nodiscard]] models::Role role() const;
  [[nodiscard]] bool is_admin() const;
  [[nodiscard]] std::string room_id() const;
  [[nodiscard]] std::vector<Participant> participants() const;
  [[nodiscard]] media::AudioStats stats() const;
  /// The screen share, from capture through to what the congestion controller
  /// says the link can carry. Zeroed when there is no call.
  [[nodiscard]] media::VideoStats video_stats() const;

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
  /// Starts the machine's sound on an already running share. Returns whether
  /// it is going out, and records why not when it is not.
  bool start_screen_audio(media::MediaSession& session, ScreenAudio audio);

  /// Why the screen audio did not start, when it was asked for. Kept rather
  /// than returned because starting the sound is not allowed to fail the share:
  /// see start_screen_share.
  Error screen_audio_failure_;
  std::unordered_map<std::string, Participant> participants_;
  /// Per participant playback volume, kept here so that a choice made before
  /// the call survives into it.
  std::unordered_map<std::string, double> volumes_;

  /// Held until the socket is open, because the server only accepts
  /// `authenticate` on a connection that exists.
  std::string pending_username_;
  std::string pending_password_;
  /// The name to rejoin under after a reconnection.
  std::string display_name_;

  Callbacks callbacks_;
  std::function<void(std::string)> room_created_handler_;

  std::atomic<bool> running_{false};
  std::thread metrics_thread_;
};

[[nodiscard]] std::string_view to_string(CallSession::State state) noexcept;

}  // namespace dv::client::app
