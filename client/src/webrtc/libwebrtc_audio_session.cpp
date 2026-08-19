// The libwebrtc side of audio::AudioSession.
//
// This is the only file in the client that includes a libwebrtc header. Above
// it there is the interface in client/src/audio/audio_session.hpp, and below it
// nothing else in the project depends on the toolchain described in
// docs/webrtc-toolchain.md.
//
// Built only when DV_BUILD_CLIENT_MEDIA is on. The stub that takes its place
// otherwise lives in client/src/audio/audio_session.cpp.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <api/audio/audio_device.h>
#include <api/audio/create_audio_device_module.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/audio_options.h>
#include <api/create_peerconnection_factory.h>
#include <api/environment/environment.h>
#include <api/environment/environment_factory.h>
#include <api/jsep.h>
#include <api/media_stream_interface.h>
#include <api/peer_connection_interface.h>
#include <api/rtp_receiver_interface.h>
#include <api/rtp_transceiver_interface.h>
#include <api/scoped_refptr.h>
#include <api/set_local_description_observer_interface.h>
#include <api/set_remote_description_observer_interface.h>
#include <api/stats/rtc_stats_collector_callback.h>
#include <api/stats/rtc_stats_report.h>
#include <api/stats/rtcstats_objects.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <rtc_base/logging.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/thread.h>

#include <dv/logging/logger.hpp>

#include "audio/audio_session.hpp"

namespace dv::client::audio {
namespace {

/// The libwebrtc runtime: three threads, the factory, and SSL.
///
/// Created once and never torn down. libwebrtc's threads outlive most of what
/// uses them, and shutting the runtime down while a callback is in flight is a
/// well known way to crash on exit for no benefit: the process is ending
/// anyway.
class Engine {
 public:
  static Engine& instance() {
    static Engine engine;
    return engine;
  }

  [[nodiscard]] webrtc::PeerConnectionFactoryInterface* factory() const { return factory_.get(); }
  [[nodiscard]] const std::string& failure() const { return failure_; }

 private:
  Engine() {
    // libwebrtc keeps its own logging, silent by default. It is the only way
    // to see why a device failed to open or a codec was rejected, so it is one
    // environment variable away rather than a rebuild.
    if (const char* level = std::getenv("DV_WEBRTC_LOG"); level != nullptr) {
      const std::string value = level;
      webrtc::LogMessage::LogToDebug(value == "verbose" ? webrtc::LS_VERBOSE
                                     : value == "info"  ? webrtc::LS_INFO
                                                        : webrtc::LS_WARNING);
      webrtc::LogMessage::SetLogToStderr(true);
    }

    webrtc::InitializeSSL();

    network_thread_ = webrtc::Thread::CreateWithSocketServer();
    worker_thread_ = webrtc::Thread::Create();
    signaling_thread_ = webrtc::Thread::Create();
    network_thread_->SetName("dv-network", nullptr);
    worker_thread_->SetName("dv-worker", nullptr);
    signaling_thread_->SetName("dv-signaling", nullptr);

    if (!network_thread_->Start() || !worker_thread_->Start() || !signaling_thread_->Start()) {
      failure_ = "could not start the libwebrtc threads";
      return;
    }

    // The audio device module is created here, once, and handed to the factory.
    // Left to the factory it would be built and torn down with every peer
    // connection, and on Linux each cycle re-opens PulseAudio: a call that
    // starts after another one ended waits ten seconds on the audio thread,
    // twice, before it can negotiate.
    const webrtc::Environment environment = webrtc::CreateEnvironment();
    const auto layer = std::getenv("DV_AUDIO_NULL_DEVICE") != nullptr
                           ? webrtc::AudioDeviceModule::kDummyAudio
                           : webrtc::AudioDeviceModule::kPlatformDefaultAudio;
    worker_thread_->BlockingCall([&] {
      // Has to be built on the worker thread, which is where libwebrtc drives
      // it from afterwards.
      audio_device_ = webrtc::CreateAudioDeviceModule(environment, layer);
    });

    factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(), worker_thread_.get(), signaling_thread_.get(), audio_device_,
        webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(), webrtc::CreateBuiltinVideoDecoderFactory(),
        /*audio_mixer=*/nullptr,
        /*audio_processing=*/nullptr);

