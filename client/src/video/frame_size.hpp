#pragma once

#include <algorithm>

namespace dv::client::video {

struct Size {
  int width = 0;
  int height = 0;

  [[nodiscard]] constexpr bool empty() const noexcept { return width <= 0 || height <= 0; }

  friend constexpr bool operator==(const Size&, const Size&) = default;
};

/// The size to send a monitor at, given the ceiling from the configuration.
///
/// Section 5.2 of SPEC.md asks for 1280x720. A monitor is rarely exactly that,
/// so the frame is fitted inside the box rather than stretched into it: the
/// aspect ratio is kept, and text on a shared screen stays the shape it was.
///
/// Never upscales. A monitor smaller than the box is sent as it is, because
/// inventing pixels costs bandwidth and CPU and adds nothing to look at.
///
/// The result is always even in both dimensions. I420 stores one chroma sample
/// per two by two block of luma, so an odd side has no representation and
/// whoever receives it has to crop.
[[nodiscard]] constexpr Size fit_within(Size source, Size bounds) noexcept {
  if (source.empty()) {
    return {};
  }
  if (bounds.empty()) {
    bounds = source;
  }

  // Integer arithmetic on purpose: the same input has to produce the same size
  // on every machine, and a float here would make that depend on rounding.
  int width = source.width;
  int height = source.height;
  if (width > bounds.width) {
    height = static_cast<int>(static_cast<long long>(height) * bounds.width / width);
    width = bounds.width;
  }
  if (height > bounds.height) {
    width = static_cast<int>(static_cast<long long>(width) * bounds.height / height);
    height = bounds.height;
  }

  // Rounded down to even, then floored at the smallest frame that still has a
  // chroma sample in it.
  width = std::max(2, width - (width % 2));
  height = std::max(2, height - (height % 2));
  return {.width = width, .height = height};
}

}  // namespace dv::client::video
