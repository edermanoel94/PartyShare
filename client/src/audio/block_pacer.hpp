#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace dv::client::audio {

/// Section 9 of SPEC.md puts the call at 48 kHz, and the whole audio path below
/// and above this deals in blocks of 10 ms.
inline constexpr int kSampleRateHz = 48000;
/// Stereo, unlike the microphone. What the screen carries is music and video,
/// and folding those to one channel throws away the half of the mix that makes
/// them worth sending at all.
inline constexpr std::size_t kChannels = 2;
inline constexpr std::size_t kFramesPerBlock = static_cast<std::size_t>(kSampleRateHz) / 100;
inline constexpr std::size_t kSamplesPerBlock = kFramesPerBlock * kChannels;

/// Sits between the thread the operating system wakes when it has audio and the
/// clock that has to hand out exactly one block every 10 ms.
///
/// Two clocks meet here and neither can be told to wait. WASAPI delivers on the
/// render device's clock, in packets of whatever size the audio engine felt
/// like accumulating; the encoder wants 480 frames on the dot, on the system
/// clock. The two crystals are not the same crystal, so over a long call one
/// side is always slowly gaining on the other.
///
/// So this holds a short ring and enforces the two rules that keep the drift
/// from turning into a defect:
///
///   - Too full means late. Audio waiting in a buffer is latency and nothing
///     else, so above `high_watermark_frames` the oldest is thrown away until
///     only `target_frames` is left. Dropping the oldest rather than the newest
///     is the same choice video::FrameQueue makes, for the same reason: being
///     late by a fraction of a second beats being behind by a growing one.
///
///   - Too empty means silence, not a stutter. A process loopback capture
///     produces nothing at all while the application is quiet - Windows does
///     not send packets of silence - so starving is the normal state, not an
///     error. What must never happen is the block being skipped: the encoder
///     would then hear the audio speed up.
///
/// After a starve the ring refills to `prime_frames` before handing anything
/// out again. Without that, one late packet becomes a block of silence every
/// 10 ms for as long as the capture stays a hair behind, which is audibly worse
/// than a single gap.
///
/// Thread safe: push and take are meant to be called from different threads,
/// though the Windows capturer happens to call both from one.
class BlockPacer {
 public:
  struct Options {
    /// A tenth of a second of audio waiting is a tenth of a second late.
    std::size_t high_watermark_frames = static_cast<std::size_t>(kSampleRateHz) / 10;
    /// Where a trim leaves the ring.
    std::size_t target_frames = static_cast<std::size_t>(kSampleRateHz) / 20;
    /// How much has to arrive before the first block goes out, and again after
    /// every starve.
    std::size_t prime_frames = static_cast<std::size_t>(kSampleRateHz) / 50;
  };

  struct Stats {
    std::uint64_t frames_pushed = 0;
    /// Thrown away because the ring was over the high watermark. A number that
    /// keeps climbing means the consumer is slower than the capture.
    std::uint64_t frames_dropped = 0;
    std::uint64_t blocks_taken = 0;
    /// Blocks that carried silence because there was nothing to carry.
    std::uint64_t blocks_silent = 0;
  };

  // Two constructors rather than one with a default argument. GCC will not
  // have `Options` defaulted in the declaration - "default member initializer
  // required before the end of its enclosing class", because the defaults of a
  // nested type are not available while the enclosing one is still being
  // defined. MSVC takes it, which is how it got written that way.
  BlockPacer() : BlockPacer(Options{}) {}

  explicit BlockPacer(const Options& options) : options_(options) {
    // Room for the watermark plus a couple of blocks, so that a push that
    // arrives just under the limit still fits before the trim runs.
    ring_.assign((options_.high_watermark_frames + (kFramesPerBlock * 2)) * kChannels, 0);
    if (options_.target_frames > options_.high_watermark_frames) {
      options_.target_frames = options_.high_watermark_frames;
    }
  }

  /// Adds interleaved stereo samples. The span has to hold whole frames.
  void push(std::span<const std::int16_t> interleaved) {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (const std::int16_t sample : interleaved) {
      write(sample);
    }
    stats_.frames_pushed += interleaved.size() / kChannels;
    trim();
  }

  /// Adds `frames` of silence. Separate from push because Windows reports a
  /// silent packet with a flag rather than with a buffer of zeros, and
  /// materialising those zeros only to copy them would be work for nothing.
  void push_silence(std::size_t frames) {
    const std::lock_guard<std::mutex> lock(mutex_);
    for (std::size_t i = 0; i < frames * kChannels; ++i) {
      write(0);
    }
    stats_.frames_pushed += frames;
    trim();
  }

  /// Fills `block` with exactly one block, which is `kSamplesPerBlock` samples.
  ///
  /// Returns true when that block came from the capture and false when it is
  /// silence the clock made up. Either way the block is filled: a caller that
  /// skipped it on false would be handing the encoder a shorter second.
  bool take(std::span<std::int16_t> block) {
    const std::lock_guard<std::mutex> lock(mutex_);
    ++stats_.blocks_taken;

    if (!primed_ && frames() >= options_.prime_frames) {
      primed_ = true;
    }
    if (!primed_ || frames() < kFramesPerBlock) {
      // Starving un-primes, so the next block waits for the ring to refill
      // rather than alternating between audio and silence.
      primed_ = false;
      std::fill(block.begin(), block.end(), static_cast<std::int16_t>(0));
      ++stats_.blocks_silent;
      return false;
    }

    for (std::size_t i = 0; i < kSamplesPerBlock && i < block.size(); ++i) {
      block[i] = read();
    }
    return true;
  }

  /// Frames waiting to go out.
  [[nodiscard]] std::size_t buffered_frames() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return frames();
  }

  [[nodiscard]] Stats stats() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
  }

  /// Forgets everything buffered, for a capture that is starting over.
  void clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    head_ = 0;
    size_ = 0;
    primed_ = false;
  }

 private:
  [[nodiscard]] std::size_t frames() const { return size_ / kChannels; }

  void write(std::int16_t sample) {
    if (size_ == ring_.size()) {
      // Cannot happen after a trim, and dropping the oldest is the right answer
      // if it ever does.
      read();
      stats_.frames_dropped += 1;
    }
    ring_[(head_ + size_) % ring_.size()] = sample;
    ++size_;
  }

  std::int16_t read() {
    const std::int16_t sample = ring_[head_];
    head_ = (head_ + 1) % ring_.size();
    --size_;
    return sample;
  }

  void trim() {
    if (frames() <= options_.high_watermark_frames) {
      return;
    }
    const std::size_t excess = frames() - options_.target_frames;
    for (std::size_t i = 0; i < excess * kChannels; ++i) {
      read();
    }
    stats_.frames_dropped += excess;
  }

  mutable std::mutex mutex_;
  Options options_;
  std::vector<std::int16_t> ring_;
  std::size_t head_ = 0;
  std::size_t size_ = 0;
  bool primed_ = false;
  Stats stats_;
};

}  // namespace dv::client::audio
