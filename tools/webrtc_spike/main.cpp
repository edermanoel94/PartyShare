// M3 toolchain spike.
//
// Proves the four things the rest of the plan depends on:
//   1. a prebuilt libwebrtc links on this platform
//   2. a PeerConnection can be created and can produce an SDP offer
//   3. modules/desktop_capture enumerates monitors and delivers a frame
//      (section 7 of SPEC.md)
//   4. the AudioDeviceModule enumerates input and output devices (section 8)
//   5. when built against the system standard library, that a std::string can
//      cross the libwebrtc boundary and reach our own code intact
//
// It is intentionally standalone and throwaway. Nothing in client/ or server/
// depends on it.

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <variant>

#include <api/audio/audio_device.h>
#include <api/audio/create_audio_device_module.h>
#include <api/audio_codecs/builtin_audio_decoder_factory.h>
#include <api/audio_codecs/builtin_audio_encoder_factory.h>
#include <api/create_peerconnection_factory.h>
#include <api/environment/environment.h>
#include <api/environment/environment_factory.h>
#include <api/make_ref_counted.h>
#include <api/peer_connection_interface.h>
#include <api/video_codecs/builtin_video_decoder_factory.h>
#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <modules/desktop_capture/desktop_capture_options.h>
#include <modules/desktop_capture/desktop_capturer.h>
#include <modules/desktop_capture/desktop_frame.h>
#include <rtc_base/ssl_adapter.h>
#include <rtc_base/thread.h>

#ifdef DV_SPIKE_WITH_SHARED
#include <dv/protocol/message.hpp>
#endif

namespace {

int failures = 0;

void report(const char* step, bool ok, const std::string& detail = {}) {
  std::printf("[%s] %-28s %s\n", ok ? " OK " : "FAIL", step, detail.c_str());
  if (!ok) {
    ++failures;
  }
}

/// Collects the offer produced by CreateOffer. libwebrtc calls back on the
/// signaling thread, so the spike blocks until one of the two methods fires.
class OfferObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  void OnSuccess(webrtc::SessionDescriptionInterface* description) override {
    description->ToString(&sdp_);
    done_ = true;
  }
  void OnFailure(webrtc::RTCError error) override {
    error_ = error.message();
    done_ = true;
  }

  [[nodiscard]] bool done() const { return done_; }
  [[nodiscard]] const std::string& sdp() const { return sdp_; }
  [[nodiscard]] const std::string& error() const { return error_; }

 private:
  bool done_ = false;
  std::string sdp_;
  std::string error_;
};

/// Empty observer: this spike never connects to a peer, it only negotiates
/// locally.
class NullPeerConnectionObserver : public webrtc::PeerConnectionObserver {
 public:
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState) override {}
  void OnIceCandidate(const webrtc::IceCandidateInterface*) override {}
};

/// True when a display server is reachable. Screen capture cannot work
/// without one, so the spike reports that case as skipped.
bool has_display_server() {
#if defined(WEBRTC_LINUX)
  const char* x11 = std::getenv("DISPLAY");
  const char* wayland = std::getenv("WAYLAND_DISPLAY");
  return (x11 != nullptr && *x11 != '\0') || (wayland != nullptr && *wayland != '\0');
#else
  return true;
#endif
}

// Receives the single frame check_screen_capture asks for. The capturer calls
// back on the thread that drives it, so no synchronization is needed here.
class FrameSink : public webrtc::DesktopCapturer::Callback {
 public:
  void OnCaptureResult(webrtc::DesktopCapturer::Result result,
                       std::unique_ptr<webrtc::DesktopFrame> frame) override {
    this->result = result;
    if (frame != nullptr) {
      width = frame->size().width();
      height = frame->size().height();
      bytes = static_cast<std::size_t>(frame->stride()) * static_cast<std::size_t>(height);
    }
    done = true;
  }

  webrtc::DesktopCapturer::Result result = webrtc::DesktopCapturer::Result::ERROR_PERMANENT;
  int width = 0;
  int height = 0;
  std::size_t bytes = 0;
  bool done = false;
};

