#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rtc/rtc.hpp>

#include <dv/protocol/message.hpp>

#include "signaling/hub.hpp"

namespace dv::server::sfu {

/// Forwards media between the participants of a room, section 12 of SPEC.md.
///
/// Audio today, video from M6. Both travel on the same connection per
/// participant, because a second one would be a second ICE negotiation and a
/// second DTLS handshake for no gain.
///
/// Topology: every participant has one PeerConnection with the server, never
/// with each other. A room of five participants is five connections, not
/// twenty, and one upstream per participant instead of four.
///
/// The server is always the offerer. It owns the mids, the SSRCs and the
/// payload types that way, which is what makes forwarding a matter of
/// rewriting a header instead of translating between two negotiations. A
/// participant only ever answers.
///
/// Each session carries:
///   - one recvonly track, the participant's own microphone
///   - one sendonly track per other participant, carrying that participant's
///     audio, with the msid set to their user id so the client knows whose
///     voice it is
///
/// Nothing is transcoded. Opus packets are forwarded as they arrive, with the
/// SSRC and the payload type rewritten to the ones negotiated on the outgoing
/// track.
class MediaRouter : public MediaSignals {
 public:
  struct Options {
    /// STUN and TURN URLs, in libdatachannel form, for example
    /// stun:stun.l.google.com:19302.
    std::vector<std::string> ice_servers;
    /// Opus at 48 kHz, as section 9 of SPEC.md requires. 111 is the payload
    /// type every browser and libwebrtc build uses for it.
    int opus_payload_type = 111;
  };

  /// Delivers one protocol frame to one participant. Called from the router's
  /// own thread, never from a media callback and never from the caller's
  /// thread, so it is free to take whatever locks the transport needs.
  using SignalHandler = std::function<void(const std::string& user_id, protocol::Message)>;

  explicit MediaRouter(Options options);
  ~MediaRouter() override;

  MediaRouter(const MediaRouter&) = delete;
  MediaRouter& operator=(const MediaRouter&) = delete;
  MediaRouter(MediaRouter&&) = delete;
  MediaRouter& operator=(MediaRouter&&) = delete;

  void on_signal(SignalHandler handler);

  // --- MediaSignals ----------------------------------------------------------

  void on_participant_joined(const std::string& room_id, const models::User& user) override;
  void on_participant_left(const std::string& room_id, const std::string& user_id) override;
  void on_media_signal(const std::string& room_id, const std::string& from_user_id,
                       const protocol::Message& message) override;

  // --- introspection, used by the tests and by the metrics log ---------------

  [[nodiscard]] std::size_t session_count() const;
  /// Number of outgoing tracks a participant has, one per other participant.
  [[nodiscard]] std::size_t outbound_track_count(const std::string& user_id) const;
  /// Audio packets received from participants since startup.
  [[nodiscard]] std::uint64_t audio_packets_received() const noexcept {
    return audio_packets_received_.load();
  }
  /// Audio packets forwarded since startup. One received packet becomes one
  /// forwarded packet per other participant in the room.
  [[nodiscard]] std::uint64_t audio_packets_forwarded() const noexcept {
    return audio_packets_forwarded_.load();
  }

 private:
  struct Outbound {
    std::shared_ptr<rtc::Track> track;
    std::uint32_t ssrc = 0;
    int payload_type = 0;
  };

  struct Session {
    std::string user_id;
    std::string room_id;
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::Track> inbound;
    /// Keyed by the user whose audio the track carries.
    std::unordered_map<std::string, Outbound> outbound;
    /// Mids are assigned by us because we write the offer. 0 is the inbound
    /// track, everything after it is an outbound one.
    unsigned next_mid = 0;
    /// A track added while an offer was still in flight. Renegotiating from a
    /// state that is not stable breaks the connection, so it waits.
    bool renegotiation_pending = false;
  };

  /// Must be called with `mutex_` held.
  Session* find_session(const std::string& user_id);
  const Session* find_session(const std::string& user_id) const;

  /// Adds a sendonly track carrying `source_user_id` to `session`, and asks for
  /// a renegotiation. Must be called with `mutex_` held.
  void add_outbound_track(Session& session, const std::string& source_user_id);

  /// Produces an offer when the session is in a state that allows it. Must be
  /// called with `mutex_` held.
  void negotiate(Session& session);

  void forward_audio(const std::string& from_user_id, const std::string& room_id,
                     rtc::binary packet);

  /// Queues a frame for the signal handler. Safe to call with `mutex_` held:
  /// the handler runs on the worker thread, with no lock of ours taken.
  void enqueue(const std::string& user_id, protocol::Message message);

  /// Queues work that needs `mutex_`, for use from a libdatachannel callback
  /// that may run inside a call that already holds it. Renegotiation is the
  /// case that matters: setLocalDescription reports the new signaling state
  /// synchronously, on the same thread.
  void post(std::function<void()> task);

  void worker_loop();

  Options options_;

  mutable std::mutex mutex_;
  std::unordered_map<std::string, Session> sessions_;  // by user id
  std::uint32_t next_ssrc_ = 1;

  /// Guards the queue and the handler. Never held while a task runs.
  std::mutex worker_mutex_;
  std::condition_variable worker_changed_;
  std::deque<std::function<void()>> tasks_;
  SignalHandler signal_handler_;
  bool stopping_ = false;
  std::thread worker_thread_;

  std::atomic<std::uint64_t> audio_packets_received_{0};
  std::atomic<std::uint64_t> audio_packets_forwarded_{0};
};

}  // namespace dv::server::sfu
