#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rtc/rtc.hpp>

#include <dv/protocol/message.hpp>

#include "sfu/atomic_shared_ptr.hpp"
#include "sfu/audio_description.hpp"
#include "sfu/loss_repair.hpp"
#include "sfu/video_feedback.hpp"
#include "sfu/video_stitcher.hpp"
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
/// Nothing is transcoded. Audio packets are forwarded as they arrive, with the
/// SSRC rewritten to the one negotiated on the outgoing track. The payload
/// type travels untouched: this server writes every offer, so the numbers are
/// the same on every leg, and a packet that arrived as RED has to leave as RED.
class MediaRouter : public MediaSignals {
 public:
  struct Options {
    /// STUN and TURN URLs, in libdatachannel form, for example
    /// stun:stun.l.google.com:19302.
    std::vector<std::string> ice_servers;
    /// The UDP range ICE binds in. Zero on both means the system's ephemeral
    /// range, which is what happened before this option existed. See
    /// dv::config::NetworkConfig for what a range costs and buys.
    std::uint16_t port_range_begin = 0;
    std::uint16_t port_range_end = 0;
    /// Opus at 48 kHz, as section 9 of SPEC.md requires. 111 is the payload
    /// type every browser and libwebrtc build uses for it.
    int opus_payload_type = 111;
    /// The ceiling written into the Opus fmtp of every audio m-line, in kbps.
    ///
    /// It is what the offer asks a sender to stay under, and this server is
    /// always the offerer, so it is the only place the number can come from.
    /// Until the screen learned to carry sound it did not matter and was not
    /// applied at all: voice fits in 48 kbps and nobody was near the ceiling.
    /// Music is not voice, and a share is stereo. See
    /// docs/09-screen-audio.md, section 5.
    int opus_max_bitrate_kbps = 96;
    /// RED next to Opus on every audio m-line, RFC 2198, or nothing for Opus
    /// alone. See sfu/audio_description.hpp for what it buys and what it
    /// costs; `[audio] redundancy = false` in the server's config is what
    /// empties it.
    std::optional<int> red_payload_type = 63;
    /// Retransmission on every audio leg: `nack` in the offer, a NACK
    /// responder on every outgoing audio track and a sfu::LossRepair on every
    /// incoming one. `[audio] retransmission = false` in the server's config
    /// is what turns it off. See docs/16-audio-plan.md, step 2.
    bool audio_nack = true;
    /// H.264, section 6 of SPEC.md. 96 is the first dynamic payload type and
    /// what everything in this space uses for it.
    int h264_payload_type = 96;
    /// The range a screen share is held to, and how the SFU moves inside it.
    /// See sfu/bandwidth_estimator.hpp.
    BandwidthEstimator::Options bandwidth;
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

  void on_participant_joined(const std::string& room_id, const std::string& room_name,
                             const models::User& user, const std::string& user_label) override;
  void on_participant_left(const std::string& room_id, const std::string& user_id) override;
  void on_media_signal(const std::string& room_id, const std::string& from_user_id,
                       const protocol::Message& message) override;

  // --- introspection, used by the tests and by the metrics log ---------------

  [[nodiscard]] std::size_t session_count() const;
  /// Number of outgoing audio tracks a participant has, one per other
  /// participant.
  [[nodiscard]] std::size_t outbound_track_count(const std::string& user_id) const;

  /// True once the participant has both video m-lines, the one that carries
  /// their own screen up and the one that brings the shared screen down.
  [[nodiscard]] bool has_video_tracks(const std::string& user_id) const;
  /// Audio packets received from participants since startup.
  [[nodiscard]] std::uint64_t audio_packets_received() const noexcept {
    return audio_packets_received_.load();
  }
  /// Audio packets forwarded since startup. One received packet becomes one
  /// forwarded packet per other participant in the room.
  [[nodiscard]] std::uint64_t audio_packets_forwarded() const noexcept {
    return audio_packets_forwarded_.load();
  }
  /// Of the audio packets received, those that arrived as RED. It is the only
  /// place the redundancy is observable from outside: a client's statistics
  /// name the codec RED carries, never RED itself.
  [[nodiscard]] std::uint64_t audio_red_packets_received() const noexcept {
    return audio_red_packets_received_.load();
  }
  /// NACKs that listeners sent back on outgoing audio tracks, which
  /// libdatachannel answers out of its cache. The other half of the audio
  /// repair: `audio_repair_stats` is what this server asks senders for, this
  /// is what listeners ask this server for.
  [[nodiscard]] std::uint64_t audio_nacks_received() const noexcept {
    return audio_nacks_received_.load();
  }

