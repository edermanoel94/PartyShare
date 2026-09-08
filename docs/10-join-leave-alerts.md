# 10. Join and leave alerts

When somebody enters or leaves a room, the client says so in two ways:

| | Joining | Leaving |
| --- | --- | --- |
| **Notification** — balloon plus a taskbar flash | only if the window is not focused | never |
| **Sound** — a chime | only if the balloon did *not* go up | always |

Always exactly one sound per event. The balloon brings the system's own sound
with it and there is no way to ask for it silently, so the chime gives way when
the balloon appears. With the window focused there is no balloon, and the chime
plays.

The asymmetry is deliberate. A chime is a cue **from** the room, and whoever
needs it most is the person looking at a shared screen with the participant list
out of view. A notification is a cue **about** the room, and it is useless to
somebody already looking at the list. And somebody leaving is news for the room,
not for the operating system — the chime already said it, and a balloon per
departure would be a queue of them at the end of every call.

The chime can be turned off: `[ui] room_sounds` in `config.ini`, or the **Room
sounds** box in Settings. On by default — a cue nobody asked for is easier to turn
off than a cue nobody knows exists is to find. The balloon has no switch of ours;
whoever wants it silent uses the system's own.

## 1. The notification, and what `QSystemTrayIcon` costs

`ui::Notifier` uses two things from Qt and nothing else:
`QSystemTrayIcon::showMessage` for the balloon, and `QApplication::alert` to
light the taskbar button. Both are portable and cost nothing to package.

**This is a decision, not a default nobody questioned.** The native Windows path
was written, it worked, and it was removed.

