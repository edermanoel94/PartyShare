#include "sfu/audio_router.hpp"

#include <cstring>
#include <utility>
#include <variant>

#include <dv/logging/logger.hpp>

namespace dv::server::sfu {
namespace {

/// Opus is always 48 kHz on the wire, section 9 of SPEC.md.
constexpr int kOpusClockRate = 48000;

[[nodiscard]] rtc::Configuration make_configuration(const std::vector<std::string>& ice_servers) {
  rtc::Configuration configuration;
  for (const std::string& server : ice_servers) {
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
  return configuration;
}

}  // namespace

AudioRouter::AudioRouter(Options options) : options_(std::move(options)) {
  worker_thread_ = std::thread([this] { worker_loop(); });
}

AudioRouter::~AudioRouter() {
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

void AudioRouter::on_signal(SignalHandler handler) {
  const std::lock_guard<std::mutex> lock(worker_mutex_);
  signal_handler_ = std::move(handler);
}

void AudioRouter::on_participant_joined(const std::string& room_id, const models::User& user) {
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
  session.connection =
      std::make_shared<rtc::PeerConnection>(make_configuration(options_.ice_servers));

  const std::string user_id = user.id;

  // Installed before anything can produce one. libdatachannel delivers these
  // from its own threads, and they must never take `mutex_` on a path that
  // could already hold it, which is why they only enqueue.
  session.connection->onLocalDescription([this, user_id, room_id](rtc::Description description) {
    if (description.type() != rtc::Description::Type::Offer) {
      // The SFU is always the offerer, so anything else is a bug on our side.
      DV_LOG_WARN("SFU: ignoring a local {} for {}", description.typeString(), user_id);
      return;
    }
    enqueue(user_id, protocol::Offer{room_id, std::string(protocol::kSfuUserId), user_id,
                                     std::string(description)});
  });

  session.connection->onLocalCandidate([this, user_id, room_id](rtc::Candidate candidate) {
    enqueue(user_id, protocol::IceCandidate{room_id, std::string(protocol::kSfuUserId), user_id,
                                            std::string(candidate), candidate.mid(), 0});
  });

  session.connection->onStateChange([user_id](rtc::PeerConnection::State state) {
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
  session.inbound = session.connection->addTrack(inbound);

  // Generates the receiver reports the sender needs to estimate loss and RTT.
  session.inbound->setMediaHandler(std::make_shared<rtc::RtcpReceivingSession>());

  session.inbound->onMessage(
      [this, user_id, room_id](rtc::binary packet) {
        forward_audio(user_id, room_id, std::move(packet));
      },
      [](const rtc::string&) {
        // Text on a media track is not part of anything we speak.
      });

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

  negotiate(created);
  DV_LOG_INFO("SFU: session for {} in room {} ({} sessions)", user.id, room_id, sessions_.size());
}

void AudioRouter::on_participant_left(const std::string& room_id, const std::string& user_id) {
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
  }

  // Closed outside the lock: it waits for the callbacks, which take the lock.
  closing->close();
  DV_LOG_INFO("SFU: session of {} in room {} closed", user_id, room_id);
}

void AudioRouter::on_media_signal(const std::string& room_id, const std::string& from_user_id,
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

std::size_t AudioRouter::session_count() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return sessions_.size();
}

std::size_t AudioRouter::outbound_track_count(const std::string& user_id) const {
  const std::lock_guard<std::mutex> lock(mutex_);
  const Session* session = find_session(user_id);
  return session == nullptr ? 0 : session->outbound.size();
}

AudioRouter::Session* AudioRouter::find_session(const std::string& user_id) {
  const auto it = sessions_.find(user_id);
  return it == sessions_.end() ? nullptr : &it->second;
}

const AudioRouter::Session* AudioRouter::find_session(const std::string& user_id) const {
  const auto it = sessions_.find(user_id);
  return it == sessions_.end() ? nullptr : &it->second;
}

void AudioRouter::add_outbound_track(Session& session, const std::string& source_user_id) {
  if (session.outbound.contains(source_user_id)) {
    return;
  }

  const std::uint32_t ssrc = next_ssrc_++;

  rtc::Description::Audio media(std::to_string(session.next_mid++),
                                rtc::Description::Direction::SendOnly);
  media.addOpusCodec(options_.opus_payload_type);

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

void AudioRouter::negotiate(Session& session) {
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

void AudioRouter::forward_audio(const std::string& from_user_id, const std::string& room_id,
                                rtc::binary packet) {
  if (rtc::IsRtcp(packet)) {
    // Handled by the track's own RTCP session, not something to forward.
    return;
  }
  if (packet.size() < sizeof(rtc::RtpHeader)) {
    return;
  }
  packets_received_.fetch_add(1, std::memory_order_relaxed);

  const std::lock_guard<std::mutex> lock(mutex_);

  for (auto& [user_id, session] : sessions_) {
    if (user_id == from_user_id || session.room_id != room_id) {
      continue;
    }
    const auto it = session.outbound.find(from_user_id);
    if (it == session.outbound.end() || !it->second.track->isOpen()) {
      continue;
    }

    // One copy per destination: each carries a different SSRC, which is what
    // keeps the receiver from seeing several participants as one stream.
    rtc::binary copy = packet;
    auto* header = reinterpret_cast<rtc::RtpHeader*>(copy.data());
    header->setSsrc(it->second.ssrc);
    header->setPayloadType(static_cast<std::uint8_t>(it->second.payload_type));

    it->second.track->send(std::move(copy));
    packets_forwarded_.fetch_add(1, std::memory_order_relaxed);
  }
}

void AudioRouter::enqueue(const std::string& user_id, protocol::Message message) {
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

void AudioRouter::post(std::function<void()> task) {
  const std::lock_guard<std::mutex> lock(worker_mutex_);
  tasks_.push_back(std::move(task));
  worker_changed_.notify_one();
}

void AudioRouter::worker_loop() {
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
