#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <dv/core/result.hpp>

#include "audio/loopback_capturer.hpp"
#include "video/screen_capturer.hpp"
#include "video/video_frame.hpp"

namespace dv::client::media {

/// One ICE candidate, in the shape the signaling protocol carries it.
struct IceCandidate {
  std::string candidate;
  std::string sdp_mid;
  int sdp_mline_index = 0;

  friend bool operator==(const IceCandidate&, const IceCandidate&) = default;
};

/// A capture or playback device, as the system reports it.
struct AudioDevice {
  /// Stable enough to be stored in the configuration and matched again later.
  /// On every platform libwebrtc supports, this is the device name.
  std::string id;
  std::string name;
  bool is_default = false;

  friend bool operator==(const AudioDevice&, const AudioDevice&) = default;
};

/// How loud someone is right now, from 0 to 1.
struct AudioLevel {
  /// Empty for the local microphone.
  std::string user_id;
  double level = 0;
  bool speaking = false;

  friend bool operator==(const AudioLevel&, const AudioLevel&) = default;
};

/// How hard the noise suppressor bites, in the four steps libwebrtc offers.
/// They take 6, 12, 18 and 21 dB off the noise, in that order. See
/// docs/16-audio-plan.md, step 5.
enum class NoiseSuppressionLevel : std::uint8_t { Low, Moderate, High, VeryHigh };

/// The spelling `audio.noise_suppression_level` uses for each level, and back.
/// Nothing for a word that is not one of the four; the configuration refuses
/// those before they get here, so a caller may treat nothing as impossible and
/// still have to pick an answer for it.
[[nodiscard]] constexpr std::string_view to_string(NoiseSuppressionLevel level) noexcept {
  switch (level) {
    case NoiseSuppressionLevel::Low:
      return "low";
    case NoiseSuppressionLevel::Moderate:
      return "moderate";
    case NoiseSuppressionLevel::High:
      return "high";
    case NoiseSuppressionLevel::VeryHigh:
      return "very_high";
  }
  return "high";
}

[[nodiscard]] constexpr std::optional<NoiseSuppressionLevel> parse_noise_suppression_level(
    std::string_view text) noexcept {
  for (const NoiseSuppressionLevel level :
       {NoiseSuppressionLevel::Low, NoiseSuppressionLevel::Moderate, NoiseSuppressionLevel::High,
        NoiseSuppressionLevel::VeryHigh}) {
    if (text == to_string(level)) {
      return level;
    }
  }
  return std::nullopt;
}

/// What section 22 of SPEC.md asks to be measured, read from the WebRTC stats.
struct AudioStats {
  double round_trip_time_ms = 0;
  double jitter_ms = 0;
  std::uint64_t packets_lost = 0;
  std::uint64_t packets_sent = 0;
  std::uint64_t packets_received = 0;
  std::uint64_t bytes_sent = 0;
  std::uint64_t bytes_received = 0;
  /// Outgoing audio bitrate, averaged over the last collection interval.
  double send_bitrate_kbps = 0;
  double receive_bitrate_kbps = 0;
  /// What the audio encoder is currently asked to aim for, in kbps. Fixed at
  /// the offer's ceiling unless the experimental adaptive audio is on, when it
  /// is what the allocation hands the audio: docs/16-audio-plan.md, step 10.
  double audio_target_bitrate_kbps = 0;

