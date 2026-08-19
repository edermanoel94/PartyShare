#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <utility>

#include "video/video_frame.hpp"

namespace dv::client::video {

/// A short buffer between whoever captures frames and whoever encodes them,
/// which drops the oldest frame when it is full.
///
/// Dropping the oldest rather than the newest is the whole point. A screen
/// share that falls behind should show the present late by one frame, not the
/// past on time: with a queue that blocks or that drops new frames, a slow
/// encoder turns into seconds of accumulated delay and never recovers, and
/// section 5.2 of SPEC.md puts low latency first.
///
/// Small on purpose. At thirty frames a second, two frames is 66 ms of slack,
/// which absorbs a hiccup without hiding a real problem.
///
/// Thread safe. Push and pop are meant to be called from different threads.
class FrameQueue {
 public:
  static constexpr std::size_t kDefaultCapacity = 2;

  explicit FrameQueue(std::size_t capacity = kDefaultCapacity)
      : capacity_(capacity == 0 ? 1 : capacity) {}

  /// Adds a frame, discarding the oldest if there is no room. Returns true when
  /// nothing had to be discarded.
  bool push(VideoFrame frame) {
    const std::lock_guard<std::mutex> lock(mutex_);
    bool dropped = false;
    while (frames_.size() >= capacity_) {
      frames_.pop_front();
      ++dropped_;
      dropped = true;
    }
    frames_.push_back(std::move(frame));
    return !dropped;
  }

  /// Takes the oldest frame, or nothing when the queue is empty. Never blocks:
  /// the caller decides what to do with an idle moment.
  [[nodiscard]] std::optional<VideoFrame> pop() {
    const std::lock_guard<std::mutex> lock(mutex_);
    if (frames_.empty()) {
      return std::nullopt;
    }
    VideoFrame frame = std::move(frames_.front());
    frames_.pop_front();
    return frame;
  }

  void clear() {
    const std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
  }

  [[nodiscard]] std::size_t size() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return frames_.size();
  }

  [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

  /// How many frames have been discarded since the queue was created. A number
  /// that keeps climbing means the encoder cannot keep up with the capture.
  [[nodiscard]] std::uint64_t dropped() const {
    const std::lock_guard<std::mutex> lock(mutex_);
    return dropped_;
  }

 private:
  mutable std::mutex mutex_;
  std::deque<VideoFrame> frames_;
  std::size_t capacity_;
  std::uint64_t dropped_ = 0;
};

}  // namespace dv::client::video