  /// What the repair of the incoming audio is doing, summed over every
  /// participant. See sfu/loss_repair.hpp.
  struct AudioRepairStats {
    std::uint64_t requests_sent = 0;
    std::uint64_t packets_missing = 0;
    std::uint64_t packets_repaired = 0;
  };
  [[nodiscard]] AudioRepairStats audio_repair_stats() const;
  /// Video packets received from whoever is sharing a screen.
  [[nodiscard]] std::uint64_t video_packets_received() const noexcept {
    return video_packets_received_.load();
  }
  [[nodiscard]] std::uint64_t video_packets_forwarded() const noexcept {
    return video_packets_forwarded_.load();
  }
  /// Keyframe requests passed from a viewer up to the sharer. Section 5.2 of
  /// SPEC.md needs these: a participant who joins mid transmission sees
  /// nothing until the sender produces an intra frame.
  [[nodiscard]] std::uint64_t keyframe_requests_forwarded() const noexcept {
    return keyframe_requests_forwarded_.load();
  }

  /// What the repair of the incoming screen share is doing, summed over the
  /// participants sending one. See sfu/video_feedback.hpp.
  struct VideoRepairStats {
    std::uint64_t requests_sent = 0;
    std::uint64_t packets_missing = 0;
    std::uint64_t packets_repaired = 0;
    /// The highest target any sharer is currently being asked to aim for, in
    /// kbps. Zero while nobody is sharing.
    int target_kbps = 0;
    /// The lowest a viewer has said it can take, in kbps, which is the cap on
    /// the number above. Zero while no viewer has said anything.
    int viewer_ceiling_kbps = 0;
    /// How many REMB reports arrived from viewers **and were folded in**.
    ///
    /// The second half is what makes this usable as a signal to wait on: a
    /// non-zero count means the ceiling beside it already accounts for that
    /// report. It counted arrivals rather than completions once, and a test
    /// that waited on it read a ceiling from before the report it had just
    /// been told about.
    std::uint64_t viewer_reports_received = 0;
  };
  [[nodiscard]] VideoRepairStats video_repair_stats() const;

 private:
  struct Outbound {
    std::shared_ptr<rtc::Track> track;
    std::uint32_t ssrc = 0;
    int payload_type = 0;
    /// Only on the video track, and shared rather than copied: an Outbound is
    /// copied into the routing table, and every copy has to be rewriting the
    /// same series. Null on an audio track, which needs none of it - an audio
    /// track carries one participant for its whole life, so its sequence space
    /// never changes hands. See sfu/video_stitcher.hpp.
    std::shared_ptr<VideoStitcher> stitcher;
  };

  /// Where one participant's media has to go, and what to rewrite it to.
  struct Route {
    std::string room_id;
    /// The names the forwarding path writes its one log line with, carried
    /// here for the reason everything else in this table is: that path takes
    /// no lock, so it cannot read `sessions_` to ask.
    std::string room_label;
    std::string user_label;
    std::vector<Outbound> audio;
    std::vector<Outbound> video;
  };

  /// Everything the forwarding path needs, by source participant, plus the
  /// inbound video tracks by room for keyframe requests.
  ///
  /// This exists so that forwarding takes no lock at all.
  ///
  /// libdatachannel delivers media on its own thread while holding an internal
  /// lock on the peer connection, and it takes that same lock from addTrack.
  /// A router that took its own mutex on the forwarding path would therefore
  /// have two threads acquiring two locks in opposite orders: one arriving
  /// through a join and one through an RTP packet. That deadlocks the whole
  /// server, and needs no more than a packet landing while somebody joins.
  ///
  /// So the table is built under `mutex_` and published as a whole. Readers
  /// take a copy of the pointer and walk a structure nobody will modify.
  struct RoutingTable {
    std::unordered_map<std::string, Route> by_source;
    std::unordered_map<std::string, std::vector<std::shared_ptr<rtc::Track>>> video_inbound_by_room;
    /// The feedback handler of every participant who could be sharing, by
    /// room. Published here rather than looked up in `sessions_` because a
    /// viewer's REMB arrives on a libdatachannel thread that already holds the
    /// peer connection's own lock, and taking `mutex_` there is the deadlock
    /// this table exists to avoid.
    std::unordered_map<std::string, std::vector<std::shared_ptr<VideoFeedback>>>
        video_feedback_by_room;
  };

  struct Session {
    std::string user_id;
    std::string room_id;
    /// How this participant and their room read in a log line, as the Hub
    /// resolved them when the session was created.
    ///
    /// Copied rather than looked up later, and not only to save the lookup:
    /// this class runs on its own thread and holds no reference to anything
    /// that could answer the question. A session also outlives the account
    /// behind it often enough -- somebody deleted mid-call is precisely the
    /// case these lines get read for -- that asking afterwards would print an
    /// identifier exactly when a name was worth having.
    std::string user_label;
    std::string room_label;
    std::shared_ptr<rtc::PeerConnection> connection;
    std::shared_ptr<rtc::Track> inbound;
    /// Asks this participant for the audio packets that did not arrive. Null
    /// while `Options::audio_nack` is off. See sfu/loss_repair.hpp.
    std::shared_ptr<LossRepair> audio_repair;
    /// Keyed by the user whose audio the track carries.
    std::unordered_map<std::string, Outbound> outbound;