  /// What the jitter buffer had to invent, summed over every participant's
  /// incoming audio: the samples received at all, the samples played that
  /// were never received, and the number of times playout entered such a
  /// stretch. The first turns the second into a ratio; the third is how many
  /// holes were heard, which is what a listener actually notices.
  ///
  /// Totals since the connection began, like the packet counts. A loss the
  /// redundancy repaired never reaches these: they measure what the listener
  /// heard go missing, which `packets_lost` cannot tell from a loss that was
  /// made good. See docs/16-audio-plan.md, step 1.
  std::uint64_t total_samples_received = 0;
  std::uint64_t concealed_samples = 0;
  std::uint64_t concealment_events = 0;
  /// The rest of what the jitter buffer reports, for docs/16-audio-plan.md
  /// step 9: concealment that only papered over silence, which nobody hears;
  /// the delay every emitted sample waited in the buffer, summed, and how many
  /// samples that sum is over, which together give the buffer's average depth;
  /// packets the buffer threw away as too late; and packets whose loss was
  /// covered by in-band FEC, which is zero while RED carries the redundancy.
  std::uint64_t silent_concealed_samples = 0;
  double jitter_buffer_delay_seconds = 0;
  std::uint64_t jitter_buffer_emitted_count = 0;
  std::uint64_t packets_discarded = 0;
  std::uint64_t fec_packets_received = 0;

  /// The retransmission, from this end. NACKs this client sent for audio it
  /// was expecting and did not get, NACKs it received for audio it sent, and
  /// the audio packets it sent again in answer. Totals since the connection
  /// began. All three stay at zero while the server does not offer `nack`.
  /// See docs/16-audio-plan.md, step 2.
  std::uint64_t nacks_sent = 0;
  std::uint64_t nacks_received = 0;
  std::uint64_t retransmitted_packets_sent = 0;

  /// True while the echo canceller is running on the captured audio.
  ///
  /// libwebrtc only reports echo return loss when the echo controller exists,
  /// so this is the one place where AEC3 being alive is observable from
  /// outside the media layer, rather than merely requested.
  bool echo_cancellation_active = false;
  /// How much of the echo the canceller is removing, in dB. Zero when it is
  /// not running, or when there is no echo to remove.
  double echo_return_loss_db = 0;
  /// True while the gain control runs on the captured audio, read back from
  /// the processing module rather than measured: the gain control has no
  /// statistic of its own. It is libwebrtc's second generation controller,
  /// AGC2, since docs/16-audio-plan.md step 4. `legacy_gain_control_active`
  /// is the first generation, which that step retired and which stays false
  /// so that a test can say so.
  bool gain_control_active = false;
  bool legacy_gain_control_active = false;
  /// The noise suppressor, read back the same way: whether it runs, and how
  /// hard, since docs/16-audio-plan.md step 5 made the level a setting.
  bool noise_suppression_active = false;
  NoiseSuppressionLevel noise_suppression_level = NoiseSuppressionLevel::High;

  /// True while what the screen is playing is being mixed into this
  /// participant's audio. See `start_screen_audio`.
  bool screen_audio_active = false;
  /// The microphone on its own, from 0 to 1, read before the screen audio is
  /// added to it.
  ///
  /// Separate from the level in `AudioLevel` because once a share is on they
  /// stop being the same number: everything downstream of the mixer carries
  /// voice and music together, and a level taken from there would show somebody
  /// as talking for the whole length of a video.
  double microphone_level = 0;
  /// Blocks of screen audio that carried silence because the application was
  /// quiet, out of `screen_audio_blocks`. Both count from the start of the
  /// process rather than from the start of the share.
  std::uint64_t screen_audio_blocks = 0;
  std::uint64_t screen_audio_silent_blocks = 0;
  /// Blocks of screen audio that actually reached the encoder, and blocks the
  /// mixer had to fill with silence because the buffer between the capture and
  /// the encoder had run dry.
  ///
  /// Separate from the two above, which count what the capture produced. The
  /// capture can be delivering perfectly while this starves, and it is only
  /// here that the difference shows.
  std::uint64_t screen_audio_mixed_blocks = 0;
  std::uint64_t screen_audio_starved_blocks = 0;
  /// Frames thrown away because that buffer went over its watermark, which is
  /// the encoder falling behind the capture rather than the other way round.
  std::uint64_t screen_audio_dropped_frames = 0;
  /// Blocks in which the mixer's limiter had to turn the gain down because the
  /// voice and the screen together did not fit a sample. Zero for a share that
  /// never trips it. docs/16-audio-plan.md, step 8.
  std::uint64_t screen_audio_limited_blocks = 0;

