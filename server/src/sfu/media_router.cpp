#include "sfu/media_router.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

#include <dv/logging/logger.hpp>

namespace dv::server::sfu {
namespace {

/// Opus is always 48 kHz on the wire, section 9 of SPEC.md.
constexpr int kOpusClockRate = 48000;

/// RFC 6464. Senders put the loudness of each packet in the RTP header, and
/// the SFU forwards those bytes untouched, so a participant can tell who is
/// speaking without decoding anything or polling statistics.
///
/// The same id is declared in both directions on purpose: a forwarded packet
/// keeps the id the sender wrote, so the receiver has to read it under that
/// same id.
constexpr int kAudioLevelExtensionId = 1;
constexpr const char* kAudioLevelExtensionUri = "urn:ietf:params:rtp-hdrext:ssrc-audio-level";

/// True when the RTP header says the packet carries padding.
///
/// Such a packet is dropped rather than forwarded, because libdatachannel's
/// sender report builder asserts on one and takes the whole server down with
/// it. A participant that sends padded packets therefore damages its own
/// picture, which the retransmission path then repairs, instead of ending
/// everybody's call.
///
/// This is not hypothetical. Negotiating the abs-send-time extension, which is
/// what a viewer needs before it can report a delay based estimate, makes
/// libwebrtc probe for bandwidth, and probing is padding. The extension is not
/// negotiated for that reason, and this guard is here because a client we did
/// not write can still send one.
[[nodiscard]] bool is_padded(const rtc::binary& packet) {
  return reinterpret_cast<const rtc::RtpHeader*>(packet.data())->padding();
}

/// H.264 RTP runs on a 90 kHz clock, which is what every video profile uses.
constexpr int kVideoClockRate = 90000;

[[nodiscard]] rtc::Configuration make_configuration(const MediaRouter::Options& options) {
  rtc::Configuration configuration;
  for (const std::string& server : options.ice_servers) {
    if (server.empty()) {
      continue;
    }
    try {
      configuration.iceServers.emplace_back(server);
    } catch (const std::exception& error) {
      // A malformed URL must not take the server down: ICE still works with
      // host candidates on a local network.
      DV_LOG_WARN("SFU: ignoring unusable ICE server '{}': {}", server, error.what());
    }
  }
  // Left alone, libdatachannel's own defaults mean "any ephemeral port", which
  // is only firewallable by opening the whole ephemeral range. A configured
  // range narrows that to something an operator can write down, at one port
  // per participant.
  if (options.port_range_begin != 0 && options.port_range_end != 0) {
    configuration.portRangeBegin = options.port_range_begin;
    configuration.portRangeEnd = options.port_range_end;
  }
  return configuration;
}

}  // namespace

MediaRouter::MediaRouter(Options options) : options_(std::move(options)) {
  worker_thread_ = std::thread([this] { worker_loop(); });
}

MediaRouter::~MediaRouter() {
  {
    const std::lock_guard<std::mutex> lock(worker_mutex_);
    stopping_ = true;
    worker_changed_.notify_all();
  }
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }

  // Sessions are destroyed outside the lock: closing a PeerConnection waits for
  // its callbacks, and those callbacks take this mutex.
  std::unordered_map<std::string, Session> sessions;
  {
    const std::lock_guard<std::mutex> lock(mutex_);
    sessions.swap(sessions_);
  }
  for (auto& [user_id, session] : sessions) {
    session.connection->close();
  }
}

void MediaRouter::on_signal(SignalHandler handler) {
  const std::lock_guard<std::mutex> lock(worker_mutex_);
  signal_handler_ = std::move(handler);
}

