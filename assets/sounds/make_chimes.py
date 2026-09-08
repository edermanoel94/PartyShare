"""Generates the two chimes the room plays when somebody arrives or leaves, and
the buzz it plays when somebody nudges you.

Kept out of the build, for the reason assets/ui/make_arrows.py is: they are a
few kilobytes each and a file in the repository beats a script nobody runs.
This is here so the sounds can be changed without opening an audio editor.

Two notes, both of which are the difference between a cue and an annoyance:

  * The two are the same interval played in opposite directions. Rising is
    somebody arriving and falling is somebody leaving, which is a convention
    old enough that nobody has to be taught it.
  * Every tone is enveloped. A sine that starts and stops at full amplitude
    ends with a click at each edge, and a click is the part people hear.
"""

import math
import pathlib
import struct

RATE = 44100
# Quiet. This plays over a call somebody is listening to, and a cue that has to
# compete with a voice is a cue that interrupts it.
PEAK = 0.22
# A perfect fifth, E5 and B5. High enough to sit above speech rather than in
# it, and consonant enough that hearing it forty times in a call is bearable.
LOW = 659.25
HIGH = 987.77
# Per tone. Two of them make a cue of a fifth of a second, which is under the
# length at which a sound stops reading as one event.
TONE_SECONDS = 0.10
# Of each tone, at each end. Long enough to kill the click, short enough that
# the pitch is still audible as a pitch.
FADE_SECONDS = 0.012

# The nudge. Not a cue about the room but a request from somebody in it, so it
# has to be told apart from an arrival by ear alone: lower, longer, and not a
# note. A tone whose loudness wobbles twenty-eight times a second reads as a
# buzz rather than a pitch, which is what a nudge sounded like where the idea
# comes from. G3, well under the fifth above, and a little louder than the
# chimes because the window is shaking as it plays and a sound that is quieter
# than the picture is a sound nobody connects to it.
NUDGE_HZ = 196.00
NUDGE_TREMOLO_HZ = 28.0
NUDGE_SECONDS = 0.40
NUDGE_PEAK = 0.30


def envelope(i, total, fade):
    """Raised cosine at both ends, flat in the middle."""
    if i < fade:
        return 0.5 - 0.5 * math.cos(math.pi * i / fade)
    if i >= total - fade:
        return 0.5 - 0.5 * math.cos(math.pi * (total - 1 - i) / fade)
    return 1.0


def tone(frequency, seconds):
    """One enveloped sine, as a list of floats between -1 and 1."""
    total = int(RATE * seconds)
    fade = int(RATE * FADE_SECONDS)
    samples = []
    for i in range(total):
        gain = envelope(i, total, fade)
        samples.append(PEAK * gain * math.sin(2 * math.pi * frequency * i / RATE))
    return samples


def buzz(frequency, tremolo, seconds, peak):
    """One enveloped sine with its amplitude wobbling at `tremolo` hertz."""
    total = int(RATE * seconds)
    fade = int(RATE * FADE_SECONDS)
    samples = []
    for i in range(total):
        t = i / RATE
        # Between 0.2 and 1.0 of the peak, so the wobble is heard as a buzz and
        # not as the sound switching off and on.
        wobble = 0.6 + 0.4 * math.sin(2 * math.pi * tremolo * t)
        gain = envelope(i, total, fade) * wobble
        samples.append(peak * gain * math.sin(2 * math.pi * frequency * t))
    return samples


def wav(path, samples):
    """Writes 16-bit mono PCM. The only format PlaySound is guaranteed to take."""
    frames = b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in samples)
    header = (
        b"RIFF"
        + struct.pack("<I", 36 + len(frames))
        + b"WAVEfmt "
        + struct.pack("<IHHIIHH", 16, 1, 1, RATE, RATE * 2, 2, 16)
        + b"data"
        + struct.pack("<I", len(frames))
    )
    pathlib.Path(path).write_bytes(header + frames)
    print("wrote", path, f"{len(frames) + 44} bytes")


def main():
    here = pathlib.Path(__file__).parent
    wav(here / "joined.wav", tone(LOW, TONE_SECONDS) + tone(HIGH, TONE_SECONDS))
    wav(here / "left.wav", tone(HIGH, TONE_SECONDS) + tone(LOW, TONE_SECONDS))
    wav(here / "nudge.wav", buzz(NUDGE_HZ, NUDGE_TREMOLO_HZ, NUDGE_SECONDS, NUDGE_PEAK))


if __name__ == "__main__":
    main()
