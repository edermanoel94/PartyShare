#pragma once

#include <cstddef>
#include <cstdint>

namespace dv::client::audio {

/// How the gate moves, in milliseconds.
///
/// Three numbers and not one, because the three ends of a sentence are
/// different problems. The attack is how fast the first word gets through, and
/// it is short because every millisecond of it is a millisecond of that word
/// heard at half volume. The hold is how long a pause may last before it counts
/// as the end of the sentence, and it is long because a breath between two
/// clauses is longer than it feels. The release is how the room goes away, and
/// it is neither: fast enough not to sound like a fade, slow enough not to
/// click.
struct VoiceGateTiming {
  int attack_ms = 10;
  int hold_ms = 300;
  int release_ms = 100;
};

/// The envelope of a voice gate: a gain that is one while somebody is speaking
/// and zero while nobody is, with the three transitions above between the two.
///
/// Only the envelope. Who decides whether a block has a voice in it is the
/// caller's business, and it is kept out of here so that this can be tested by
/// saying "voice, voice, silence" rather than by synthesising speech. The
/// libwebrtc side is media::VoiceGateProcessor, which asks the library's
/// detector and hands the answer here, one 10 ms block at a time.
///
/// The gain is applied in place and moves by one step per sample rather than
/// per block, so that opening and closing are ramps and not edges: a gain that
/// jumps from zero to one between two samples is a click, and a click at the
/// start of every sentence is worse than the noise the gate was for.
class VoiceGate {
 public:
  explicit VoiceGate(VoiceGateTiming timing = {});

  /// Forgets everything and prepares for blocks at `sample_rate_hz`. The gate
  /// starts open, with a full hold ahead of it: the first thing anybody hears
  /// after a reset is whatever the microphone carries, and the gate closes only
  /// once it has heard the room for a hold's worth and found no voice in it.
  void reset(int sample_rate_hz);

  /// One block. `voiced` is the detector's word on it, and the gain that
  /// follows from that word - and from the words before it - is applied to the
  /// `num_frames` samples of each of the `num_channels` channels in place.
  void apply(bool voiced, float* const* channels, std::size_t num_channels, std::size_t num_frames);

  /// Whether the last block was let through: it had a voice in it, or the hold
  /// since the last one that did has not run out.
  [[nodiscard]] bool open() const noexcept { return open_; }
  /// The gain at the end of the last block, from zero to one.
  [[nodiscard]] float gain() const noexcept { return gain_; }
  /// Blocks seen since the reset, and how many of them the gate was closed for.
  [[nodiscard]] std::uint64_t blocks() const noexcept { return blocks_; }
  [[nodiscard]] std::uint64_t closed_blocks() const noexcept { return closed_blocks_; }

 private:
  VoiceGateTiming timing_;
  std::size_t hold_samples_ = 0;
  float attack_step_ = 1.0F;
  float release_step_ = 1.0F;
  /// Samples of silence still allowed before the gate closes.
  std::size_t hold_remaining_ = 0;
  float gain_ = 1.0F;
  bool open_ = true;
  std::uint64_t blocks_ = 0;
  std::uint64_t closed_blocks_ = 0;
};

}  // namespace dv::client::audio