void MediaRouter::on_participant_joined(const std::string& room_id, const models::User& user) {
  const std::lock_guard<std::mutex> lock(mutex_);

  if (sessions_.contains(user.id)) {
    // Rejoining, or a reconnection the signaling layer saw as a fresh join.
    // The old connection is worthless now, and the peers' tracks for it are
    // rebuilt below.
    DV_LOG_INFO("SFU: replacing the existing session of {}", user.id);
    Session old = std::move(sessions_.at(user.id));
    sessions_.erase(user.id);
    old.connection->close();
  }

  Session session;
  session.user_id = user.id;
  session.room_id = room_id;
  session.connection = std::make_shared<rtc::PeerConnection>(make_configuration(options_));

  const std::string user_id = user.id;

  // Installed before anything can produce one. libdatachannel delivers these
  // from its own threads, and they must never take `mutex_` on a path that
  // could already hold it, which is why they only enqueue.
  session.connection->onLocalDescription(
      [this, user_id, room_id](const rtc::Description& description) {
        if (description.type() != rtc::Description::Type::Offer) {
          // The SFU is always the offerer, so anything else is a bug on our side.
          DV_LOG_WARN("SFU: ignoring a local {} for {}", description.typeString(), user_id);
          return;
        }
        // The offer is the contract with the client: which codecs, which
        // extensions, and which feedback the SFU is willing to act on. One
        // environment variable away rather than a rebuild, because the
        // question "was nack negotiated" comes up every time video misbehaves.
        if (std::getenv("DV_DUMP_SDP") != nullptr) {
          DV_LOG_INFO("SFU: offer to {}\n{}", user_id, std::string(description));
        }
        enqueue(user_id, protocol::Offer{.room_id = room_id,
                                         .from_user_id = std::string(protocol::kSfuUserId),
                                         .to_user_id = user_id,
                                         .sdp = std::string(description)});
      });

  session.connection->onLocalCandidate([this, user_id, room_id](const rtc::Candidate& candidate) {
    enqueue(user_id, protocol::IceCandidate{.room_id = room_id,
                                            .from_user_id = std::string(protocol::kSfuUserId),
                                            .to_user_id = user_id,
                                            .candidate = std::string(candidate),
                                            .sdp_mid = candidate.mid(),
                                            .sdp_mline_index = 0});
  });

  // maybe_unused because the only use of `state` is the debug log below, and
  // SPDLOG_DEBUG compiles to nothing in a release build.
  session.connection->onStateChange([user_id]([[maybe_unused]] rtc::PeerConnection::State state) {
    DV_LOG_DEBUG("SFU: connection of {} is now {}", user_id, static_cast<int>(state));
  });

  session.connection->onSignalingStateChange(
      [this, user_id](rtc::PeerConnection::SignalingState state) {
        if (state != rtc::PeerConnection::SignalingState::Stable) {
          return;
        }
        // A track added while an offer was in flight could not be negotiated then,
        // and this is the moment it becomes possible again.
        //
        // Posted rather than done here: libdatachannel reports the state change on
        // the thread that caused it, which is usually a thread already inside
        // negotiate() with `mutex_` held.
        post([this, user_id] {
          const std::lock_guard<std::mutex> pending_lock(mutex_);
          Session* pending = find_session(user_id);
          if (pending != nullptr && pending->renegotiation_pending) {
            negotiate(*pending);
          }
        });
      });

  // The participant's own microphone.
  rtc::Description::Audio inbound(std::to_string(session.next_mid++),
                                  rtc::Description::Direction::RecvOnly);
  inbound.addOpusCodec(options_.opus_payload_type);
  inbound.addExtMap(
      rtc::Description::Entry::ExtMap(kAudioLevelExtensionId, kAudioLevelExtensionUri));
  session.inbound = session.connection->addTrack(inbound);

  // Generates the receiver reports the sender needs to estimate loss and RTT.
  session.inbound->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

  session.inbound->onMessage(
      [this, user_id, room_id](const rtc::binary& packet) {
        forward_audio(user_id, room_id, packet);
      },
      [](const rtc::string&) {
        // Text on a media track is not part of anything we speak.
      });

  // The participant's own screen, coming up. Created now and left silent, so
  // that starting to share later costs no renegotiation.
  rtc::Description::Video video_inbound(std::to_string(session.next_mid++),
                                        rtc::Description::Direction::RecvOnly);
  video_inbound.addH264Codec(options_.h264_payload_type);
  session.video_inbound = session.connection->addTrack(video_inbound);
  auto video_receiving = std::make_shared<rtc::RtcpReceivingSession>();
  // Repairs the incoming screen share instead of letting the loss travel on to
  // every viewer, and tells the sharer how much the link can carry. See
  // sfu/video_feedback.hpp.
  session.video_feedback =
      std::make_shared<VideoFeedback>(VideoFeedback::Options{.bandwidth = options_.bandwidth});
  video_receiving->addToChain(session.video_feedback);
  session.video_inbound->setMediaHandler(video_receiving);
  session.video_inbound->onMessage(
      [this, user_id, room_id](const rtc::binary& packet) {
        forward_video(user_id, room_id, packet);
      },
      [](const rtc::string&) {});

  // The shared screen, going down.
  add_video_outbound_track(session);

  auto [it, inserted] = sessions_.emplace(user.id, std::move(session));
  Session& created = it->second;

  // Wire the newcomer to everyone already in the room, in both directions.
  for (auto& [other_id, other] : sessions_) {
    if (other_id == user.id || other.room_id != room_id) {
      continue;
    }
    add_outbound_track(created, other_id);
    add_outbound_track(other, user.id);
    negotiate(other);
  }

  publish_routes();
  negotiate(created);
  DV_LOG_INFO("SFU: session for {} in room {} ({} sessions)", user.id, room_id, sessions_.size());
}

