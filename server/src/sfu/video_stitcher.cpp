#include "sfu/video_stitcher.hpp"

namespace dv::server::sfu {
namespace {

/// Whether `later` comes after `earlier` in a 16 bit sequence space, RFC 3550
/// section 3: the difference is read as signed, so that 0 is one past 65535
/// rather than 65536 behind it.
[[nodiscard]] bool is_after(std::uint16_t later, std::uint16_t earlier) {
  return static_cast<std::int16_t>(later - earlier) > 0;
}

/// How far apart two sequence numbers are, whichever way round they are.
[[nodiscard]] std::uint16_t distance(std::uint16_t a, std::uint16_t b) {
  const auto signed_gap = static_cast<std::int16_t>(a - b);
  return static_cast<std::uint16_t>(signed_gap < 0 ? -signed_gap : signed_gap);
}

}  // namespace

VideoStitcher::Rewritten VideoStitcher::rewrite(const std::string& source_user_id,
                                                std::uint16_t sequence, std::uint32_t timestamp) {
  const std::lock_guard<std::mutex> lock(mutex_);

  if (!started_) {
    // The first packet defines the series. It goes out exactly as it came in,
    // so that a call where nobody ever hands the screen over is byte for byte
    // what it was before this class existed.
    started_ = true;
    source_ = source_user_id;
    sequence_offset_ = 0;
    timestamp_offset_ = 0;
    last_in_sequence_ = sequence;
    last_in_timestamp_ = timestamp;
    last_out_sequence_ = sequence;
    last_out_timestamp_ = timestamp;
    return Rewritten{.sequence = sequence, .timestamp = timestamp, .rebased = false};
  }

  // A new participant, an encoder that restarted, or a gap too wide to be loss.
  const bool rewound = static_cast<std::int32_t>(timestamp - last_in_timestamp_) <
                       -static_cast<std::int32_t>(kMaxOrdinaryRewind);
  const bool new_series = source_user_id != source_ ||
                          distance(sequence, last_in_sequence_) > kMaxOrdinaryJump || rewound;

  if (new_series) {
    // Start immediately after everything this track has already carried, so
    // that the viewer sees one packet more of the same stream rather than the
    // beginning of another one.
    sequence_offset_ = static_cast<std::uint16_t>(last_out_sequence_ + 1 - sequence);
    timestamp_offset_ = last_out_timestamp_ + kHandoverTicks - timestamp;
    source_ = source_user_id;
    last_in_sequence_ = sequence;
    last_in_timestamp_ = timestamp;
  }

  const auto out_sequence = static_cast<std::uint16_t>(sequence + sequence_offset_);
  const std::uint32_t out_timestamp = timestamp + timestamp_offset_;

  // Only ever forwards. A packet that arrives out of order keeps the number the
  // offset gives it, which is what lets the viewer put it back where it belongs
  // and what lets a retransmission answer the NACK that asked for it, but it
  // must not drag the high water mark back with it: the next handover starts
  // from there.
  if (is_after(out_sequence, last_out_sequence_)) {
    last_out_sequence_ = out_sequence;
    last_out_timestamp_ = out_timestamp;
  }
  if (is_after(sequence, last_in_sequence_)) {
    last_in_sequence_ = sequence;
    last_in_timestamp_ = timestamp;
  }

  return Rewritten{.sequence = out_sequence, .timestamp = out_timestamp, .rebased = new_series};
}

std::string VideoStitcher::source() const {
  const std::lock_guard<std::mutex> lock(mutex_);
  return source_;
}

}  // namespace dv::server::sfu
