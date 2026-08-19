// The libwebrtc side of media::MediaSession.
//
// This is the only file in the client that includes a libwebrtc header. Above
// it there is the interface in client/src/media/media_session.hpp, and below it
// nothing else in the project depends on the toolchain described in
// docs/webrtc-toolchain.md.
//
// Built only when DV_BUILD_CLIENT_MEDIA is on. The stub that takes its place
// otherwise lives in client/src/media/media_session.cpp.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <api/audio/audio_device.h>
#include <api/audio/audio_processing.h>
#include <api/audio/builtin_audio_processing_builder.h>
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
#include <api/transport/rtp/rtp_source.h>
#include <api/video/adapted_video_track_source.h>
#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <api/video/video_sink_interface.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <libyuv/convert.h>
#include <libyuv/convert_argb.h>
#include <rtc_base/logging.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/thread.h>

#include <dv/logging/logger.hpp>

#include "media/media_session.hpp"
#include "video/frame_queue.hpp"
#include "video/screen_capturer.hpp"

namespace dv::client::media {
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

  /// One microphone source for the whole process, created on first use.
  ///
  /// Shared because the capture device is shared: two sessions in one process
  /// would otherwise fight over it.
  [[nodiscard]] webrtc::scoped_refptr<webrtc::AudioSourceInterface> microphone(
      const webrtc::AudioOptions& options) {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (microphone_ == nullptr) {
      microphone_ = factory_->CreateAudioSource(options);
    }
    return microphone_;
  }

  /// Devices as the platform reports them.
  ///
  /// Every call into the audio device module happens on the worker thread,
  /// which is the thread libwebrtc drives it from. Touching it from anywhere
  /// else trips its own thread checker.
  [[nodiscard]] Result<std::vector<AudioDevice>> devices(bool input) {
    if (audio_device_ == nullptr) {
      return Result<std::vector<AudioDevice>>::failure("media_unavailable",
                                                       "there is no audio device module");
    }

    std::vector<AudioDevice> found;
    std::string error;

    worker_thread_->BlockingCall([&] {
      if (audio_device_->Init() != 0) {
        error = "the audio device module could not be initialised";
        return;
      }

      const int16_t count =
          input ? audio_device_->RecordingDevices() : audio_device_->PlayoutDevices();
      if (count < 0) {
        error = "the platform could not enumerate the devices";
        return;
      }

      // Oversized on purpose: some platform backends have been seen writing
      // past the documented limit.
      char name[webrtc::kAdmMaxDeviceNameSize * 4] = {};
      char guid[webrtc::kAdmMaxGuidSize * 4] = {};

      for (int16_t index = 0; index < count; ++index) {
        const int32_t result = input ? audio_device_->RecordingDeviceName(index, name, guid)
                                     : audio_device_->PlayoutDeviceName(index, name, guid);
        if (result != 0) {
          continue;
        }
        // Index 0 is the platform default on every backend libwebrtc has.
        found.push_back(AudioDevice{name, name, index == 0});
      }
    });

    if (!error.empty()) {
      return Result<std::vector<AudioDevice>>::failure("media_unavailable", error);
    }
    return found;
  }

  /// Switches device without ending the call: capture is stopped, the device
  /// is changed, and capture is started again, all on the worker thread.
  [[nodiscard]] Result<std::monostate> select_device(bool input, const std::string& device_id) {
    auto listed = devices(input);
    if (!listed) {
      return Result<std::monostate>::failure(listed.error());
    }

    const std::vector<AudioDevice> available = std::move(listed).take();
    std::optional<int16_t> index;
    for (std::size_t i = 0; i < available.size(); ++i) {
      if (available[i].id == device_id) {
        index = static_cast<int16_t>(i);
        break;
      }
    }
    if (!index.has_value()) {
      return Result<std::monostate>::failure("device_not_found",
                                             "no audio device named " + device_id);
    }

    std::string error;
    worker_thread_->BlockingCall([&] {
      if (input) {
        const bool was_recording = audio_device_->Recording();
        if (was_recording) {
          audio_device_->StopRecording();
        }
        if (audio_device_->SetRecordingDevice(static_cast<uint16_t>(*index)) != 0) {
          error = "the platform refused the capture device";
          return;
        }
        if (was_recording) {
          if (audio_device_->InitRecording() != 0 || audio_device_->StartRecording() != 0) {
            error = "the capture device was selected but could not be started";
          }
        }
      } else {
        const bool was_playing = audio_device_->Playing();
        if (was_playing) {
          audio_device_->StopPlayout();
        }
        if (audio_device_->SetPlayoutDevice(static_cast<uint16_t>(*index)) != 0) {
          error = "the platform refused the playback device";
          return;
        }
        if (was_playing) {
          if (audio_device_->InitPlayout() != 0 || audio_device_->StartPlayout() != 0) {
            error = "the playback device was selected but could not be started";
          }
        }
      }
    });

    if (!error.empty()) {
      return Result<std::monostate>::failure("device_error", error);
    }
    return std::monostate{};
  }

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