void MediaRouter::on_participant_left(const std::string& room_id, const std::string& user_id) {
  std::shared_ptr<rtc::PeerConnection> closing;
  {
    const std::lock_guard<std::mutex> lock(mutex_);

    const auto it = sessions_.find(user_id);
    if (it == sessions_.end()) {
      return;
    }
    closing = it->second.connection;
    sessions_.erase(it);

    // Drop the track that carried this participant everywhere else, and
    // renegotiate so the peers stop expecting it.
    for (auto& [other_id, other] : sessions_) {
      if (other.room_id != room_id) {
        continue;
      }
      const auto outbound = other.outbound.find(user_id);
      if (outbound == other.outbound.end()) {
        continue;
      }
      outbound->second.track->close();
      other.outbound.erase(outbound);
      negotiate(other);
    }

    publish_routes();
  }

  // Closed outside the lock: it waits for the callbacks, which take the lock.
  closing->close();
  DV_LOG_INFO("SFU: session of {} in room {} closed", user_id, room_id);
}

void MediaRouter::on_media_signal(const std::string& room_id, const std::string& from_user_id,
                                  const protocol::Message& message) {
  const std::lock_guard<std::mutex> lock(mutex_);

  Session* session = find_session(from_user_id);
  if (session == nullptr || session->room_id != room_id) {
    DV_LOG_WARN("SFU: {} sent media signaling with no session in room {}", from_user_id, room_id);
    return;
  }

  std::visit(
      [&](const auto& value) {
        using T = std::decay_t<decltype(value)>;

        if constexpr (std::is_same_v<T, protocol::Answer>) {
          // The other half of the DV_DUMP_SDP pair above: an offer nobody can
          // see the reply to only tells half of why media is not flowing.
          if (std::getenv("DV_DUMP_SDP") != nullptr) {
            DV_LOG_INFO("SFU: answer from {}\n{}", from_user_id, value.sdp);
          }
          try {
            session->connection->setRemoteDescription(
                rtc::Description(value.sdp, rtc::Description::Type::Answer));
          } catch (const std::exception& error) {
            DV_LOG_WARN("SFU: rejected the answer of {}: {}", from_user_id, error.what());
          }

        } else if constexpr (std::is_same_v<T, protocol::IceCandidate>) {
          try {
            session->connection->addRemoteCandidate(rtc::Candidate(value.candidate, value.sdp_mid));
          } catch (const std::exception& error) {
            DV_LOG_WARN("SFU: rejected a candidate of {}: {}", from_user_id, error.what());
          }

        } else if constexpr (std::is_same_v<T, protocol::Offer>) {
          // The SFU offers, the participant answers. An offer from a
          // participant means the two sides disagree about that.
          DV_LOG_WARN("SFU: ignoring an offer from {}, the server is the offerer", from_user_id);

        } else {
          DV_LOG_WARN("SFU: ignoring an unexpected frame from {}", from_user_id);
        }
      },
      message);
}

std::size_t MediaRouter::session_count() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

std::size_t MediaRouter::outbound_track_count(const std::string& user_id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const Session* session = find_session(user_id);
  return session == nullptr ? 0 : session->outbound.size();
}

bool MediaRouter::has_video_tracks(const std::string& user_id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const Session* session = find_session(user_id);
  return session != nullptr && session->video_inbound != nullptr &&
         session->video_outbound.has_value();
}