    /// This participant's own screen, coming up. Present from the moment the
    /// session exists, and silent until they start sharing.
    std::shared_ptr<rtc::Track> video_inbound;
    /// The shared screen, going down. There is one of these rather than one
    /// per participant because section 5.2 of SPEC.md allows a single sharer
    /// at a time, and RoomManager enforces it.
    ///
    /// Both exist from the start so that starting or stopping a share needs no
    /// renegotiation. That is what makes "parar e reiniciar o compartilhamento
    /// funciona sem reiniciar a chamada" true by construction rather than by
    /// getting a mid-call offer right.
    std::optional<Outbound> video_outbound;
    /// Tells the sharer what to resend and what to aim for. See
    /// sfu/video_feedback.hpp.
    std::shared_ptr<VideoFeedback> video_feedback;
    /// Mids are assigned by us because we write the offer. 0 is the inbound
    /// track, everything after it is an outbound one.
    unsigned next_mid = 0;
    /// A track added while an offer was still in flight. Renegotiating from a
    /// state that is not stable breaks the connection, so it waits.
    bool renegotiation_pending = false;
  };

  /// A viewer said how much it can take. Called from a libdatachannel thread,
  /// and must not touch `mutex_`.
  void note_viewer_bandwidth(const std::string& viewer_id, const std::string& room_id,
                             unsigned int bitrate_bps);

  /// Must be called with `mutex_` held.
  Session* find_session(const std::string& user_id);
  const Session* find_session(const std::string& user_id) const;

  /// Adds a sendonly track carrying `source_user_id` to `session`, and asks for
  /// a renegotiation. Must be called with `mutex_` held.
  void add_outbound_track(Session& session, const std::string& source_user_id);

  /// Adds the sendonly video track that carries the shared screen. Must be
  /// called with `mutex_` held, before the session is negotiated for the first
  /// time.
  void add_video_outbound_track(Session& session);

  /// Produces an offer when the session is in a state that allows it. Must be
  /// called with `mutex_` held.
  void negotiate(Session& session);

  /// Rebuilds the routing table from the sessions. Must be called with
  /// `mutex_` held, after any change to who is in a room or which tracks they
  /// have.
  void publish_routes();

  /// The current table. Takes no lock: the pointer is read atomically and what
  /// it points at is never modified.
  [[nodiscard]] std::shared_ptr<const RoutingTable> routes() const;

  void forward_audio(const std::string& from_user_id, const std::string& room_id,
                     const rtc::binary& packet);

  void forward_video(const std::string& from_user_id, const std::string& room_id,
                     const rtc::binary& packet);

  /// A viewer asked for an intra frame. Passes the request up to whoever is
  /// sending video in that room.
  void request_keyframe_from_sharer(const std::string& room_id);

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

  /// Replaced wholesale under `mutex_`, read without it. See RoutingTable.
  /// Not `std::atomic<std::shared_ptr<...>>` directly: the libc++ that ships
  /// with Xcode has no such specialisation. See sfu/atomic_shared_ptr.hpp.
  AtomicSharedPtr<const RoutingTable> routes_{std::make_shared<const RoutingTable>()};

  /// Guards the queue and the handler. Never held while a task runs.
  std::mutex worker_mutex_;
  std::condition_variable worker_changed_;
  std::deque<std::function<void()>> tasks_;
  SignalHandler signal_handler_;
  bool stopping_ = false;
  std::thread worker_thread_;

  std::atomic<std::uint64_t> audio_packets_received_{0};
  std::atomic<std::uint64_t> audio_packets_forwarded_{0};
  std::atomic<std::uint64_t> audio_red_packets_received_{0};
  std::atomic<std::uint64_t> audio_nacks_received_{0};
  std::atomic<std::uint64_t> video_packets_received_{0};
  std::atomic<std::uint64_t> video_packets_forwarded_{0};
  std::atomic<std::uint64_t> keyframe_requests_forwarded_{0};

  /// What each viewer last said it could take, in kbps, and the smallest of
  /// them per room. Its own mutex because it is written from libdatachannel
  /// threads that must never wait on `mutex_`.
  mutable std::mutex bandwidth_mutex_;
  std::unordered_map<std::string, int> viewer_bandwidth_kbps_;
  std::unordered_map<std::string, std::string> viewer_room_;
  std::atomic<std::uint64_t> viewer_reports_received_{0};
  std::atomic<int> viewer_ceiling_kbps_{0};
};

}  // namespace dv::server::sfu