    if (factory_ == nullptr) {
      failure_ = "could not create the peer connection factory";
    }
  }

  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  std::string failure_;
};

[[nodiscard]] MediaState to_media_state(
    webrtc::PeerConnectionInterface::PeerConnectionState state) {
  switch (state) {
    case webrtc::PeerConnectionInterface::PeerConnectionState::kNew:
      return MediaState::New;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnecting:
      return MediaState::Connecting;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kConnected:
      return MediaState::Connected;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kDisconnected:
      return MediaState::Disconnected;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kFailed:
      return MediaState::Failed;
    case webrtc::PeerConnectionInterface::PeerConnectionState::kClosed:
      return MediaState::Closed;
  }
  return MediaState::New;
}

class LibwebrtcAudioSession;

/// Applies the answer libwebrtc just produced and hands the SDP up.
class LocalDescriptionObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  using Handler = std::function<void(webrtc::RTCError)>;
  explicit LocalDescriptionObserver(Handler handler) : handler_(std::move(handler)) {}
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override { handler_(error); }

 private:
  Handler handler_;
};

class RemoteDescriptionObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  using Handler = std::function<void(webrtc::RTCError)>;
  explicit RemoteDescriptionObserver(Handler handler) : handler_(std::move(handler)) {}
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override { handler_(error); }

 private:
  Handler handler_;
};

class StatsObserver : public webrtc::RTCStatsCollectorCallback {
 public:
  using Handler = std::function<void(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>&)>;
  explicit StatsObserver(Handler handler) : handler_(std::move(handler)) {}
  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    handler_(report);
  }

 private:
  Handler handler_;
};

class LibwebrtcAudioSession final : public AudioSession, public webrtc::PeerConnectionObserver {
 public:
  explicit LibwebrtcAudioSession(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

  ~LibwebrtcAudioSession() override {
    close();
    if (stats_thread_.joinable()) {
      stats_thread_.join();
    }
  }

  [[nodiscard]] Result<std::monostate> start(const AudioSessionOptions& options) {
    Engine& engine = Engine::instance();
    if (engine.factory() == nullptr) {
      return Result<std::monostate>::failure("media_unavailable", engine.failure());
    }

    webrtc::PeerConnectionInterface::RTCConfiguration configuration;
    configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    for (const std::string& url : options.ice_servers) {
      if (url.empty()) {
        continue;
      }
      webrtc::PeerConnectionInterface::IceServer server;
      server.urls.push_back(url);
      if (url.rfind("turn", 0) == 0) {
        server.username = options.turn_username;
        server.password = options.turn_password;
      }
      configuration.servers.push_back(std::move(server));
    }

    webrtc::PeerConnectionDependencies dependencies(this);
    auto created =
        engine.factory()->CreatePeerConnectionOrError(configuration, std::move(dependencies));
    if (!created.ok()) {
      return Result<std::monostate>::failure("media_unavailable",
                                             std::string(created.error().message()));
    }
    connection_ = created.MoveValue();

    // The microphone track is added before any negotiation. In unified plan
    // that gives us a transceiver ready to be paired with the recvonly audio
    // line the server offers, which is what makes this end the sender.
    webrtc::AudioOptions audio_options;
    audio_options.echo_cancellation = options.echo_cancellation;
    audio_options.noise_suppression = options.noise_suppression;
    audio_options.auto_gain_control = options.automatic_gain_control;

    auto source = engine.factory()->CreateAudioSource(audio_options);
    if (source == nullptr) {
      return Result<std::monostate>::failure("media_unavailable", "could not open an audio source");
    }

    local_track_ = engine.factory()->CreateAudioTrack("dv-microphone", source.get());
    if (local_track_ == nullptr) {
      return Result<std::monostate>::failure("media_unavailable",
                                             "could not create the microphone track");
    }

    if (auto added = connection_->AddTrack(local_track_, {"dv-local"}); !added.ok()) {
      return Result<std::monostate>::failure("media_unavailable",
                                             std::string(added.error().message()));
    }

    running_ = true;
    stats_thread_ = std::thread([this] { stats_loop(); });
    return std::monostate{};
  }

  // --- AudioSession ----------------------------------------------------------

  Result<std::monostate> apply_remote_offer(const std::string& sdp) override {
    webrtc::SdpParseError parse_error;
    std::unique_ptr<webrtc::SessionDescriptionInterface> offer =
        webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, sdp, &parse_error);
    if (offer == nullptr) {
      return Result<std::monostate>::failure("invalid_sdp", parse_error.description);
    }

    auto observer = webrtc::make_ref_counted<RemoteDescriptionObserver>(
        [this](webrtc::RTCError error) { on_remote_description_set(error); });
    connection_->SetRemoteDescription(std::move(offer), observer);
    return std::monostate{};
  }