    // Section 9 of SPEC.md asks for echo cancellation, noise suppression and
    // automatic gain control.
    //
    // Handing the factory no processing module would still give us one, built
    // from its defaults, and the AudioOptions each session sets would still
    // switch AEC3, noise suppression and AGC1 on. What the defaults do not
    // cover is everything AudioOptions has no field for, which is where the
    // tuning below lives: the high pass filter, the width of the pipeline and
    // the rate it runs at.
    //
    // Whether each block is on stays with AudioOptions, which the media engine
    // folds into this config for every session, so a user turning noise
    // suppression off still turns it off. Note that the module belongs to the
    // factory rather than to a peer connection: sessions in the same process
    // share it, and the last options applied win. A client process has one
    // local user, so that is a distinction without a difference here.
    webrtc::AudioProcessing::Config processing;
    // AEC3, which is what the built-in echo canceller is when no custom
    // EchoControlFactory is injected.
    processing.echo_canceller.enabled = true;
    processing.noise_suppression.enabled = true;
    processing.noise_suppression.level =
        webrtc::AudioProcessing::Config::NoiseSuppression::Level::kHigh;
    // Rumble, desk bumps and DC offset, none of which Opus should be paying
    // bits for.
    processing.high_pass_filter.enabled = true;
    // Adaptive analog is the desktop mode: it drives the operating system's
    // own input volume and only compresses in software what is left.
    processing.gain_controller1.enabled = true;
    processing.gain_controller1.mode =
        webrtc::AudioProcessing::Config::GainController1::kAdaptiveAnalog;
    processing.gain_controller1.analog_gain_controller.enabled = true;
    // The call is mono at 48 kHz, so multi-channel processing would only cost
    // CPU for channels that are not there.
    processing.pipeline.multi_channel_capture = false;
    processing.pipeline.multi_channel_render = false;
    processing.pipeline.maximum_internal_processing_rate = 48000;

    webrtc::scoped_refptr<webrtc::AudioProcessing> audio_processing =
        webrtc::BuiltinAudioProcessingBuilder(processing).Build(environment);
    if (audio_processing == nullptr) {
      failure_ = "could not create the audio processing module";
      return;
    }

    factory_ = webrtc::CreatePeerConnectionFactory(
        network_thread_.get(), worker_thread_.get(), signaling_thread_.get(), audio_device_,
        webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(),
        webrtc::CreateBuiltinVideoEncoderFactory(), webrtc::CreateBuiltinVideoDecoderFactory(),
        /*audio_mixer=*/nullptr, std::move(audio_processing));

    if (factory_ == nullptr) {
      failure_ = "could not create the peer connection factory";
    }
  }

  std::unique_ptr<webrtc::Thread> network_thread_;
  std::unique_ptr<webrtc::Thread> worker_thread_;
  std::unique_ptr<webrtc::Thread> signaling_thread_;
  std::mutex mutex_;
  webrtc::scoped_refptr<webrtc::AudioSourceInterface> microphone_;
  webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  std::string failure_;
};

/// WebRTC accepts playback volume up to 10, where 1 is what the sender sent.
constexpr double kMaxVolume = 10.0;

/// Levels are sampled often enough for the indicator to look alive without
/// costing anything: five times a second.
constexpr auto kLevelInterval = std::chrono::milliseconds(200);

/// Roughly -34 dBov. Below this is room noise, not speech.
constexpr double kSpeakingThreshold = 0.02;

/// How long someone keeps counting as speaking after their last loud packet.
constexpr auto kSpeakingHold = std::chrono::milliseconds(600);