  /// What the capture device is delivering, in Hz. Zero before the first frame.
  /// A headset in communications mode delivers 16000, and then nothing above
  /// 8 kHz of the voice ever leaves the machine: the number is here so that the
  /// settings dialog can say so. docs/16-audio-plan.md, step 6.
  int microphone_sample_rate_hz = 0;
};

/// What the screen share is doing, section 22 of SPEC.md.
struct VideoStats {
  std::uint64_t frames_captured = 0;
  std::uint64_t frames_sent = 0;
  std::uint64_t frames_received = 0;
  /// Frames thrown away because the encoder could not keep up. See
  /// video::FrameQueue.
  std::uint64_t frames_dropped = 0;
  std::uint64_t keyframes_sent = 0;
  /// The size actually being sent, which is the monitor fitted into the
  /// configured ceiling.
  int send_width = 0;
  int send_height = 0;
  double send_fps = 0;
  double receive_fps = 0;
  double send_bitrate_kbps = 0;
  double receive_bitrate_kbps = 0;

  /// What the congestion controller believes the link can carry right now, in
  /// kbps. Property of the transport rather than of the video, since audio and
  /// video share one, but it lives here because video is what has to give way
  /// when the number falls.
  ///
  /// Zero until the first estimate exists, which needs a few seconds of
  /// traffic and feedback.
  double available_send_bitrate_kbps = 0;
  /// What the encoder is currently aiming at, in kbps, which is the estimate
  /// above clamped into the configured range.
  double target_bitrate_kbps = 0;

  /// What libwebrtc reports as the encoder actually running, for example
  /// "OpenH264" or "NVENC". Empty until the first frame is encoded.
  ///
  /// Read from the statistics rather than from what was asked for, because
  /// those are different things: a hardware encoder can be created, accept a
  /// stream and then be replaced by the software one halfway through.
  std::string encoder;
  /// libwebrtc's own verdict on whether that encoder is hardware.
  bool hardware_encoder = false;
};

enum class MediaState : std::uint8_t {
  New,
  Connecting,
  Connected,
  /// Recoverable: ICE is trying again.
  Disconnected,
  Failed,
  Closed,
};

/// Inline on purpose: the media implementation is a library of its own, and a
/// definition in the core would make the two depend on each other in a cycle.
[[nodiscard]] constexpr std::string_view to_string(MediaState state) noexcept {
  switch (state) {
    case MediaState::New:
      return "new";
    case MediaState::Connecting:
      return "connecting";
    case MediaState::Connected:
      return "connected";
    case MediaState::Disconnected:
      return "disconnected";
    case MediaState::Failed:
      return "failed";
    case MediaState::Closed:
      return "closed";
  }
  return "unknown";
}

/// The client end of the media connection with the SFU.
///
/// This interface exists so that everything above it can be built and tested
/// without libwebrtc. That matters twice over: libwebrtc is a 66 MB static
/// library that has to be built from source (see docs/07-webrtc-toolchain.md), and
/// the logic that drives a call is worth testing without a sound card, an ICE
/// exchange or a second machine.
///
/// The negotiation direction is fixed: the server offers and the client
/// answers, as described in section 4.3 of docs/06-protocol.md. There is no method
/// to create an offer, on purpose.
///
/// Implementations are thread safe, and every callback arrives on a media
/// thread, never on the caller's. Whatever the callbacks capture has to outlive
/// the session.
class MediaSession {
 public:
  struct Callbacks {
    /// The answer to send back to the SFU.
    std::function<void(std::string sdp)> on_local_answer;
    std::function<void(IceCandidate candidate)> on_local_candidate;
    /// A remote participant's audio started or stopped arriving. The user id
    /// comes from the msid the server put on the track.
    std::function<void(std::string user_id, bool active)> on_remote_audio;
    std::function<void(MediaState state)> on_state;
    /// Levels for the local microphone and every remote participant, several
    /// times a second. Section 8 of SPEC.md asks for the indicator; this is
    /// what feeds it.
    std::function<void(std::vector<AudioLevel> levels)> on_levels;

