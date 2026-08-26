#include "ui/chimes.hpp"

#include <cstddef>

#include <dv/logging/logger.hpp>

#include <QApplication>
#include <QByteArray>
#include <QFile>
#include <QIODevice>
#include <QString>

#include "ui/sound.hpp"

namespace dv::ui {
namespace {

/// The bytes of one chime, read once and then kept.
///
/// Kept, and not read per play, for two reasons. The obvious one is that a
/// file read on the interface thread every time somebody joins is work for
/// nothing. The one that matters is ui::play_wav's contract: the buffer has to
/// outlive the sound, and the sound outlives the call that started it. A
/// function local static is the shortest lifetime that is obviously long
/// enough.
[[nodiscard]] const QByteArray& bytes_of(Chime chime) {
  static const QByteArray joined = [] {
    QFile file(QStringLiteral(":/sounds/joined.wav"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
  }();
  static const QByteArray left = [] {
    QFile file(QStringLiteral(":/sounds/left.wav"));
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
  }();
  return chime == Chime::Joined ? joined : left;
}

/// Not atomic, and it does not need to be: set from main() before there is a
/// window, and afterwards only from the interface thread, which is also the
/// only thread play_chime may be called from.
bool enabled = true;

}  // namespace

void set_chimes_enabled(bool on) {
  enabled = on;
}

bool chimes_enabled() {
  return enabled;
}

void play_chime(Chime chime) {
  if (!enabled) {
    return;
  }

  const QByteArray& wav = bytes_of(chime);
  if (wav.isEmpty()) {
    // Compiled into the executable, so an empty one is a build that went wrong
    // rather than a machine that is missing something. Worth a line in the log
    // and not worth a sound: beeping here would hide it.
    DV_LOG_WARN("chime: the sound is not in the resources, so nothing will be played");
    return;
  }

  if (play_wav(wav.constData(), static_cast<std::size_t>(wav.size()))) {
    return;
  }

  // One sound for both events, so it says that the room changed without
  // saying how. Still better than silence on a platform whose half of
  // ui::play_wav nobody has written yet.
  QApplication::beep();
}

}  // namespace dv::ui