/// The video track source the screen capture feeds.
///
/// AdaptedVideoTrackSource rather than a plain source, because the adapter is
/// what lets the encoder ask for a smaller frame or a slower rate when the
/// connection cannot carry what is being produced. Nothing drives it yet, and
/// that hook is the M8 work; declaring the source this way is what makes it
/// possible without touching the pipeline.
class ScreenTrackSource : public webrtc::AdaptedVideoTrackSource {
 public:
  /// True is not cosmetic here. It tells the encoder this is screen content,
  /// so it keeps text sharp and lets the frame rate drop instead of blurring
  /// everything, which is the opposite of what it does for a camera.
  [[nodiscard]] bool is_screencast() const override { return true; }

  [[nodiscard]] std::optional<bool> needs_denoising() const override { return false; }

  [[nodiscard]] webrtc::MediaSourceInterface::SourceState state() const override {
    return webrtc::MediaSourceInterface::kLive;
  }

  [[nodiscard]] bool remote() const override { return false; }

  /// Converts one captured BGRA frame and hands it to the encoder.
  ///
  /// Called from the pump thread, never from the capture thread: the colour
  /// conversion is the expensive part of this pipeline and doing it where the
  /// frames are produced would cost capture rate.
  void deliver(const video::VideoFrame& frame, std::int64_t timestamp_us) {
    int adapted_width = 0;
    int adapted_height = 0;
    int crop_width = 0;
    int crop_height = 0;
    int crop_x = 0;
    int crop_y = 0;
    if (!AdaptFrame(frame.width(), frame.height(), timestamp_us, &adapted_width, &adapted_height,
                    &crop_width, &crop_height, &crop_x, &crop_y)) {
      // Nobody is watching, or the adapter decided to drop this one.
      return;
    }

    webrtc::scoped_refptr<webrtc::I420Buffer> buffer =
        webrtc::I420Buffer::Create(frame.width(), frame.height());
    if (libyuv::ARGBToI420(frame.data(), frame.stride(), buffer->MutableDataY(), buffer->StrideY(),
                           buffer->MutableDataU(), buffer->StrideU(), buffer->MutableDataV(),
                           buffer->StrideV(), frame.width(), frame.height()) != 0) {
      return;
    }

    OnFrame(webrtc::VideoFrame::Builder()
                .set_video_frame_buffer(buffer)
                .set_timestamp_us(timestamp_us)
                .set_rotation(webrtc::kVideoRotation_0)
                .build());
  }
};

/// Receives the decoded screen somebody else is sharing.
class RemoteVideoSink final : public webrtc::VideoSinkInterface<webrtc::VideoFrame> {
 public:
  using Handler = std::function<void(video::VideoFrame)>;

  explicit RemoteVideoSink(Handler handler) : handler_(std::move(handler)) {}

  void OnFrame(const webrtc::VideoFrame& frame) override {
    if (!handler_) {
      return;
    }
    webrtc::scoped_refptr<webrtc::I420BufferInterface> buffer =
        frame.video_frame_buffer()->ToI420();
    if (buffer == nullptr) {
      return;
    }

    const int width = buffer->width();
    const int height = buffer->height();
    const auto bytes = static_cast<std::size_t>(width) * static_cast<std::size_t>(height) *
                       static_cast<std::size_t>(video::VideoFrame::kBytesPerPixel);
    std::vector<std::uint8_t> pixels(bytes);

    if (libyuv::I420ToARGB(buffer->DataY(), buffer->StrideY(), buffer->DataU(), buffer->StrideU(),
                           buffer->DataV(), buffer->StrideV(), pixels.data(),
                           width * video::VideoFrame::kBytesPerPixel, width, height) != 0) {
      return;
    }

    handler_(video::VideoFrame{width, height, std::move(pixels)});
  }

 private:
  Handler handler_;
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

class LibwebrtcMediaSession;

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

class LibwebrtcMediaSession final : public MediaSession, public webrtc::PeerConnectionObserver {
 public:
  explicit LibwebrtcMediaSession(Callbacks callbacks) : callbacks_(std::move(callbacks)) {}

  ~LibwebrtcMediaSession() override {
    close();
    if (stats_thread_.joinable()) {
      stats_thread_.join();
    }
    if (levels_thread_.joinable()) {
      levels_thread_.join();
    }
  }