void check_screen_capture() {
  webrtc::DesktopCaptureOptions options = webrtc::DesktopCaptureOptions::CreateDefault();
#if defined(WEBRTC_WIN)
  // Prefer Desktop Duplication over the GDI fallback (section 7 of SPEC.md).
  options.set_allow_directx_capturer(true);
#endif

  std::unique_ptr<webrtc::DesktopCapturer> capturer =
      webrtc::DesktopCapturer::CreateScreenCapturer(options);
  if (capturer == nullptr) {
    // On Linux this happens with no display server attached, which is the
    // normal state on a CI runner. It is a skip, not a toolchain failure.
    const bool headless = has_display_server();
    report("screen capturer", !headless,
           headless ? "CreateScreenCapturer returned null" : "skipped: no display server attached");
    return;
  }

  webrtc::DesktopCapturer::SourceList sources;
  if (!capturer->GetSourceList(&sources)) {
    report("screen capturer", false, "GetSourceList failed");
    return;
  }

  report("screen capturer", !sources.empty(), "monitors found: " + std::to_string(sources.size()));
  for (const auto& source : sources) {
    std::printf("        monitor id=%lld title=\"%s\"\n", static_cast<long long>(source.id),
                source.title.c_str());
  }

  if (sources.empty()) {
    return;
  }

  // Enumeration alone does not prove much: it works on backends that then fail
  // to produce pixels. M6 depends on real frames, so one is captured here.
  FrameSink sink;
  if (!capturer->SelectSource(sources.front().id)) {
    report("screen capture frame", false, "SelectSource failed");
    return;
  }
  capturer->Start(&sink);

  // X11 answers inside CaptureFrame, the XDG portal path does not: it has to
  // negotiate a session with the user first, so the result arrives later.
  constexpr auto kTimeout = std::chrono::seconds(15);
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (!sink.done && std::chrono::steady_clock::now() < deadline) {
    capturer->CaptureFrame();
    if (!sink.done) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
  }

  if (!sink.done) {
    report("screen capture frame", false, "no frame within 15s");
    return;
  }
  if (sink.result != webrtc::DesktopCapturer::Result::SUCCESS) {
    report("screen capture frame", false, "capture returned an error result");
    return;
  }

  report("screen capture frame", sink.width > 0 && sink.height > 0,
         std::to_string(sink.width) + "x" + std::to_string(sink.height) + ", " +
             std::to_string(sink.bytes / 1024) + " KiB");
}

void check_audio_devices() {
  const webrtc::Environment env = webrtc::CreateEnvironment();
  auto adm = webrtc::CreateAudioDeviceModule(env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  if (adm == nullptr || adm->Init() != 0) {
    report("audio device module", false, "could not initialize");
    return;
  }

  if (adm->RecordingDevices() < 0 || adm->PlayoutDevices() < 0) {
    report("audio device module", false, "device enumeration failed");
    adm->Terminate();
    return;
  }

  const int16_t recording = adm->RecordingDevices();
  const int16_t playout = adm->PlayoutDevices();
  report("audio device module", recording > 0 || playout > 0,
         "inputs: " + std::to_string(recording) + ", outputs: " + std::to_string(playout));

  // The buffers are oversized on purpose. Some platform backends have been
  // seen writing past the documented limit, and a spike must not crash while
  // it is busy proving the toolchain works.
  char name[webrtc::kAdmMaxDeviceNameSize * 4] = {};
  char guid[webrtc::kAdmMaxGuidSize * 4] = {};
  for (int16_t i = 0; i < recording; ++i) {
    if (adm->RecordingDeviceName(i, name, guid) == 0) {
      std::printf("        input  [%d] %s\n", i, name);
    }
  }
  for (int16_t i = 0; i < playout; ++i) {
    if (adm->PlayoutDeviceName(i, name, guid) == 0) {
      std::printf("        output [%d] %s\n", i, name);
    }
  }
  adm->Terminate();
}

/// Passes a std::string produced by libwebrtc into our own code and back.
///
/// This is the whole standard library question in one function. If libwebrtc
/// carries its own libc++ while dv::shared uses libstdc++, this either fails to
/// link or corrupts the string, which is exactly what has to be caught before
/// M4 starts.
void check_standard_library() {
#ifdef DV_SPIKE_WITH_SHARED
  const std::string room_id = "8F42A1";
  const std::string wire = dv::protocol::serialize(
      dv::protocol::Message{dv::protocol::JoinRoom{room_id, "user123", "Ana"}});

  const auto parsed = dv::protocol::parse(wire);
  const bool ok = parsed.ok() && std::holds_alternative<dv::protocol::JoinRoom>(parsed.value()) &&
                  std::get<dv::protocol::JoinRoom>(parsed.value()).room_id == room_id;

  report("std::string across ABI", ok, ok ? "dv::shared linked and interoperating" : "");
#else
  report("std::string across ABI", true,
         "skipped: this libwebrtc carries its own libc++, dv::shared not linked");
#endif
}

}  // namespace

