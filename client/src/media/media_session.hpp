#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <dv/core/result.hpp>

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

  /// True while the echo canceller is running on the captured audio.
  ///
  /// libwebrtc only reports echo return loss when the echo controller exists,
  /// so this is the one place where AEC3 being alive is observable from
  /// outside the media layer, rather than merely requested.
  bool echo_cancellation_active = false;
  /// How much of the echo the canceller is removing, in dB. Zero when it is
  /// not running, or when there is no echo to remove.
  double echo_return_loss_db = 0;
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
/// library that has to be built from source (see docs/webrtc-toolchain.md), and
/// the logic that drives a call is worth testing without a sound card, an ICE
/// exchange or a second machine.
///
/// The negotiation direction is fixed: the server offers and the client
/// answers, as described in section 4.3 of docs/protocol.md. There is no method
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

  /// Stops sending audio without renegotiating: the track stays in place and
  /// simply carries nothing.
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

  /// The last stats snapshot. Collection is asynchronous, so this returns what
  /// was gathered most recently rather than blocking for a fresh reading.
  [[nodiscard]] virtual AudioStats stats() const = 0;

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
  bool automatic_gain_control = true;

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

/// The devices the system offers, in the order the platform reports them.
///
/// Enumeration does not need a call in progress, which is why these are free
/// functions: the settings dialog has to work before anyone joins a room. They
/// fail with `media_unavailable` in a build without libwebrtc.
[[nodiscard]] Result<std::vector<AudioDevice>> input_devices();
[[nodiscard]] Result<std::vector<AudioDevice>> output_devices();

}  // namespace dv::client::media
