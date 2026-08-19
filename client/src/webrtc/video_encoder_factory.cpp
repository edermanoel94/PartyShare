#include "webrtc/video_encoder_factory.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <api/video_codecs/builtin_video_encoder_factory.h>
#include <api/video_codecs/video_encoder_software_fallback_wrapper.h>

#include <dv/logging/logger.hpp>

#include "webrtc/hardware_encoder.hpp"

namespace dv::client::media {
namespace {

/// Hands out a hardware encoder backed by a software one.
///
/// The list of formats is the software factory's, unchanged. That is
/// deliberate: what a machine can encode in hardware varies by card and by
/// driver, and negotiating a codec that turns out to have no hardware behind
/// it has to end in software rather than in a failed call. So the offer says
/// what the software can always do, and the hardware is used when it happens
/// to cover it.
class HardwareAcceleratedVideoEncoderFactory : public webrtc::VideoEncoderFactory {
 public:
  explicit HardwareAcceleratedVideoEncoderFactory(bool prefer_hardware)
      : software_(webrtc::CreateBuiltinVideoEncoderFactory()), prefer_hardware_(prefer_hardware) {}

  std::vector<webrtc::SdpVideoFormat> GetSupportedFormats() const override {
    return software_->GetSupportedFormats();
  }

  webrtc::VideoEncoderFactory::CodecSupport QueryCodecSupport(
      const webrtc::SdpVideoFormat& format, std::optional<std::string> scalability_mode,
      std::optional<webrtc::Resolution> resolution) const override {
    CodecSupport support =
        software_->QueryCodecSupport(format, std::move(scalability_mode), resolution);
    // The one thing this factory knows that the software one does not.
    // libwebrtc uses it to decide how hard it may push the encoder.
    support.is_power_efficient = using_hardware();
    return support;
  }

  std::unique_ptr<webrtc::VideoEncoder> Create(const webrtc::Environment& env,
                                               const webrtc::SdpVideoFormat& format) override {
    std::unique_ptr<webrtc::VideoEncoder> software = software_->Create(env, format);
    if (!using_hardware()) {
      return software;
    }

    std::unique_ptr<webrtc::VideoEncoder> hardware = create_hardware_encoder(env, format);
    if (hardware == nullptr) {
      DV_LOG_INFO("Media: no hardware encoder for {}, encoding it in software", format.name);
      return software;
    }
    if (software == nullptr) {
      // Nothing to fall back to. Better the hardware alone than nothing at
      // all, and libwebrtc will report the failure if it cannot start.
      return hardware;
    }

    DV_LOG_INFO("Media: encoding {} with {}, with software as fallback", format.name,
                hardware_encoder_support().implementation);
    // libwebrtc's own wrapper rather than one of ours: it is what Chrome ships
    // to switch a running stream from a hardware encoder that started failing
    // to the software one, keyframe and all, without the call noticing.
    return webrtc::CreateVideoEncoderSoftwareFallbackWrapper(
        env, std::move(software), std::move(hardware), /*prefer_temporal_support=*/false);
  }

 private:
  [[nodiscard]] bool using_hardware() const {
    return prefer_hardware_ && hardware_encoder_support().available;
  }

  std::unique_ptr<webrtc::VideoEncoderFactory> software_;
  bool prefer_hardware_;
};

}  // namespace

std::unique_ptr<webrtc::VideoEncoderFactory> create_video_encoder_factory(bool prefer_hardware) {
  const HardwareEncoderSupport support = hardware_encoder_support();
  if (!prefer_hardware) {
    DV_LOG_INFO("Media: hardware encoding is switched off, the screen is encoded in software");
  } else if (support.available) {
    DV_LOG_INFO("Media: hardware encoding available through {}", support.implementation);
  } else {
    DV_LOG_INFO("Media: no hardware encoding ({}), the screen is encoded in software",
                support.detail);
  }
  return std::make_unique<HardwareAcceleratedVideoEncoderFactory>(prefer_hardware);
}

}  // namespace dv::client::media