void MediaRouter::note_viewer_bandwidth(const std::string& viewer_id, const std::string& room_id,
                                        unsigned int bitrate_bps) {
  const auto kbps = static_cast<int>(bitrate_bps / 1000);
  if (kbps <= 0) {
    return;
  }
  viewer_reports_received_.fetch_add(1, std::memory_order_relaxed);

  int smallest = kbps;
  {
    const std::lock_guard<std::mutex> lock(bandwidth_mutex_);
    viewer_bandwidth_kbps_[viewer_id] = kbps;
    viewer_room_[viewer_id] = room_id;
    for (const auto& [other_id, other_kbps] : viewer_bandwidth_kbps_) {
      const auto room = viewer_room_.find(other_id);
      if (room == viewer_room_.end() || room->second != room_id) {
        continue;
      }
      smallest = std::min(smallest, other_kbps);
    }
  }
  viewer_ceiling_kbps_.store(smallest, std::memory_order_relaxed);

  // Through the published table, never through `sessions_`: this runs on a
  // libdatachannel thread that already holds the peer connection's lock.
  const std::shared_ptr<const RoutingTable> table = routes();
  const auto feedback = table->video_feedback_by_room.find(room_id);
  if (feedback == table->video_feedback_by_room.end()) {
    return;
  }
  for (const auto& handler : feedback->second) {
    handler->set_bandwidth_ceiling(smallest);
  }
}

MediaRouter::VideoRepairStats MediaRouter::video_repair_stats() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  VideoRepairStats total;
  for (const auto& [user_id, session] : sessions_) {
    if (session.video_feedback == nullptr) {
      continue;
    }
    total.requests_sent += session.video_feedback->requests_sent();
    total.packets_missing += session.video_feedback->packets_missing();
    total.packets_repaired += session.video_feedback->packets_repaired();
    total.target_kbps = std::max(total.target_kbps, session.video_feedback->target_kbps());
  }
  total.viewer_ceiling_kbps = viewer_ceiling_kbps_.load(std::memory_order_relaxed);
  total.viewer_reports_received = viewer_reports_received_.load(std::memory_order_relaxed);
  return total;
}

MediaRouter::Session* MediaRouter::find_session(const std::string& user_id) {
  const auto it = sessions_.find(user_id);
  return it == sessions_.end() ? nullptr : &it->second;
}

const MediaRouter::Session* MediaRouter::find_session(const std::string& user_id) const {
  const auto it = sessions_.find(user_id);
  return it == sessions_.end() ? nullptr : &it->second;
}

void MediaRouter::add_outbound_track(Session& session, const std::string& source_user_id) {
  if (session.outbound.contains(source_user_id)) {
    return;
  }

  const std::uint32_t ssrc = next_ssrc_++;

  rtc::Description::Audio media(std::to_string(session.next_mid++),
                                rtc::Description::Direction::SendOnly);
  media.addOpusCodec(options_.opus_payload_type);
  media.addExtMap(rtc::Description::Entry::ExtMap(kAudioLevelExtensionId, kAudioLevelExtensionUri));

  // The msid is how the receiver learns whose voice this is. Without it a
  // client would get N indistinguishable audio tracks.
  media.addSSRC(ssrc, "sfu-" + source_user_id, source_user_id, source_user_id);

  Outbound outbound;
  outbound.ssrc = ssrc;
  outbound.payload_type = options_.opus_payload_type;
  outbound.track = session.connection->addTrack(media);

  // Sender reports let the receiver measure jitter and round trip time, which
  // is what the metrics of section 22 of SPEC.md are read from.
  auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
      ssrc, source_user_id, static_cast<std::uint8_t>(options_.opus_payload_type), kOpusClockRate);
  outbound.track->setMediaHandler(std::make_shared<rtc::RtcpSrReporter>(rtp_config));

  session.outbound.emplace(source_user_id, std::move(outbound));
  session.renegotiation_pending = true;
}

// Reads nothing from the router today, only from the session handed to it. It
// stays a member because it is part of the router's own protocol with itself,
// and the contract about `mutex_` in the header is about the caller.
// NOLINTNEXTLINE(readability-convert-member-functions-to-static)
void MediaRouter::negotiate(Session& session) {
  if (!session.renegotiation_pending && session.connection->localDescription().has_value()) {
    return;
  }

  // Renegotiating from anything but a stable state corrupts the session. The
  // signaling state callback picks this up again once the current exchange is
  // over.
  if (session.connection->signalingState() != rtc::PeerConnection::SignalingState::Stable) {
    session.renegotiation_pending = true;
    return;
  }

  session.renegotiation_pending = false;
  try {
    session.connection->setLocalDescription(rtc::Description::Type::Offer);
  } catch (const std::exception& error) {
    DV_LOG_ERROR("SFU: could not offer to {}: {}", session.user_id, error.what());
  }
}

