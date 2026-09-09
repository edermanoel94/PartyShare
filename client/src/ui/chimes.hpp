#pragma once

#include <cstdint>

namespace dv::ui {

/// The three things a room can tell you without words: two about its
/// membership, and one from somebody in it.
///
/// The base type is named for the same reason config::UnknownOptions names
/// one: three values do not need four bytes, and clang-tidy holds the project
/// to it.
enum class Chime : std::uint8_t {
  /// A rising fifth. Somebody arrived.
  Joined,
  /// The same fifth falling. Somebody left.
  Left,
  /// A short buzz, lower than the other two. Somebody nudged you, and the
  /// window is shaking as this plays. See MainWindow::apply_nudge.
  Nudge,
};

/// Turns the chime on or off for the whole process.
///
/// Process wide, and not a member of anything, because the chime already is:
/// it plays through one platform voice that has no notion of which window
/// asked for it. A per-window switch would be a promise the layer underneath
/// cannot keep.
///
/// main() sets it from `[ui] room_sounds` at startup, and the settings dialog
/// moves it as the box is ticked - before anything is written to the file, so
/// that turning it off is silent at once rather than at the next launch.
void set_chimes_enabled(bool on);

/// Whether play_chime would make a sound. What the settings dialog shows.
[[nodiscard]] bool chimes_enabled();

/// Plays the cue for `chime`, or does nothing when it is switched off or the
/// platform cannot.
///
/// Deliberately not gated on whether the window has the focus, which is the
/// difference between this and the balloon beside it. A notification is about
/// the room and is pointless to somebody already looking at it; a chime is a
/// cue *from* the room, and the person it is most for is the one watching a
/// shared screen with the participant list scrolled out of sight.
///
/// Never fails, from the caller's point of view. A cue that did not play is
/// not worth interrupting a call over, so the ladder ends in silence rather
/// than in an error: the native path, then QApplication::beep, then nothing.
///
/// Call it from the interface thread.
void play_chime(Chime chime);

}  // namespace dv::ui
