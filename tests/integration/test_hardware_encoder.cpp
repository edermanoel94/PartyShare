// The hardware encoder, driven directly.
//
// In one process, with no network, no SFU and no SRTP. That is deliberate:
// MediaEndToEndTest.TheScreenIsEncodedBySomethingThatSaysWhatItIs proves the
// same thing through a whole call, which makes it the right test for the
// pipeline and the wrong one for the encoder. A call that cannot be set up on
// the machine running the suite says nothing at all about whether the card can
// encode.
//
// What this covers is exactly what the platform port changes: loading the
// driver libraries, probing them, and getting H.264 out the other end.
//
// It lives in the media suite rather than the unit one because it includes
// libwebrtc headers, and the strict warning set of dv_unit_tests cannot be
// applied to a translation unit that does. See client/CMakeLists.txt.

#include <cstdint>
#include <mutex>
#include <vector>

#include <api/environment/environment.h>
#include <api/environment/environment_factory.h>
#include <api/video/i420_buffer.h>
#include <api/video/video_frame.h>
#include <api/video_codecs/video_codec.h>
#include <api/video_codecs/video_encoder.h>
#include <gtest/gtest.h>
#include <modules/video_coding/include/video_error_codes.h>

#include "webrtc/hardware_encoder.hpp"

namespace {

using dv::client::media::create_hardware_encoder;
using dv::client::media::hardware_encoder_support;
using dv::client::media::HardwareEncoderSupport;

constexpr int kWidth = 1920;
constexpr int kHeight = 1080;
constexpr int kFramerate = 60;
constexpr int kBitrateKbps = 6000;

/// Collects what the encoder produces, from whichever thread it produces it on.
class Collector : public webrtc::EncodedImageCallback {
 public:
  Result OnEncodedImage(const webrtc::EncodedImage& image,
                        const webrtc::CodecSpecificInfo* /*codec_specific*/) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++frames_;
    if (first_.empty() && image.size() > 0) {
      first_.assign(image.data(), image.data() + image.size());
      first_was_key_ = image._frameType == webrtc::VideoFrameType::kVideoFrameKey;
    }
    return Result(Result::OK);
  }

  /// Counted rather than ignored. An encoder that accepts every frame and
  /// drops them all under rate control would otherwise look like one that
  /// silently produced nothing, and the two want different investigations.
  void OnFrameDropped(std::uint32_t /*rtp_timestamp*/, int /*spatial_id*/,
                      bool /*is_end_of_temporal_unit*/) override {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++dropped_;
  }

  [[nodiscard]] int dropped() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }

  [[nodiscard]] int frames() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return frames_;
  }
  [[nodiscard]] std::vector<std::uint8_t> first() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return first_;
  }
  [[nodiscard]] bool first_was_key() {
    const std::lock_guard<std::mutex> lock(mutex_);
    return first_was_key_;
  }

 private:
  std::mutex mutex_;
  int frames_ = 0;
  int dropped_ = 0;
  std::vector<std::uint8_t> first_;
  bool first_was_key_ = false;
};

/// A frame with something in it that changes, so the encoder has residual to
/// code rather than a flat field it can express in almost no bits.
[[nodiscard]] webrtc::VideoFrame make_frame(int index) {
  webrtc::scoped_refptr<webrtc::I420Buffer> buffer = webrtc::I420Buffer::Create(kWidth, kHeight);
  for (int row = 0; row < kHeight; ++row) {
    std::uint8_t* line =
        buffer->MutableDataY() + (static_cast<std::ptrdiff_t>(row) * buffer->StrideY());
    for (int column = 0; column < kWidth; ++column) {
      line[column] = static_cast<std::uint8_t>((column + row + (index * 16)) & 0xFF);
    }
  }
  // Chroma flat: this is a screen share, and a screen is mostly grey.
  std::fill(buffer->MutableDataU(),
            buffer->MutableDataU() + (buffer->StrideU() * ((kHeight + 1) / 2)), 128);
  std::fill(buffer->MutableDataV(),
            buffer->MutableDataV() + (buffer->StrideV() * ((kHeight + 1) / 2)), 128);

  return webrtc::VideoFrame::Builder()
      .set_video_frame_buffer(buffer)
      .set_timestamp_us(static_cast<std::int64_t>(index) * (1'000'000 / kFramerate))
      .set_rotation(webrtc::kVideoRotation_0)
      .build();
}