int main() {
  std::printf("libwebrtc toolchain spike\n\n");

  webrtc::InitializeSSL();

  auto network_thread = webrtc::Thread::CreateWithSocketServer();
  auto worker_thread = webrtc::Thread::Create();
  auto signaling_thread = webrtc::Thread::Create();
  network_thread->SetName("network", nullptr);
  worker_thread->SetName("worker", nullptr);
  signaling_thread->SetName("signaling", nullptr);
  report("threads started",
         network_thread->Start() && worker_thread->Start() && signaling_thread->Start());

  auto factory = webrtc::CreatePeerConnectionFactory(
      network_thread.get(), worker_thread.get(), signaling_thread.get(),
      /*default_adm=*/nullptr, webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(), webrtc::CreateBuiltinVideoEncoderFactory(),
      webrtc::CreateBuiltinVideoDecoderFactory(), /*audio_mixer=*/nullptr,
      /*audio_processing=*/nullptr);
  report("peer connection factory", factory != nullptr);

  if (factory != nullptr) {
    webrtc::PeerConnectionInterface::RTCConfiguration configuration;
    configuration.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
    webrtc::PeerConnectionInterface::IceServer stun;
    stun.urls.emplace_back("stun:stun.l.google.com:19302");
    configuration.servers.push_back(stun);

    NullPeerConnectionObserver observer;
    webrtc::PeerConnectionDependencies dependencies(&observer);
    auto peer_connection =
        factory->CreatePeerConnectionOrError(configuration, std::move(dependencies));
    report("peer connection", peer_connection.ok(),
           peer_connection.ok() ? "" : peer_connection.error().message());

    if (peer_connection.ok()) {
      auto connection = peer_connection.MoveValue();
      connection->AddTransceiver(webrtc::MediaType::AUDIO);
      connection->AddTransceiver(webrtc::MediaType::VIDEO);

      auto offer_observer = webrtc::make_ref_counted<OfferObserver>();
      connection->CreateOffer(offer_observer.get(),
                              webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());

      // The callback lands on libwebrtc's signaling thread, not on this one,
      // so the main thread simply waits for it. Thread::Current() is null
      // here: the main thread is not wrapped by libwebrtc.
      for (int attempt = 0; attempt < 100 && !offer_observer->done(); ++attempt) {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }

      report("sdp offer", !offer_observer->sdp().empty(),
             offer_observer->sdp().empty()
                 ? offer_observer->error()
                 : std::to_string(offer_observer->sdp().size()) + " bytes");
      if (!offer_observer->sdp().empty()) {
        std::printf("\n--- offer ---\n%s-------------\n\n", offer_observer->sdp().c_str());
      }
      connection->Close();
    }
  }

  check_screen_capture();
  check_audio_devices();
  check_standard_library();

  webrtc::CleanupSSL();

  std::printf("\n%s\n", failures == 0 ? "spike passed" : "spike FAILED");
  return failures == 0 ? 0 : 1;
}