  [[nodiscard]] Result<std::monostate> start(const MediaSessionOptions& options) {
    options_ = options;
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

    auto source = engine.microphone(audio_options);
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

    // The screen track goes in now too, and stays empty until somebody starts
    // sharing. The server offers a video m-line to every participant from the
    // moment they join for the same reason: with the transceiver already
    // paired, starting and stopping a share needs no renegotiation, and a
    // share that comes and goes cannot disturb the call.
    video_source_ = webrtc::make_ref_counted<ScreenTrackSource>();
    local_video_track_ = engine.factory()->CreateVideoTrack(video_source_, "dv-screen");
    if (local_video_track_ == nullptr) {
      return Result<std::monostate>::failure("media_unavailable",
                                             "could not create the screen track");
    }

    auto video_added = connection_->AddTrack(local_video_track_, {"dv-screen"});
    if (!video_added.ok()) {
      return Result<std::monostate>::failure("media_unavailable",
                                             std::string(video_added.error().message()));
    }
    video_sender_ = video_added.value();
    apply_video_bitrate();

    running_ = true;
    stats_thread_ = std::thread([this] { stats_loop(); });
    levels_thread_ = std::thread([this] { levels_loop(); });
    return std::monostate{};
  }

  // --- MediaSession ----------------------------------------------------------

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

  Result<std::monostate> set_participant_volume(const std::string& user_id,
                                                double volume) override {
    if (volume < 0.0 || volume > kMaxVolume) {
      return Result<std::monostate>::failure(
          "invalid_value", "volume has to be between 0 and " + std::to_string(kMaxVolume));
    }

    const std::lock_guard<std::mutex> lock(remote_mutex_);
    volumes_[user_id] = volume;

    const auto it = remote_.find(user_id);
    if (it == remote_.end()) {
      // Remembered for when the track shows up. Setting the volume of someone
      // who has not spoken yet is a normal thing for an interface to do.
      return Result<std::monostate>::failure("unknown_participant",
                                             "no audio track carries " + user_id + " yet");
    }

    it->second.volume = volume;
    apply_volume(it->second);
    return std::monostate{};
  }

  Result<std::monostate> set_input_device(const std::string& device_id) override {
    return Engine::instance().select_device(/*input=*/true, device_id);
  }

  Result<std::monostate> set_output_device(const std::string& device_id) override {
    return Engine::instance().select_device(/*input=*/false, device_id);
  }

  Result<std::monostate> start_screen_share(const std::string& monitor_id) override {
    stop_screen_share();

    auto created = video::create_screen_capturer(
        options_.capture,
        [this](video::VideoFrame frame) {
          // Straight into the queue, and back to capturing. Everything
          // expensive happens on the pump thread.
          if (!frame_queue_.push(std::move(frame))) {
            const std::lock_guard<std::mutex> lock(video_mutex_);
            ++video_stats_.frames_dropped;
          }
          {
            const std::lock_guard<std::mutex> lock(video_mutex_);
            ++video_stats_.frames_captured;
          }
        },
        [this](Error reason) {
          // Capture ended on its own. The track stays where it is; only the
          // pixels stop.
          sharing_.store(false);
          if (callbacks_.on_screen_share_ended) {
            callbacks_.on_screen_share_ended(std::move(reason));
          }
        });
    if (!created) {
      return Result<std::monostate>::failure(created.error());
    }

    std::unique_ptr<video::ScreenCapturer> capturer = std::move(created).take();
    if (auto started = capturer->start(monitor_id); !started) {
      return Result<std::monostate>::failure(started.error());
    }

    frame_queue_.clear();
    {
      const std::lock_guard<std::mutex> lock(video_mutex_);
      capturer_ = std::move(capturer);
    }

    sharing_.store(true);
    pumping_.store(true);
    pump_thread_ = std::thread([this] { pump_loop(); });
    return std::monostate{};
  }

  void stop_screen_share() override {
    sharing_.store(false);
    pumping_.store(false);
    if (pump_thread_.joinable() && pump_thread_.get_id() != std::this_thread::get_id()) {
      pump_thread_.join();
    }

    std::unique_ptr<video::ScreenCapturer> capturer;
    {
      const std::lock_guard<std::mutex> lock(video_mutex_);
      capturer = std::move(capturer_);
    }
    if (capturer) {
      capturer->stop();
    }
    frame_queue_.clear();
  }