void MediaRouter::add_video_outbound_track(Session& session) {
  const std::uint32_t ssrc = next_ssrc_++;

  rtc::Description::Video media(std::to_string(session.next_mid++),
                                rtc::Description::Direction::SendOnly);
  media.addH264Codec(options_.h264_payload_type);

  // A fixed msid, because unlike audio this track does not belong to one
  // participant: it carries whoever happens to be sharing. Who that is comes
  // over signaling, as ScreenShareStarted, which the hub already broadcasts.
  const std::string label = "sfu-screen";
  media.addSSRC(ssrc, label, label, label);

  Outbound outbound;
  outbound.ssrc = ssrc;
  outbound.payload_type = options_.h264_payload_type;
  outbound.track = session.connection->addTrack(media);

  auto rtp_config = std::make_shared<rtc::RtpPacketizationConfig>(
      ssrc, label, static_cast<std::uint8_t>(options_.h264_payload_type), kVideoClockRate);
  auto reporter = std::make_shared<rtc::RtcpSrReporter>(rtp_config);

  // A viewer that needs an intra frame says so on the track it is receiving
  // on, and the only place that can produce one is the sender. Nothing between
  // the two decodes anything, so the request has to be carried across.
  const std::string room_id = session.room_id;
  reporter->addToChain(std::make_shared<rtc::PliHandler>(
      [this, room_id] { request_keyframe_from_sharer(room_id); }));

  // Answers a viewer that says it missed packet number n by sending that
  // packet again, out of a cache of the last few hundred.
  //
  // Without it the only repair a viewer has is to ask for a new intra frame,
  // and on a lossy link that does not converge: an intra frame of a 720p
  // screen is upwards of a hundred packets, so at 5% loss it almost never
  // arrives whole, and the viewer asks again. Measured on this project, a
  // screen share on a 5% loss link delivered four frames in fifteen seconds
  // while the SFU carried eight keyframe requests. Retransmission is what
  // turns that into a picture that degrades instead of freezing.
  reporter->addToChain(std::make_shared<rtc::RtcpNackResponder>());

  // What this viewer says its own link can carry. The SFU is the only party
  // that hears all of them, and it is the sharer, not the viewer, who has to
  // act on the slowest one.
  const std::string viewer_id = session.user_id;
  reporter->addToChain(
      std::make_shared<rtc::RembHandler>([this, viewer_id, room_id](unsigned int bitrate_bps) {
        note_viewer_bandwidth(viewer_id, room_id, bitrate_bps);
      }));
  outbound.track->setMediaHandler(reporter);

  session.video_outbound = std::move(outbound);
  session.renegotiation_pending = true;
}

void MediaRouter::forward_video(const std::string& from_user_id, const std::string& room_id,
                                const rtc::binary& packet) {
  if (rtc::IsRtcp(packet)) {
    return;
  }
  if (packet.size() < sizeof(rtc::RtpHeader) || is_padded(packet)) {
    return;
  }
  video_packets_received_.fetch_add(1, std::memory_order_relaxed);

  const std::shared_ptr<const RoutingTable> table = routes();
  const auto route = table->by_source.find(from_user_id);
  if (route == table->by_source.end() || route->second.room_id != room_id) {
    return;
  }

  for (const Outbound& destination : route->second.video) {
    if (!destination.track->isOpen()) {
      continue;
    }

    rtc::binary copy = packet;
    auto* header = reinterpret_cast<rtc::RtpHeader*>(copy.data());
    header->setSsrc(destination.ssrc);
    header->setPayloadType(static_cast<std::uint8_t>(destination.payload_type));

    destination.track->send(std::move(copy));
    video_packets_forwarded_.fetch_add(1, std::memory_order_relaxed);
  }
}

void MediaRouter::request_keyframe_from_sharer(const std::string& room_id) {
  // Read from the routing table rather than from the sessions, for the same
  // reason forwarding is: this arrives on a libdatachannel thread that is
  // already holding a lock of its own.
  const std::shared_ptr<const RoutingTable> table = routes();
  const auto tracks = table->video_inbound_by_room.find(room_id);
  if (tracks == table->video_inbound_by_room.end()) {
    return;
  }

  // Asked of everyone in the room rather than of the sharer alone. The router
  // does not track who is sharing, and does not need to: a participant who is
  // not sending video has nothing to produce an intra frame from, so the
  // request costs them one ignored RTCP packet.
  for (const auto& track : tracks->second) {
    if (track->isOpen() && track->requestKeyframe()) {
      keyframe_requests_forwarded_.fetch_add(1, std::memory_order_relaxed);
    }
  }
}

