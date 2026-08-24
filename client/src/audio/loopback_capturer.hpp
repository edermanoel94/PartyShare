#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <variant>

#include <dv/core/result.hpp>

#include "audio/block_pacer.hpp"

namespace dv::client::audio {

/// What the capture is allowed to hear.
enum class LoopbackMode : std::uint8_t {
  /// Everything the machine is playing except this process.
  ///
  /// The exception is not a convenience. Without it the capture picks up the
  /// other participants coming out of the speakers and sends them back into the
  /// call, after the echo canceller has already done its work and with nothing
  /// left to remove it. See docs/audio-da-tela-compartilhada.md, section 6.
  System,
  /// One application and the processes below it, and nothing else.
  Process,
};

/// What the capture is doing, in the shape of video::ScreenCaptureStats.
struct LoopbackStats {
  std::uint64_t blocks_delivered = 0;
  /// Blocks that carried silence because nothing was playing. On Windows this
  /// is the normal state of a quiet application rather than a fault: the
  /// process loopback produces no packets at all until there is sound.
  std::uint64_t blocks_silent = 0;
  std::uint64_t frames_captured = 0;
  /// Frames thrown away because the buffer was over its watermark. See
  /// BlockPacer.
  std::uint64_t frames_dropped = 0;
};

/// Captures what an application, or the whole machine, is playing.
///
/// The boundary exists for the same reason video::ScreenCapturer's does:
/// everything above it should not know which operating system it is on, and
/// should build and be testable without one. Windows has this; Linux and macOS
/// do not yet, and there the factory fails rather than pretending.
///
/// Blocks arrive on the capturer's own thread, never on the caller's, and one
/// every 10 ms once capture starts - including while the application is silent.
/// A consumer can therefore treat the block rate as a clock.
class LoopbackCapturer {
 public:
  /// One block of interleaved stereo 16-bit PCM at 48 kHz, exactly
  /// `kSamplesPerBlock` samples long.
  ///
  /// The span is only valid for the duration of the call: whoever needs to keep
  /// the audio copies it. Called on the capture thread every 10 ms, so it must
  /// not block - a slow sink costs blocks.
  using BlockSink = std::function<void(std::span<const std::int16_t> block)>;

  /// Capture ended on its own. The application closing its audio session, the
  /// default playback device disappearing, and the audio service restarting all
  /// arrive this way.
  using ErrorSink = std::function<void(Error error)>;

  LoopbackCapturer() = default;
  virtual ~LoopbackCapturer() = default;

  LoopbackCapturer(const LoopbackCapturer&) = delete;
  LoopbackCapturer& operator=(const LoopbackCapturer&) = delete;
  LoopbackCapturer(LoopbackCapturer&&) = delete;
  LoopbackCapturer& operator=(LoopbackCapturer&&) = delete;

  /// Starts capturing. `process_id` is read only in `LoopbackMode::Process`,
  /// where it names the root of the process tree to listen to; in
  /// `LoopbackMode::System` it is ignored and this process is excluded instead.
  ///
  /// Fails with `capture_unavailable` when the system cannot do it at all -
  /// notably on Windows before build 20348, which has no per-process loopback -
  /// and with `invalid_value` for a process id of zero in `Process` mode.
  ///
  /// Unlike a screen share there is no `source_not_found`: Windows accepts a
  /// process id that no longer exists and simply delivers silence, and a
  /// process can exit a microsecond after any check would have passed.
  [[nodiscard]] virtual Result<std::monostate> start(LoopbackMode mode,
                                                     std::uint32_t process_id) = 0;

  /// Stops capturing and joins the capture thread. Safe to call when not
  /// running, and safe to call from inside a sink.
  virtual void stop() = 0;

  [[nodiscard]] virtual bool capturing() const = 0;

  [[nodiscard]] virtual LoopbackStats stats() const = 0;
};

/// True when this build and this machine can capture what is playing.
///
/// Two questions folded into one on purpose here, unlike media::HardwareEncoding
/// which keeps them apart: there is nothing a user can do about either answer,
/// so the only thing worth knowing is whether to offer the option.
[[nodiscard]] bool loopback_capture_is_available() noexcept;

[[nodiscard]] Result<std::unique_ptr<LoopbackCapturer>> create_loopback_capturer(
    LoopbackCapturer::BlockSink blocks, LoopbackCapturer::ErrorSink errors);

}  // namespace dv::client::audio