    /// A frame of the screen somebody else is sharing, already decoded and in
    /// BGRA. Arrives on a media thread, so whoever draws it has to get itself
    /// onto its own thread first.
    ///
    /// Who is sharing does not come from here. It arrives over signaling, as
    /// ScreenShareStarted, because the track carries whoever holds the floor
    /// rather than one fixed participant.
    std::function<void(video::VideoFrame frame)> on_remote_video;

    /// The screen share stopped without being asked to: permission refused,
    /// the monitor unplugged, the compositor gone.
    std::function<void(Error reason)> on_screen_share_ended;
  };

  MediaSession() = default;
  virtual ~MediaSession() = default;

  MediaSession(const MediaSession&) = delete;
  MediaSession& operator=(const MediaSession&) = delete;
  MediaSession(MediaSession&&) = delete;
  MediaSession& operator=(MediaSession&&) = delete;

  /// Applies the SFU's offer and produces the answer, which arrives through
  /// `on_local_answer`. Fails with `invalid_sdp` when the offer cannot be
  /// parsed or applied.
  [[nodiscard]] virtual Result<std::monostate> apply_remote_offer(const std::string& sdp) = 0;

  [[nodiscard]] virtual Result<std::monostate> add_remote_candidate(
      const IceCandidate& candidate) = 0;

  /// Stops sending the microphone without renegotiating.
  ///
  /// How depends on whether a screen audio share is on. Alone, the track is
  /// disabled and carries nothing, which is free. With a share on, disabling
  /// the track would take the shared sound with it, so the microphone is
  /// silenced inside the mixer instead and the track keeps carrying the share.
  virtual void set_microphone_muted(bool muted) = 0;
  [[nodiscard]] virtual bool microphone_muted() const = 0;

  /// Playback volume for one participant, from 0 to 1. Anything above 1 is
  /// amplification and is allowed up to 10, which is what WebRTC accepts.
  ///
  /// Fails with `unknown_participant` when no track carries that user yet.
  [[nodiscard]] virtual Result<std::monostate> set_participant_volume(const std::string& user_id,
                                                                      double volume) = 0;

  /// Switches the capture device without interrupting the call. Fails with
  /// `device_not_found`, and leaves the previous device in place.
  [[nodiscard]] virtual Result<std::monostate> set_input_device(const std::string& device_id) = 0;
  [[nodiscard]] virtual Result<std::monostate> set_output_device(const std::string& device_id) = 0;

  /// Starts sending `monitor_id`, or the primary monitor when it is empty.
  ///
  /// Needs no renegotiation: the m-line that carries the screen is part of the
  /// session from the moment it is created, and starting a share only starts
  /// filling it. That is what lets a share stop and start again without
  /// interrupting the call.
  ///
  /// Fails with `monitor_not_found` and `capture_unavailable`. A refusal that
  /// only arrives later, such as declining a Wayland portal, comes through
  /// `on_screen_share_ended`.
  [[nodiscard]] virtual Result<std::monostate> start_screen_share(
      const std::string& monitor_id) = 0;

  /// Stops capturing. The track stays in place and simply carries nothing,
  /// which is the same thing muting does to audio.
  virtual void stop_screen_share() = 0;

  [[nodiscard]] virtual bool sharing_screen() const = 0;