void MediaRouter::publish_routes() {
  auto table = std::make_shared<RoutingTable>();

  for (const auto& [user_id, session] : sessions_) {
    Route route;
    route.room_id = session.room_id;

    for (const auto& [other_id, other] : sessions_) {
      if (other_id == user_id || other.room_id != session.room_id) {
        continue;
      }
      if (const auto outbound = other.outbound.find(user_id); outbound != other.outbound.end()) {
        route.audio.push_back(outbound->second);
      }
      if (other.video_outbound.has_value()) {
        route.video.push_back(*other.video_outbound);
      }
    }

    table->by_source.emplace(user_id, std::move(route));

    if (session.video_inbound != nullptr) {
      table->video_inbound_by_room[session.room_id].push_back(session.video_inbound);
    }
    if (session.video_feedback != nullptr) {
      table->video_feedback_by_room[session.room_id].push_back(session.video_feedback);
    }
  }

  routes_.store(std::shared_ptr<const RoutingTable>(std::move(table)));
}

std::shared_ptr<const MediaRouter::RoutingTable> MediaRouter::routes() const {
  return routes_.load();
}

void MediaRouter::forward_audio(const std::string& from_user_id, const std::string& room_id,
                                const rtc::binary& packet) {
  if (rtc::IsRtcp(packet)) {
    // Handled by the track's own RTCP session, not something to forward.
    return;
  }
  if (packet.size() < sizeof(rtc::RtpHeader) || is_padded(packet)) {
    return;
  }
  audio_packets_received_.fetch_add(1, std::memory_order_relaxed);

  // No lock here, on purpose. See RoutingTable: this runs on a libdatachannel
  // thread that already holds a lock inside the peer connection, and taking
  // ours as well would deadlock against a join.
  const std::shared_ptr<const RoutingTable> table = routes();
  const auto route = table->by_source.find(from_user_id);
  if (route == table->by_source.end() || route->second.room_id != room_id) {
    return;
  }

  for (const Outbound& destination : route->second.audio) {
    if (!destination.track->isOpen()) {
      continue;
    }

    // One copy per destination: each carries a different SSRC, which is what
    // keeps the receiver from seeing several participants as one stream.
    rtc::binary copy = packet;
    auto* header = reinterpret_cast<rtc::RtpHeader*>(copy.data());
    header->setSsrc(destination.ssrc);
    header->setPayloadType(static_cast<std::uint8_t>(destination.payload_type));

    destination.track->send(std::move(copy));
    audio_packets_forwarded_.fetch_add(1, std::memory_order_relaxed);
  }
}

void MediaRouter::enqueue(const std::string& user_id, protocol::Message message) {
  post([this, user_id, message = std::move(message)]() mutable {
    SignalHandler handler;
    {
      const std::lock_guard<std::mutex> lock(worker_mutex_);
      handler = signal_handler_;
    }
    if (handler) {
      handler(user_id, std::move(message));
    }
  });
}

void MediaRouter::post(std::function<void()> task) {
  const std::lock_guard<std::mutex> lock(worker_mutex_);
  tasks_.push_back(std::move(task));
  worker_changed_.notify_one();
}

void MediaRouter::worker_loop() {
  while (true) {
    std::function<void()> task;
    {
      std::unique_lock<std::mutex> lock(worker_mutex_);
      worker_changed_.wait(lock, [this] { return stopping_ || !tasks_.empty(); });
      if (stopping_ && tasks_.empty()) {
        return;
      }
      task = std::move(tasks_.front());
      tasks_.pop_front();
    }

    // Run with no lock held: a task takes `mutex_`, and the signal handler
    // reaches into the transport.
    //
    // A throwing task must not take the process down. The transport is the
    // usual source: a socket can close between the SFU producing a frame and
    // this thread sending it.
    try {
      task();
    } catch (const std::exception& error) {
      DV_LOG_WARN("SFU: a queued task failed: {}", error.what());
    }
  }
}

}  // namespace dv::server::sfu
