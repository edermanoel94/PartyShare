#include "sfu/audio_description.hpp"

#include <optional>
#include <string>
#include <utility>

namespace dv::server::sfu {

std::string opus_profile(int max_bitrate_kbps, bool in_band_fec) {
  std::string profile = "minptime=10;maxaveragebitrate=" + std::to_string(max_bitrate_kbps * 1000) +
                        ";stereo=1;sprop-stereo=1";
  if (in_band_fec) {
    profile += ";useinbandfec=1";
  }
  return profile;
}

void add_audio_codecs(rtc::Description::Audio& media, const AudioCodecs& codecs) {
  const std::optional<int>& red_payload_type = codecs.red_payload_type;
  if (red_payload_type.has_value()) {
    // RFC 2198 wrapped around RFC 7587: the same clock and channel count as
    // the codec it carries, and an fmtp naming that codec for the primary and
    // for the redundant block. libwebrtc refuses a RED whose clock or channels
    // differ from the codec it names, and wants at least two entries in the
    // fmtp.
    rtc::Description::Media::RtpMap red(*red_payload_type);
    red.format = "red";
    red.clockRate = 48000;
    red.encParams = "2";
    const std::string opus = std::to_string(codecs.opus_payload_type);
    red.fmtps.push_back(opus + "/" + opus);
    media.addRtpMap(std::move(red));
  }
  media.addOpusCodec(codecs.opus_payload_type,
                     opus_profile(codecs.opus_max_bitrate_kbps, !red_payload_type.has_value()));
  if (codecs.nack) {
    // On Opus and not on RED: libwebrtc reads the feedback off the codec it
    // encodes with, and RED is the wrapping, not the codec.
    if (auto* opus = media.rtpMap(codecs.opus_payload_type); opus != nullptr) {
      opus->addFeedback("nack");
    }
  }
}

}  // namespace dv::server::sfu