**What that bought:** no shortcut is created in the user's Start menu. The native
path required one — [Microsoft's
documentation](https://learn.microsoft.com/windows/win32/shell/enable-desktop-toast-with-appusermodelid)
is categorical that an unpackaged desktop application raises no toast unless a
shortcut carrying `System.AppUserModel.ID` exists, and the platform silently
discards the toast of a process it cannot name. Creating that shortcut was a real
change to the installing machine. Five COM/WinRT libraries and a ~290 line file
went with it.

**What it cost, and it is worth knowing:**

1. **Delivery is not guaranteed.** The `QSystemTrayIcon` documentation says so in
   as many words: *"messages may not appear at all. Hence, it should not be
   relied upon as the sole means for providing critical information."* That is
   why `QApplication::alert` is raised alongside it and always — it is the half
   that can be trusted.
2. **The balloon plays its own sound, and cannot be asked for silence.** On
   Windows this is the legacy `Shell_NotifyIcon` balloon, which the shell converts
   into a toast and sonifies. The native API accepted `<audio silent="true"/>`;
   `showMessage` has no parameter for it.

   Since the balloon cannot be silenced, the chime is: `Notifier::notify` returns
   whether a balloon went up, and `MainWindow::apply_participants` plays the chime
   only when one did not. The cost is that the sound of somebody joining
   **depends on focus** — our chime with the window in front, the system's with it
   behind. Two different sounds for one event, which is less bad than two sounds
   at once. Note that `false` does not promise the balloon was *seen* — Qt warns
   that it may not appear — which is exactly why the `alert` goes up regardless.
3. **The tray icon became permanent.** It used to be created only when the native
   path failed; now it *is* the channel, and a balloon needs an icon to hang from.
   It is created in the `Notifier` constructor — adding the icon and asking it to
   speak in the same instant is the kind of ordering that works only on the
   machine where it was written.

The permanent icon is why `main()` now installs a `QApplication::windowIcon` from
the resources. On Windows the `.rc` already gives Explorer and the taskbar an
image, but that one is not reachable as a `QIcon`.

## 2. The sound: `PlaySound`, and why not `QSoundEffect`

Qt's answer would be `QSoundEffect`, which lives in **Qt Multimedia**. On Windows
Qt Multimedia plays through the FFmpeg backend, so two fifth-of-a-second chimes
would put a module and its codec plugins into the build and the installer. The Qt
installed on the build machine does not even carry the module.

`PlaySound` from `winmm` does the same in five lines, and that is what
`client/src/ui/sound_windows.cpp` uses. Three details:

- **`SND_MEMORY`** — the sound is compiled into the executable rather than
  installed beside it. Writing the resource to a temporary file just to be able to
  name a path would be a temporary file to clean up.
- **`SND_NODEFAULT`** — without it, a buffer `PlaySound` cannot interpret is
  answered with the machine's default beep, which would turn a broken asset into a
  sound that appears to work. Tested: a buffer of garbage returns `false`.
- **`SND_ASYNC`, and the buffer has to outlive the sound.** `PlaySound` reads
  from the buffer on its own thread, so `chimes.cpp` keeps the bytes in a
  function-local `static`: the shortest lifetime that is obviously long enough.

One voice only, deliberately: a second call cuts the first off, so a burst of
arrivals is one sound rather than a pile of them.

On Linux and macOS `ui::play_wav` returns `false` (`sound_stub.cpp`) and
`chimes.cpp` falls back to `QApplication::beep()` — one sound for both events,
which does not tell joining from leaving. That is the honest state until somebody
writes the ALSA and CoreAudio halves.

## 3. The sounds

`assets/sounds/joined.wav` and `left.wav`, generated by
`assets/sounds/make_chimes.py` — the same pattern as `assets/ui/make_arrows.py`:
script versioned, output versioned, outside the build.

They are the same interval — a perfect fifth, E5 and B5 — played in both
directions. Rising is somebody arriving, falling is somebody leaving; the
convention is old enough that nobody has to learn it. 200 ms, 16-bit mono PCM,
~17 KB each, with a raised-cosine envelope on both ends of each tone: a sine that
starts and stops at full amplitude clicks, and the click is the part people hear.

The peak is low, 0.22. This plays over a call somebody is listening to, and a cue
that has to compete with a voice is a cue that interrupts it.

## 4. When the alert fires

The server sends no join and no leave event: it broadcasts the whole participant
list every time it changes. Joining and leaving are therefore the two halves of
the difference between two lists, and that is how `MainWindow::apply_participants`
computes them — arrivals from who is in the new list and was not in the old,
departures the other way round, which is the half the new list cannot show because
whoever left is not in it.

Four rules:

- **The first list after entering a room announces nothing.** It is whoever was
  already there, and announcing it would greet somebody joining a room of five
  with five balloons and five chimes. It seeds the set and stays quiet.
- **You are never announced to yourself.** The list includes whoever is reading it.
- **One sound per update, not one per person.** An update carrying an arrival and
  a departure at once plays the arrival: the platform has one voice, and two cues
  talking over each other say less than one.
- **No balloon while the window is focused.** Somebody looking at the list saw the
  row appear.

The notification is decided before the sound, because its answer is what says
whether the chime should play.

## 5. The nudge

Somebody in the room can ask for your attention by name — `Nudge Ana` on the
participant list's right-click menu, which now opens for everybody and not only
for an administrator — and on your side four things happen at once, in
`MainWindow::apply_nudge`:

- **One line in the chat**, `Ana nudged you`, so that it is there when you come
  back to look. The sender sees `You nudged Ana` and nothing else.
- **The balloon or the buzz**, by the rule of section 4: the balloon goes up only
  when the window is not the one being looked at, and the buzz plays only when
  no balloon did, because a balloon brings its own sound. The buzz is
  `assets/sounds/nudge.wav`, from the same script as the chimes: a G3 with its
  loudness wobbling twenty-eight times a second, 400 ms, so it reads as a buzz
  rather than a note and is told apart from an arrival by ear alone. Both the
  buzz and the chimes are behind `room_sounds`; the two below are not, because
  the switch is about noise.
- **The operating system's own flash.** This is the one native call the
  notification path deliberately does not make. On Windows it is `FlashWindowEx`
  with `FLASHW_ALL | FLASHW_TIMERNOFG`: title bar and taskbar button, until the
  window is brought to the front. It needs no shortcut, no `AppUserModelID` and
  no library — which is why it is native where the toast of section 1 is not.
  Every other platform gets `QApplication::alert`, Qt's portable version of the
  same idea, until somebody writes the other halves of
  `client/src/ui/attention.hpp` (`attention_windows.cpp` is the model;
  `attention_stub.cpp` says what they would call).
- **The window shakes**, for just under half a second: thirty frames at sixteen
  milliseconds, side to side with a swing that starts at twelve pixels and falls
  to nothing, and a little up and down at half the reach. A maximized or full
  screen window does not move — moving one changes its state — so its contents
  shake instead. One shake at a time; a second nudge during the first joins it.

What the server allows — both in the room, not yourself, not while silenced in
chat, and one every five seconds per sender — is section 4.12 of
[chapter 6](06-protocol.md). A refusal comes back as an ordinary error and is
shown where every other error is.
