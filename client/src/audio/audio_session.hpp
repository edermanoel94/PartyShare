#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include <dv/core/result.hpp>

namespace dv::client::audio {

/// One ICE candidate, in the shape the signaling protocol carries it.
struct IceCandidate {
  std::string candidate;
  std::string sdp_mid;
  int sdp_mline_index = 0;

  friend bool operator==(const IceCandidate&, const IceCandidate&) = default;
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
class AudioSession {
 public:
  struct Callbacks {
    /// The answer to send back to the SFU.
    std::function<void(std::string sdp)> on_local_answer;
    std::function<void(IceCandidate candidate)> on_local_candidate;
    /// A remote participant's audio started or stopped arriving. The user id
    /// comes from the msid the server put on the track.
    std::function<void(std::string user_id, bool active)> on_remote_audio;
    std::function<void(MediaState state)> on_state;
  };

  AudioSession() = default;
  virtual ~AudioSession() = default;

  AudioSession(const AudioSession&) = delete;
  AudioSession& operator=(const AudioSession&) = delete;
  AudioSession(AudioSession&&) = delete;
  AudioSession& operator=(AudioSession&&) = delete;

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

  /// The last stats snapshot. Collection is asynchronous, so this returns what
  /// was gathered most recently rather than blocking for a fresh reading.
  [[nodiscard]] virtual AudioStats stats() const = 0;

  [[nodiscard]] virtual MediaState state() const = 0;

  virtual void close() = 0;
};

struct AudioSessionOptions {
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
[[nodiscard]] Result<std::unique_ptr<AudioSession>> create_audio_session(
    const AudioSessionOptions& options, AudioSession::Callbacks callbacks);

/// True when this build can actually create a session.
[[nodiscard]] bool media_is_available() noexcept;

}  // namespace dv::client::audio