  /// Starts sending what the machine is playing, or what one application is
  /// playing, mixed into this participant's own audio track.
  ///
  /// Needs no renegotiation and no second track, for the reason
  /// docs/09-screen-audio.md gives at length: libwebrtc has one
  /// capture stream per factory, so a second local audio track would carry the
  /// same sound as the first. What it has instead is a hook that runs after the
  /// echo canceller and before the encoder, and that is where the mixing
  /// happens.
  ///
  /// `source_id` is a process id, and is read only in `LoopbackMode::Process`.
  /// In `LoopbackMode::System` the capture is everything the machine plays
  /// *except* this process, which is not a nicety: capturing our own output
  /// would send every other participant their own voice back, past the echo
  /// canceller, with nothing left to remove it.
  ///
  /// Fails with `capture_unavailable` on a platform or a Windows too old to do
  /// it, and with `invalid_value` for a process id of zero in `Process` mode.
  /// A capture that dies later - the application closing, the playback device
  /// disappearing - simply stops, and `screen_audio_active` goes false.
  [[nodiscard]] virtual Result<std::monostate> start_screen_audio(audio::LoopbackMode mode,
                                                                  std::uint32_t source_id) = 0;

  /// Stops mixing. The microphone carries on alone, and the track goes back to
  /// being muted by being disabled rather than by being silenced.
  virtual void stop_screen_audio() = 0;

  [[nodiscard]] virtual bool screen_audio_active() const = 0;

  /// How loud the shared screen's sound goes out, as a percentage of what the
  /// application is playing. 100 leaves it alone.
  ///
  /// Sender side, and it cannot be anything else. The screen audio rides inside
  /// this participant's own audio track, so by the time it reaches anybody it
  /// has been encoded together with the voice and no receiver can tell the two
  /// apart again. One person turning this down turns it down for the whole
  /// room, which is the honest consequence of the design in
  /// docs/09-screen-audio.md and not a limitation of this call.
  /// `set_participant_volume` is the receiving side and a different question.
  ///
  /// Remembered whether or not a share is running: it is the level the next one
  /// starts at, the same way the mode is.
  virtual void set_screen_audio_volume(int percent) = 0;

  [[nodiscard]] virtual int screen_audio_volume() const = 0;

  /// Turns the three blocks of the audio processing module on and off while the
  /// call runs.
  ///
  /// Applied to the module itself rather than through AudioOptions, which
  /// sounds like a detail and is not. The capture source is created once per
  /// process and cached - a second session gets the first one's options back -
  /// so options carried by a session only ever describe the state at the moment
  /// the first one was built. The module is the thing that is actually
  /// processing, and it takes a new configuration at any time.
  ///
  /// The level says how hard the suppressor bites while it is on; it is
  /// remembered while it is off, so turning the suppressor back on returns to
  /// the level it had.
  virtual void set_audio_processing(bool echo_cancellation, bool noise_suppression,
                                    bool automatic_gain_control,
                                    NoiseSuppressionLevel noise_suppression_level) = 0;

  /// The range the screen encoder may use, in kbps. Section 6 of SPEC.md puts
  /// it between 1.5 and 3 Mbps by default, and makes it configurable.
  ///
  /// Takes effect immediately, without renegotiating: it is a property of the
  /// sender, not of the session.
  [[nodiscard]] virtual Result<std::monostate> set_video_bitrate(int min_kbps, int max_kbps) = 0;

  /// The size and rate the screen is captured and sent at.
  ///
  /// Needs no renegotiation either, for the same reason starting a share does
  /// not: the m-line and the track are already there, and this only changes
  /// what is pushed into them. A share that is running is restarted on the
  /// same monitor, which costs a keyframe and the handful of frames the
  /// platform takes to hand over the first one.
  ///
  /// Fails with `invalid_value` for an empty size or a rate below one. When
  /// the restart itself fails, the share ends the way any other capture
  /// failure ends it, through `on_screen_share_ended`, so that nothing is left
  /// believing a share is still on.
  [[nodiscard]] virtual Result<std::monostate> set_capture_options(
      const video::ScreenCaptureOptions& options) = 0;