  [[nodiscard]] bool sharing_screen() const override { return sharing_.load(); }

  Result<std::monostate> set_video_bitrate(int min_kbps, int max_kbps) override {
    if (min_kbps <= 0 || max_kbps < min_kbps) {
      return Result<std::monostate>::failure(
          "invalid_value", "the bitrate range has to be positive and the maximum at least the "
                           "minimum");
    }
    options_.video_min_bitrate_kbps = min_kbps;
    options_.video_max_bitrate_kbps = max_kbps;
    apply_video_bitrate();
    return std::monostate{};
  }

  [[nodiscard]] VideoStats video_stats() const override {
    const std::lock_guard<std::mutex> lock(video_mutex_);
    VideoStats copy = video_stats_;
    copy.frames_dropped = video_stats_.frames_dropped + frame_queue_.dropped();
    if (capturer_) {
      copy.send_fps = capturer_->stats().fps;
    }
    return copy;
  }

  [[nodiscard]] AudioStats stats() const override {
    const std::lock_guard<std::mutex> lock(stats_mutex_);
    return stats_;
  }

  [[nodiscard]] MediaState state() const override { return state_.load(); }

  void close() override {
    running_ = false;
    // Before the connection goes: the pump thread touches the track source,
    // and the capture thread touches the queue the pump reads from.
    stop_screen_share();
    if (connection_ != nullptr) {
      connection_->Close();
      connection_ = nullptr;
    }
    // Released here and not only in the destructor. The track holds the audio
    // source, the source holds the capture device open, and the device is
    // shared with every other session in the process: leaving it attached
    // makes the next session wait on it.
    local_track_ = nullptr;
    {
      const std::lock_guard<std::mutex> lock(video_mutex_);
      if (remote_video_track_ != nullptr && remote_video_sink_ != nullptr) {
        remote_video_track_->RemoveSink(remote_video_sink_.get());
      }
      remote_video_track_ = nullptr;
      remote_video_sink_.reset();
    }
    local_video_track_ = nullptr;
    video_sender_ = nullptr;
    video_source_ = nullptr;
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
  /// One remote participant's audio, as it arrives.
  struct Remote {
    webrtc::scoped_refptr<webrtc::RtpReceiverInterface> receiver;
    webrtc::scoped_refptr<webrtc::MediaStreamTrackInterface> track;
    double volume = 1.0;
  };

  void report_remote_track(const webrtc::scoped_refptr<webrtc::RtpReceiverInterface>& receiver,
                           bool active) {
    if (receiver == nullptr) {
      return;
    }

    // Video is the shared screen, and it is handled apart. It carries whoever
    // holds the floor rather than one participant, so unlike audio there is no
    // user id to read off it: who is sharing arrives over signaling.
    if (receiver->media_type() == webrtc::MediaType::VIDEO) {
      report_remote_video(receiver, active);
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
    const std::string user_id = stream_ids.front();

    {
      const std::lock_guard<std::mutex> lock(remote_mutex_);
      if (active) {
        remote_[user_id] = Remote{receiver, receiver->track(), volume_of(user_id)};
        // A volume set before the track existed is applied now.
        apply_volume(remote_[user_id]);
      } else {
        remote_.erase(user_id);
      }
    }

    if (callbacks_.on_remote_audio) {
      callbacks_.on_remote_audio(user_id, active);
    }
  }

  void report_remote_video(const webrtc::scoped_refptr<webrtc::RtpReceiverInterface>& receiver,
                           bool active) {
    auto track = webrtc::scoped_refptr<webrtc::VideoTrackInterface>(
        static_cast<webrtc::VideoTrackInterface*>(receiver->track().get()));

    const std::lock_guard<std::mutex> lock(video_mutex_);

    // Whatever was attached before comes off first: the sink is a raw pointer
    // as far as libwebrtc is concerned, and it must not outlive the track that
    // holds it.
    if (remote_video_track_ != nullptr && remote_video_sink_ != nullptr) {
      remote_video_track_->RemoveSink(remote_video_sink_.get());
    }

    if (!active || track == nullptr) {
      remote_video_track_ = nullptr;
      remote_video_sink_.reset();
      return;
    }

    remote_video_sink_ = std::make_unique<RemoteVideoSink>([this](video::VideoFrame frame) {
      {
        const std::lock_guard<std::mutex> counting(video_mutex_);
        ++video_stats_.frames_received;
      }
      if (callbacks_.on_remote_video) {
        callbacks_.on_remote_video(std::move(frame));
      }
    });
    remote_video_track_ = std::move(track);
    remote_video_track_->AddOrUpdateSink(remote_video_sink_.get(), webrtc::VideoSinkWants{});
  }

  /// Must be called with `remote_mutex_` held.
  [[nodiscard]] double volume_of(const std::string& user_id) const {
    const auto it = volumes_.find(user_id);
    return it == volumes_.end() ? 1.0 : it->second;
  }

  static void apply_volume(const Remote& remote) {
    if (remote.track == nullptr) {
      return;
    }
    auto* audio_track = static_cast<webrtc::AudioTrackInterface*>(remote.track.get());
    if (audio_track->GetSource() != nullptr) {
      audio_track->GetSource()->SetVolume(remote.volume);
    }
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

    // The audio source is where the processing module surfaces: echo return
    // loss only has a value while the echo controller is actually running, so
    // this is what tells the difference between AEC3 configured and AEC3 on.
    //
    // It is also the only place libwebrtc reports the level of the microphone.
    // AudioTrackInterface::GetSignalLevel looks like the obvious way to ask,
    // but a local track's source never implements it, so it answers false for
    // every call and an indicator built on it stays at zero forever.
    for (const auto* source : report->GetStatsOfType<webrtc::RTCAudioSourceStats>()) {
      if (source->echo_return_loss.has_value()) {
        collected.echo_cancellation_active = true;
        collected.echo_return_loss_db = *source->echo_return_loss;
      }
      if (source->audio_level.has_value()) {
        local_level_.store(*source->audio_level);
      }
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

  /// Converts what RFC 6464 puts in the RTP header into a level from 0 to 1.
  ///
  /// The header carries -dBov: 0 is as loud as the format allows and 127 is
  /// silence. What an interface wants is a bar, so it becomes linear.
  [[nodiscard]] static double level_from_dbov(std::uint8_t dbov) {
    constexpr std::uint8_t kSilence = 127;
    if (dbov >= kSilence) {
      return 0.0;
    }
    return std::pow(10.0, -static_cast<double>(dbov) / 20.0);
  }

  void collect_levels() {
    if (!callbacks_.on_levels) {
      return;
    }

    std::vector<AudioLevel> levels;
    const auto now = std::chrono::steady_clock::now();

    // The local microphone, as the last stats report saw it. An empty user id
    // is what marks the local level apart from the remote ones.
    if (local_track_ != nullptr && !muted_.load()) {
      const double level = local_level_.load();
      levels.push_back(AudioLevel{{}, level, level > kSpeakingThreshold});
    }

    const std::lock_guard<std::mutex> lock(remote_mutex_);
    for (const auto& [user_id, remote] : remote_) {
      if (remote.receiver == nullptr) {
        continue;
      }

      double level = 0;
      for (const webrtc::RtpSource& source : remote.receiver->GetSources()) {
        if (const std::optional<std::uint8_t> dbov = source.audio_level()) {
          level = std::max(level, level_from_dbov(*dbov));
        }
      }

      // Held for a moment after the last loud packet: without it the
      // indicator blinks between syllables.
      if (level > kSpeakingThreshold) {
        speaking_until_[user_id] = now + kSpeakingHold;
      }
      const auto until = speaking_until_.find(user_id);
      const bool speaking = until != speaking_until_.end() && now < until->second;

      levels.push_back(AudioLevel{user_id, level, speaking});
    }

    callbacks_.on_levels(std::move(levels));
  }

  /// Takes frames off the queue, converts them and hands them to the encoder.
  ///
  /// This thread exists so that the colour conversion, which is the expensive
  /// part, does not run where the frames are captured. A slow encoder costs
  /// queued frames rather than capture rate.
  void pump_loop() {
    const auto interval = std::chrono::milliseconds(1000 / std::max(1, options_.capture.max_fps));
    while (pumping_.load()) {
      std::optional<video::VideoFrame> frame = frame_queue_.pop();
      if (!frame.has_value()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        continue;
      }

      const auto timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now().time_since_epoch())
                                    .count();
      if (video_source_ != nullptr) {
        video_source_->deliver(*frame, timestamp_us);
      }

      const std::lock_guard<std::mutex> lock(video_mutex_);
      ++video_stats_.frames_sent;
      video_stats_.send_width = frame->width();
      video_stats_.send_height = frame->height();
      (void)interval;
    }
  }

  /// Section 6 of SPEC.md: 1.5 to 3 Mbps for the screen.
  void apply_video_bitrate() {
    if (video_sender_ == nullptr) {
      return;
    }
    webrtc::RtpParameters parameters = video_sender_->GetParameters();
    if (parameters.encodings.empty()) {
      return;
    }
    parameters.encodings[0].min_bitrate_bps = options_.video_min_bitrate_kbps * 1000;
    parameters.encodings[0].max_bitrate_bps = options_.video_max_bitrate_kbps * 1000;
    if (const webrtc::RTCError error = video_sender_->SetParameters(parameters); !error.ok()) {
      DV_LOG_WARN("Media: the encoder refused the bitrate range: {}", error.message());
    }
  }

  void levels_loop() {
    while (running_) {
      std::this_thread::sleep_for(kLevelInterval);
      if (running_ && connection_ != nullptr) {
        collect_levels();
      }
    }
  }

  void stats_loop() {
    // Collected more often than the metrics are reported, so that a bitrate is
    // averaged over a short window rather than over the whole call.
    //
    // This is also what feeds the microphone level indicator, so it runs at
    // the level interval rather than at a metrics interval: a level meter that
    // moves once a second reads as broken.
    constexpr auto kInterval = kLevelInterval;

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
  webrtc::scoped_refptr<ScreenTrackSource> video_source_;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> local_video_track_;
  webrtc::scoped_refptr<webrtc::RtpSenderInterface> video_sender_;

  MediaSessionOptions options_;

  /// Between the capture thread and the pump thread. See video::FrameQueue for
  /// why it drops the oldest frame rather than blocking.
  video::FrameQueue frame_queue_;
  std::thread pump_thread_;
  std::atomic<bool> pumping_{false};
  std::atomic<bool> sharing_{false};

  mutable std::mutex video_mutex_;
  std::unique_ptr<video::ScreenCapturer> capturer_;
  VideoStats video_stats_;
  std::unique_ptr<RemoteVideoSink> remote_video_sink_;
  webrtc::scoped_refptr<webrtc::VideoTrackInterface> remote_video_track_;

  mutable std::mutex remote_mutex_;
  std::unordered_map<std::string, Remote> remote_;
  /// Kept apart from `remote_` so that a volume chosen before someone's track
  /// arrives is not lost.
  std::unordered_map<std::string, double> volumes_;
  /// How long a participant keeps counting as speaking after their last loud
  /// packet, so the indicator does not flicker between syllables.
  std::unordered_map<std::string, std::chrono::steady_clock::time_point> speaking_until_;

  std::atomic<bool> muted_{false};
  /// The microphone level from the last stats report, on a scale of 0 to 1.
  std::atomic<double> local_level_{0.0};
  std::atomic<MediaState> state_{MediaState::New};

  mutable std::mutex stats_mutex_;
  AudioStats stats_;
  std::chrono::steady_clock::time_point last_collection_ = std::chrono::steady_clock::now();

  std::atomic<bool> running_{false};
  std::thread stats_thread_;
  std::thread levels_thread_;
};

}  // namespace

Result<std::vector<AudioDevice>> input_devices() {
  return Engine::instance().devices(/*input=*/true);
}

Result<std::vector<AudioDevice>> output_devices() {
  return Engine::instance().devices(/*input=*/false);
}

Result<std::unique_ptr<MediaSession>> create_media_session(const MediaSessionOptions& options,
                                                           MediaSession::Callbacks callbacks) {
  auto session = std::make_unique<LibwebrtcMediaSession>(std::move(callbacks));
  if (auto started = session->start(options); !started) {
    return Result<std::unique_ptr<MediaSession>>::failure(started.error());
  }
  return std::unique_ptr<MediaSession>(std::move(session));
}

bool media_is_available() noexcept {
  return Engine::instance().factory() != nullptr;
}

}  // namespace dv::client::media
