#pragma once

#include <optional>
#include <string>

#include <rtc/rtc.hpp>

namespace dv::server::sfu {

/// What every audio m-line this server offers is made of.
///
/// One struct rather than three arguments because the parts have to agree
/// with each other: the RED fmtp names the Opus payload type, and the Opus
/// fmtp changes shape depending on whether RED is there. See
/// docs/16-audio-plan.md, step 1.
struct AudioCodecs {
  /// Opus at 48 kHz, section 9 of SPEC.md. 111 is the payload type every
  /// browser and libwebrtc build uses for it.
  int opus_payload_type = 111;
  /// The ceiling written into the Opus fmtp, in kbps.
  int opus_max_bitrate_kbps = 128;
  /// RED, RFC 2198: every packet carries the previous frame again, so an
  /// isolated loss on either leg is repaired at the receiver without anybody
  /// asking for anything. Nothing means Opus alone, which is what the offer
  /// carried before this existed.
  ///
  /// 63 is what Chrome uses for it. Any number nothing else in the offer takes
  /// would do: libwebrtc looks the codec up by name, not by number.
  std::optional<int> red_payload_type = 63;
  /// `a=rtcp-fb:<opus> nack` on the m-line, which is what turns on, in a
  /// libwebrtc client, both halves of retransmission: five seconds of send
  /// history to answer a NACK from, and a jitter buffer that asks for what it
  /// missed. What RED does not cover, two packets lost back to back, this
  /// does. See docs/16-audio-plan.md, step 2.
  ///
  /// The client's answer never echoes the line, and that is not a refusal.
  /// libwebrtc gives its own audio codecs no `nack` and intersects feedback
  /// on the way to an answer, but it reads its send parameters off the offer
  /// and lets the receive side follow the send side (`pc/channel.cc`,
  /// SetRemoteContent_w). So the offer is what counts, and this server
  /// writes every offer.
  bool nack = true;
};

/// The Opus fmtp this server offers, which is what decides what a participant
/// encodes: their answer describes what they accept to receive, and every audio
/// m-line here is one directional.
///
/// libdatachannel's own DEFAULT_OPUS_AUDIO_PROFILE, with the ceiling made
/// configurable and the in-band FEC made optional. Stereo is not decoration: it
/// is what lets a screen share carry the two channels of whatever is playing
/// rather than the average of them.
///
/// `in_band_fec` is off while RED is on, and that is deliberate. Opus in-band
/// FEC only exists in its SILK modes, and at a stereo ceiling this high the
/// encoder runs CELT, where the flag does nothing - until loss is reported,
/// when it pushes the encoder into SILK to make FEC possible, which caps the
/// audio at 8 kHz. With the redundancy in RED there is nothing left to buy
/// with that trade.
[[nodiscard]] std::string opus_profile(int max_bitrate_kbps, bool in_band_fec);

/// Adds the codecs to an audio m-line, RED first.
///
/// The order is not cosmetic. libwebrtc only pairs RED with the codec it
/// names when RED appears before that codec in the m-line, and libdatachannel
/// writes the codecs in the order they were added.
void add_audio_codecs(rtc::Description::Audio& media, const AudioCodecs& codecs);

}  // namespace dv::server::sfu