[[nodiscard]] webrtc::VideoCodec h264_settings() {
  webrtc::VideoCodec codec;
  codec.codecType = webrtc::kVideoCodecH264;
  codec.width = kWidth;
  codec.height = kHeight;
  codec.maxFramerate = kFramerate;
  codec.startBitrate = kBitrateKbps;
  codec.maxBitrate = kBitrateKbps;
  codec.minBitrate = kBitrateKbps / 4;
  return codec;
}

TEST(HardwareEncoderTest, TheSupportQueryAlwaysGivesAReason) {
  const HardwareEncoderSupport support = hardware_encoder_support();

  // On any machine, with or without a card. "no hardware encoding" with
  // nothing after it is the answer that sends somebody through driver
  // documentation for an afternoon.
  EXPECT_FALSE(support.detail.empty());

  if (support.compiled_in) {
    // Named even when none of the backends found anything, so the reason can
    // be attached to something.
    EXPECT_FALSE(support.implementation.empty());
  } else {
    EXPECT_FALSE(support.available) << "nothing compiled in cannot be available";
  }

  std::printf("hardware encoding: %s (%s) via %s\n",
              support.available ? "available" : "unavailable", support.detail.c_str(),
              support.implementation.empty() ? "nothing" : support.implementation.c_str());
  std::fflush(stdout);
}

TEST(HardwareEncoderTest, EncodesH264OnThisMachine) {
  const HardwareEncoderSupport support = hardware_encoder_support();
  if (!support.available) {
    // A CI runner without a card is not a failure. The reason is asserted
    // above; here there is simply nothing to drive.
    GTEST_SKIP() << "no hardware encoder here: " << support.detail;
  }

  const webrtc::Environment env = webrtc::CreateEnvironment();
  std::unique_ptr<webrtc::VideoEncoder> encoder =
      create_hardware_encoder(env, webrtc::SdpVideoFormat("H264"));
  ASSERT_NE(encoder, nullptr) << support.implementation
                              << " said it was available and then "
                                 "refused to make an H.264 encoder";

  Collector collector;
  ASSERT_EQ(encoder->RegisterEncodeCompleteCallback(&collector), WEBRTC_VIDEO_CODEC_OK);

  const webrtc::VideoCodec codec = h264_settings();
  const webrtc::VideoEncoder::Settings settings(webrtc::VideoEncoder::Capabilities(false),
                                                /*number_of_cores=*/1,
                                                /*max_payload_size=*/1200);
  ASSERT_EQ(encoder->InitEncode(&codec, settings), WEBRTC_VIDEO_CODEC_OK)
      << "1080p60 is what the settings dialog now offers, so it is what the card has to take";

  // The first one asked for as a keyframe, the way libwebrtc opens a stream.
  const std::vector<webrtc::VideoFrameType> key = {webrtc::VideoFrameType::kVideoFrameKey};
  const std::vector<webrtc::VideoFrameType> delta = {webrtc::VideoFrameType::kVideoFrameDelta};

  constexpr int kFrames = 10;
  for (int index = 0; index < kFrames; ++index) {
    const webrtc::VideoFrame frame = make_frame(index);
    ASSERT_EQ(encoder->Encode(frame, index == 0 ? &key : &delta), WEBRTC_VIDEO_CODEC_OK)
        << "frame " << index;
  }

  EXPECT_GT(collector.frames(), 0) << "the encoder accepted every frame and produced nothing, "
                                      "having dropped "
                                   << collector.dropped();
  EXPECT_TRUE(collector.first_was_key()) << "the first frame was asked for as a keyframe";

  // Annex-B. libwebrtc's H.264 packetiser splits on start codes, so a card
  // that handed back length-prefixed AVCC would produce a stream that encodes
  // perfectly and decodes nowhere.
  const std::vector<std::uint8_t> first = collector.first();
  ASSERT_GE(first.size(), 4U);
  const bool annex_b = (first[0] == 0 && first[1] == 0 && first[2] == 1) ||
                       (first[0] == 0 && first[1] == 0 && first[2] == 0 && first[3] == 1);
  EXPECT_TRUE(annex_b) << "the first bytes are " << static_cast<int>(first[0]) << " "
                       << static_cast<int>(first[1]) << " " << static_cast<int>(first[2]) << " "
                       << static_cast<int>(first[3]) << ", which is not an Annex-B start code";

  EXPECT_EQ(encoder->Release(), WEBRTC_VIDEO_CODEC_OK);
}

}  // namespace