  Result<std::monostate> add_remote_candidate(const IceCandidate& candidate) override {
    webrtc::SdpParseError parse_error;
    std::unique_ptr<webrtc::IceCandidate> parsed(webrtc::CreateIceCandidate(
        candidate.sdp_mid, candidate.sdp_mline_index, candidate.candidate, &parse_error));
    if (parsed == nullptr) {
      return Result<std::monostate>::failure("invalid_candidate", parse_error.description);
    }

    connection_->AddIceCandidate(std::move(parsed), [](webrtc::RTCError error) {
      if (!error.ok()) {
        DV_LOG_WARN("Media: the remote candidate was rejected: {}", error.message());
      }
    });
    return std::monostate{};
  }

  void set_microphone_muted(bool muted) override {
    muted_ = muted;
    if (local_track_ != nullptr) {
      // Disabling the track keeps the transceiver in place and sends silence,
      // so muting costs nothing and needs no renegotiation.
      local_track_->set_enabled(!muted);
    }
  }

  [[nodiscard]] bool microphone_muted() const override { return muted_.load(); }

  [[nodiscard]] AudioStats stats() const override {
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
  }

  [[nodiscard]] MediaState state() const override { return state_.load(); }

  void close() override {
    running_ = false;
    if (connection_ != nullptr) {
      connection_->Close();
      connection_ = nullptr;
    }
    // Released here and not only in the destructor. The track holds the audio
    // source, the source holds the capture device open, and the device is
    // shared with every other session in the process: leaving it attached
    // makes the next session wait on it.
    local_track_ = nullptr;
    state_ = MediaState::Closed;
  }

