#pragma once

#include <algorithm>
#include <array>
#include <string_view>

#include "video/frame_size.hpp"

namespace dv::client::video {

/// One row of the resolution menu.
///
/// A ceiling rather than a size to send at: `fit_within` keeps the monitor's
/// aspect ratio inside the box, so choosing 1080p on a 16:10 screen sends
/// 1728x1080 and not a stretched 1920x1080. See video/frame_size.hpp.
struct ScreenResolution {
  Size size;
  /// What to put in front of the user. Named by the height, the way every
  /// other program that offers this does, because "1080p" is what people
  /// recognise and "1920x1080" is what they have to read.
  std::string_view label;
};

/// Section 5.2 of SPEC.md asks for 1280x720, which is the first row and stays
/// the default. 1080p is here because a shared editor at 720p on a 27 inch
/// monitor is legible only if the person sharing enlarges their font first.
///
/// Nothing above 1080p on purpose. Past there the cost is not the encoder, it
/// is that a 4K screen scaled into a call window is 4K worth of bandwidth to
/// look at 1080p worth of pixels.
inline constexpr std::array<ScreenResolution, 2> kScreenResolutions{{
    {.size = {.width = 1280, .height = 720}, .label = "720p"},
    {.size = {.width = 1920, .height = 1080}, .label = "1080p"},
}};

/// 30 is what section 5.2 of SPEC.md asks for, and what a shared document or
/// an editor wants. 60 is for the things 30 makes unwatchable: scrolling, a
/// terminal that redraws, anything animated.
inline constexpr std::array<int, 2> kScreenFrameRates{30, 60};

/// Section 6 of SPEC.md puts 1280x720 at 30 FPS between 1.5 and 3 Mbps.
constexpr int kBaseMaxBitrateKbps = 3000;

/// Where the settings dialog stops offering a bitrate. Past here the limit on
/// a screen share is not the encoder, it is the link.
constexpr int kMaxRecommendedBitrateKbps = 8000;
constexpr int kMinRecommendedBitrateKbps = 500;

/// The maximum bitrate worth giving the encoder for a frame size and a rate.
///
/// Scaled from the 3 Mbps above: linearly in pixels, because twice the pixels
/// is twice the residual to code, and by half again for a doubled frame rate
/// rather than by double, because a screen mostly does not change in the 16 ms
/// between two frames and a macroblock that did not change costs almost
/// nothing to skip.
///
/// Below 30 FPS the number does not fall. This is a ceiling the encoder may
/// use, not a target it will spend, and lowering it there would only take away
/// the headroom a keyframe needs.
///
/// A recommendation and nothing more: it is what the settings dialog compares
/// the configured maximum against before it says anything. It never replaces a
/// bitrate somebody chose.
[[nodiscard]] constexpr int recommended_max_bitrate_kbps(Size size, int fps) noexcept {
  if (size.empty() || fps <= 0) {
    return kBaseMaxBitrateKbps;
  }

  // Integer arithmetic on purpose, as in fit_within: the same choice has to
  // produce the same number on every machine.
  constexpr long long kBasePixels = 1280LL * 720;
  const long long pixels = static_cast<long long>(size.width) * size.height;
  long long kbps = static_cast<long long>(kBaseMaxBitrateKbps) * pixels / kBasePixels;
  kbps = kbps * (60 + std::max(0, fps - 30)) / 60;
  return static_cast<int>(
      std::clamp<long long>(kbps, kMinRecommendedBitrateKbps, kMaxRecommendedBitrateKbps));
}

/// A bitrate range for the encoder, in kbps: where it starts and where it may
/// grow to.
struct BitrateRange {
  int min_kbps = 0;
  int max_kbps = 0;
};

/// The range automatic mode picks for a frame size, a rate and a floor.
///
/// The ceiling is `recommended_max_bitrate_kbps` unchanged, because that is
/// already the answer to "what is worth giving the encoder here".
///
/// The start is half of it, which is the 1.5 to 3 Mbps of section 6 of SPEC.md
/// held as a ratio rather than as two constants that only suit 720p30. Half is
/// deliberately short of the ceiling: the start is a guess made before any
/// feedback exists, and one that overshoots is paid for in loss during the
/// first seconds, while one that undershoots costs only the moment the
/// estimator needs to climb.
///
/// Never below `floor_kbps`, and never a maximum under its minimum, because
/// dv::config::validate refuses both and this is the one place that can
/// produce them without anybody typing a number.
[[nodiscard]] constexpr BitrateRange recommended_bitrate_kbps(Size size, int fps,
                                                              int floor_kbps) noexcept {
  // The floor first, and the ceiling lifted to meet it when it has to. A floor
  // above what the size is worth is a strange configuration, but it is a legal
  // one, and the alternative here is a clamp whose lower bound sits above its
  // upper bound, which is undefined behaviour rather than a bad number.
  const int lowest = std::max(1, floor_kbps);
  const int maximum = std::max(recommended_max_bitrate_kbps(size, fps), lowest);
  const int minimum = std::clamp(maximum / 2, lowest, maximum);
  return {.min_kbps = minimum, .max_kbps = maximum};
}

}  // namespace dv::client::video