  /// The last stats snapshot. Collection is asynchronous, so this returns what
  /// was gathered most recently rather than blocking for a fresh reading.
  [[nodiscard]] virtual AudioStats stats() const = 0;

  [[nodiscard]] virtual VideoStats video_stats() const = 0;

  [[nodiscard]] virtual MediaState state() const = 0;

  virtual void close() = 0;
};

struct MediaSessionOptions {
  /// STUN and TURN URLs. TURN credentials go in `turn_username` and
  /// `turn_password` rather than inside the URL, so the password does not end
  /// up in a log line.
  std::vector<std::string> ice_servers;
  std::string turn_username;
  std::string turn_password;

  /// Section 9 of SPEC.md: Opus at 48 kHz, mono, 20 ms frames.
  int sample_rate_hz = 48000;
  int channels = 1;
  int frame_duration_ms = 20;

  bool echo_cancellation = true;
  bool noise_suppression = true;
  NoiseSuppressionLevel noise_suppression_level = NoiseSuppressionLevel::High;
  bool automatic_gain_control = true;

  /// How loud a screen share's sound starts, as a percentage. See
  /// MediaSession::set_screen_audio_volume.
  int screen_audio_volume_percent = 100;
  /// Whether the mixer's peak limiter runs. See
  /// audio::ScreenAudioMixer::set_limiter.
  bool screen_audio_limiter = true;

  /// Section 5.2 of SPEC.md: 1280x720 at 30 FPS.
  video::ScreenCaptureOptions capture;
  /// Section 6 of SPEC.md: 1.5 to 3 Mbps for the screen.
  ///
  /// The minimum is where the encoder starts and what it aims for on a healthy
  /// link, not a floor it may never go below. A floor would mean that a link
  /// which cannot carry 1.5 Mbps gets 1.5 Mbps anyway, which does not deliver
  /// a picture, it delivers congestion. What congestion control is allowed to
  /// fall to is `video_floor_bitrate_kbps`.
  int video_min_bitrate_kbps = 1500;
  int video_max_bitrate_kbps = 3000;
  /// The lowest the screen share is allowed to be squeezed to before the
  /// picture is worth less than the bandwidth. Below a few hundred kbps a
  /// 1280x720 screen is unreadable, and at that point the honest answer is a
  /// slow picture rather than a broken one.
  int video_floor_bitrate_kbps = 300;

  /// Empty means the system default.
  std::string input_device;
  std::string output_device;
};

/// Creates the libwebrtc backed session.
///
/// Fails with `media_unavailable` in a build made without libwebrtc, which is
/// how the server, the tests and a CI runner without the toolchain still get a
/// working client library.
[[nodiscard]] Result<std::unique_ptr<MediaSession>> create_media_session(
    const MediaSessionOptions& options, MediaSession::Callbacks callbacks);

/// True when this build can actually create a session.
[[nodiscard]] bool media_is_available() noexcept;

/// Whether the screen share is encoded by a card or by the processor.
///
/// Two separate questions, because they have different answers: a binary can
/// be built with a hardware backend and still run on a machine that has no
/// card, no driver, or a driver that does not match its own kernel module.
struct HardwareEncoding {
  bool compiled_in = false;
  bool available = false;
  /// "NVENC", "VAAPI", "VideoToolbox", and so on. Empty when none is compiled
  /// in.
  std::string implementation;
  /// Why it is unavailable, in words meant for whoever is wondering why their
  /// card is idle.
  std::string detail;
};

[[nodiscard]] HardwareEncoding hardware_encoding();

/// The devices the system offers, in the order the platform reports them.
///
/// Enumeration does not need a call in progress, which is why these are free
/// functions: the settings dialog has to work before anyone joins a room. They
/// fail with `media_unavailable` in a build without libwebrtc.
[[nodiscard]] Result<std::vector<AudioDevice>> input_devices();
[[nodiscard]] Result<std::vector<AudioDevice>> output_devices();

}  // namespace dv::client::media
