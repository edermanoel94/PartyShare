#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace dv::client::video {

/// One captured or decoded frame, in packed BGRA.
///
/// BGRA is what every desktop capturer libwebrtc has produces, and what a Qt
/// image wants at the other end, so it is the format that costs no conversion
/// at either edge. The conversion to I420 happens once, inside the encoder, on
/// the way out.
///
/// Move only, on purpose. A 1280x720 frame is 3.5 MB, and thirty of those a
/// second is not something to copy by accident. Section 5.2 of SPEC.md puts
/// low latency and efficient CPU use at the top of the list, and a type that
/// cannot be copied silently is the cheapest way to keep both.
class VideoFrame {
 public:
  VideoFrame() = default;

  VideoFrame(int width, int height, std::vector<std::uint8_t> pixels)
      : width_(width), height_(height), pixels_(std::move(pixels)) {}

  VideoFrame(const VideoFrame&) = delete;
  VideoFrame& operator=(const VideoFrame&) = delete;
  VideoFrame(VideoFrame&&) noexcept = default;
  VideoFrame& operator=(VideoFrame&&) noexcept = default;
  ~VideoFrame() = default;

  [[nodiscard]] int width() const noexcept { return width_; }
  [[nodiscard]] int height() const noexcept { return height_; }
  /// Bytes per row. Packed, so always four bytes per pixel.
  [[nodiscard]] int stride() const noexcept { return width_ * kBytesPerPixel; }
  [[nodiscard]] bool empty() const noexcept { return pixels_.empty(); }

  [[nodiscard]] const std::uint8_t* data() const noexcept { return pixels_.data(); }
  [[nodiscard]] std::uint8_t* data() noexcept { return pixels_.data(); }
  [[nodiscard]] std::size_t size() const noexcept { return pixels_.size(); }

  /// Hands the buffer back so it can be filled again instead of reallocated.
  /// The frame is empty afterwards.
  [[nodiscard]] std::vector<std::uint8_t> take_pixels() noexcept {
    width_ = 0;
    height_ = 0;
    return std::move(pixels_);
  }

  static constexpr int kBytesPerPixel = 4;

 private:
  int width_ = 0;
  int height_ = 0;
  std::vector<std::uint8_t> pixels_;
};

}  // namespace dv::client::video