  // --- PeerConnectionObserver ------------------------------------------------

  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}

  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}

  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}

  void OnIceCandidate(const webrtc::IceCandidate* candidate) override {
    if (candidate == nullptr || !callbacks_.on_local_candidate) {
      return;
    }
    std::string sdp;
    if (!candidate->ToString(&sdp)) {
      return;
    }
    callbacks_.on_local_candidate(
        IceCandidate{sdp, candidate->sdp_mid(), candidate->sdp_mline_index()});
  }

  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override {
    const MediaState mapped = to_media_state(state);
    state_ = mapped;
    DV_LOG_INFO("Media: connection is {}", to_string(mapped));
    if (callbacks_.on_state) {
      callbacks_.on_state(mapped);
    }
  }

  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override {
    report_remote_track(transceiver->receiver(), true);
  }

  void OnRemoveTrack(webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver) override {
    report_remote_track(receiver, false);
  }

 private:
  void report_remote_track(const webrtc::scoped_refptr<webrtc::RtpReceiverInterface>& receiver,
                           bool active) {
    if (receiver == nullptr || !callbacks_.on_remote_audio) {
      return;
    }

    // The server puts the participant's id in the msid of every track it
    // sends, which is the only thing that says whose voice this is. See
    // section 4.3 of docs/protocol.md.
    const std::vector<std::string> stream_ids = receiver->stream_ids();
    if (stream_ids.empty()) {
      DV_LOG_WARN("Media: a remote track arrived with no msid, cannot tell whose voice it is");
      return;
    }
    callbacks_.on_remote_audio(stream_ids.front(), active);
  }

  void on_remote_description_set(webrtc::RTCError error) {
    if (!error.ok()) {
      DV_LOG_ERROR("Media: the offer was rejected: {}", error.message());
      return;
    }

    // With a remote offer applied, this produces the answer and sets it in one
    // step. There is no CreateAnswer call because there is nothing to inspect
    // in between.
    auto observer = webrtc::make_ref_counted<LocalDescriptionObserver>(
        [this](webrtc::RTCError local_error) { on_local_description_set(local_error); });
    connection_->SetLocalDescription(observer);
  }

  void on_local_description_set(webrtc::RTCError error) {
    if (!error.ok()) {
      DV_LOG_ERROR("Media: could not apply the local answer: {}", error.message());
      return;
    }
    if (connection_ == nullptr || connection_->local_description() == nullptr) {
      return;
    }

    std::string sdp;
    if (!connection_->local_description()->ToString(&sdp)) {
      DV_LOG_ERROR("Media: the local answer could not be serialized");
      return;
    }
    if (callbacks_.on_local_answer) {
      callbacks_.on_local_answer(std::move(sdp));
    }
  }

  void collect(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
    AudioStats collected;

    // Summed across streams: with several participants there is one inbound
    // stream per voice, and what the log wants is the call as a whole.
    for (const auto* inbound : report->GetStatsOfType<webrtc::RTCInboundRtpStreamStats>()) {
      if (inbound->kind.value_or("") != "audio") {
        continue;
      }
      collected.packets_received += inbound->packets_received.value_or(0);
      collected.bytes_received += inbound->bytes_received.value_or(0);
      collected.jitter_ms = std::max(collected.jitter_ms, inbound->jitter.value_or(0) * 1000.0);
      collected.packets_lost +=
          static_cast<std::uint64_t>(std::max(0, inbound->packets_lost.value_or(0)));
    }

    for (const auto* outbound : report->GetStatsOfType<webrtc::RTCOutboundRtpStreamStats>()) {
      if (outbound->kind.value_or("") != "audio") {
        continue;
      }
      collected.packets_sent += outbound->packets_sent.value_or(0);
      collected.bytes_sent += outbound->bytes_sent.value_or(0);
    }

    // Round trip time is only known from the other end's receiver reports.
    for (const auto* remote : report->GetStatsOfType<webrtc::RTCRemoteInboundRtpStreamStats>()) {
      collected.round_trip_time_ms =
          std::max(collected.round_trip_time_ms, remote->round_trip_time.value_or(0) * 1000.0);
    }

    const auto now = std::chrono::steady_clock::now();

    const std::lock_guard<std::mutex> lock(stats_mutex_);
    const double elapsed_seconds = std::chrono::duration<double>(now - last_collection_).count();
    if (elapsed_seconds > 0.1) {
      // Bitrate is a rate, and the report only carries totals.
      collected.send_bitrate_kbps = static_cast<double>(collected.bytes_sent - stats_.bytes_sent) *
                                    8.0 / elapsed_seconds / 1000.0;
      collected.receive_bitrate_kbps =
          static_cast<double>(collected.bytes_received - stats_.bytes_received) * 8.0 /
          elapsed_seconds / 1000.0;
      last_collection_ = now;
    } else {
      collected.send_bitrate_kbps = stats_.send_bitrate_kbps;
      collected.receive_bitrate_kbps = stats_.receive_bitrate_kbps;
    }

    stats_ = collected;
  }

  void stats_loop() {
    // Collected more often than the metrics are reported, so that a bitrate is
    // averaged over a short window rather than over the whole call.
    constexpr auto kInterval = std::chrono::milliseconds(1000);

    while (running_) {
      std::this_thread::sleep_for(kInterval);
      if (!running_ || connection_ == nullptr) {
        continue;
      }

      auto observer = webrtc::make_ref_counted<StatsObserver>(
          [this](const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) {
            collect(report);
          });
      connection_->GetStats(observer.get());
    }
  }

  Callbacks callbacks_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> connection_;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> local_track_;

  std::atomic<bool> muted_{false};
  std::atomic<MediaState> state_{MediaState::New};

  mutable std::mutex stats_mutex_;
  AudioStats stats_;
  std::chrono::steady_clock::time_point last_collection_ = std::chrono::steady_clock::now();

  std::atomic<bool> running_{false};
  std::thread stats_thread_;
};

}  // namespace

Result<std::unique_ptr<AudioSession>> create_audio_session(const AudioSessionOptions& options,
                                                           AudioSession::Callbacks callbacks) {
  auto session = std::make_unique<LibwebrtcAudioSession>(std::move(callbacks));
  if (auto started = session->start(options); !started) {
    return Result<std::unique_ptr<AudioSession>>::failure(started.error());
  }
  return std::unique_ptr<AudioSession>(std::move(session));
}

bool media_is_available() noexcept {
  return Engine::instance().factory() != nullptr;
}

}  // namespace dv::client::audio
