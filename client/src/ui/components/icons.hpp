#pragma once

#include <cstdint>

#include <QColor>
#include <QPixmap>

class QPainter;
class QRectF;

namespace dv::ui {

/// Every pictogram the interface draws.
///
/// Painted, not loaded. An SVG needs the qsvg image plugin, which the release
/// job prunes, so an icon that was a file would be an icon missing from the
/// packaged build and nowhere else; a PNG needs three files per colour per
/// scheme. A path drawn by QPainter takes its colour from the theme at the
/// moment it is painted and is sharp at any scale factor.
///
/// All of them are strokes on a 24 unit grid with round caps, so that any two
/// sit beside each other at the same weight.
enum class Icon : std::uint8_t {
  Rooms,
  Shield,
  Settings,
  Microphone,
  MicrophoneOff,
  Screen,
  Fullscreen,
  ExitFullscreen,
  Signal,
  Leave,
  Copy,
  Plus,
  Search,
  Send,
  Smile,
  Users,
  Chat,
  Check,
  Info,
  Warning,
  Error,
  Close,
  Key,
  Volume,
  ArrowLeft,
};

/// Draws `icon` centred in `box`, as large as the box's shorter side, in
/// `colour`. The painter comes back as it was handed over.
void paint_icon(QPainter& painter, Icon icon, const QRectF& box, const QColor& colour);

/// The same drawing as a pixmap `size` logical pixels square, for the places
/// that want an image rather than a paint call: a QPushButton's icon, a
/// QLabel. Rendered at `device_pixel_ratio`, so it is not scaled up on a
/// dense screen.
[[nodiscard]] QPixmap icon_pixmap(Icon icon, int size, const QColor& colour,
                                  qreal device_pixel_ratio);

}  // namespace dv::ui
