# 9. Shared screen audio

A screen share carries the picture and, on Windows, the sound the sharing machine
is playing. Somebody opens a browser, presses play on a video, and the other
participants hear it.

Windows only, for now. On Linux and macOS the capture does not exist and the
option does not appear.

## 1. The shape of the design

The sound travels **inside the sharer's own audio track**, mixed in after echo
cancellation and before the encoder. There is no second track, no second
connection and no change to the server.

That is a decision with consequences, and they are stated below rather than
discovered later.

Three things made it possible:

- Only one person shares at a time, guaranteed by `RoomManager`, so the sound can
  ride on that person's track without a new one.
- The SFU's offer already announces `stereo=1` and `maxaveragebitrate=96000`,
  inherited from libdatachannel's default Opus profile. The network path for
  music was already negotiated; there was simply no music entering it.
- libwebrtc has an official hook at exactly the right place.

### Why not a second track

libwebrtc has **one** `AudioDeviceModule` per `PeerConnectionFactory`, and
`AudioTransportImpl` hands the same captured frame to every `AudioSendStream`.
Two local audio tracks on one `PeerConnection` carry the same sound by
construction, which rules out the naive answer of "add a second `AudioTrack` fed
from the loopback".

Three ways in exist, and all three were checked against the m152 headers.

| | What it is | Verdict |
| --- | --- | --- |
| **A** | `webrtc::AudioFrameProcessor`, a documented hook that runs after the APM and before the encoder | **Chosen** |
| **B** | A second `PeerConnection` carrying only the screen audio | Registered as the evolution path, not built |
| **C** | `CreateAudioDeviceWithDataObserver` | Discarded |

**A** costs one new class, one extra field in `PeerConnectionFactoryDependencies`,
zero server change and zero renegotiation. Running after the APM is the whole
point: AEC3, noise suppression and AGC have already finished with the microphone,
and the music enters without passing through any of them. Mixing *before* the APM
would be the opposite — noise suppression at `kHigh` destroys music, and the AGC
pumps the volume at every note.

**B** buys real separation: independent mute, per-source volume, uncompromised
music quality. It costs a second ICE negotiation and a second DTLS handshake per
sharer, against the decision recorded in `media_router.hpp` — and, more seriously,
two separate transports share no RTP synchronisation group, so **there is no lip
sync guarantee** between the screen and its sound. For a video of somebody
talking, that is the defect people notice.

**C** receives `OnCaptureData` as a `const void*`, so writing there means a
`const_cast` over somebody else's buffer, and the mixing would happen before the
APM. The worst of both.

## 2. Capture

Windows 10 build 20348 and newer have **per-process loopback capture**, which is
exactly the case being asked for. `client/src/audio/loopback_capturer_windows.cpp`
activates it through `ActivateAudioInterfaceAsync` with
`AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK`.

Two modes, and the difference between them is a safety property, not a preference:

| Mode | Parameters | |
| --- | --- | --- |
| `process` | `INCLUDE_TARGET_PROCESS_TREE` with the chosen application's PID | That application only |
| `system` | `EXCLUDE_TARGET_PROCESS_TREE` with `GetCurrentProcessId()` | Everything the machine plays **except PartyShare itself** |

The exclusion is not a detail. Without it, the voices of the other participants
come out of the speakers, back in through the capture, and are sent to them
again — after the echo canceller, which has nothing left to cancel with. There is
no "capture everything" mode for that reason, and a Windows too old for
per-process loopback disables the feature rather than falling back to endpoint
loopback, which cannot exclude anything.

Two details of the implementation:

- **No format conversion.** With `AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM` the audio
  engine resamples and remixes to whatever is asked for, so we ask for 48 kHz,
  2 channels, 16 bits — the format the rest of the path already speaks — and
  there is no conversion left to write.
- **Silence has to be invented.** Per-process loopback produces no packets at all
  while the application is quiet, so the capture loop fills silence by the clock.
  Without that the track freezes and the receiver hears the last buffer stretched.

`client/src/audio/block_pacer.hpp` is the buffer between the WASAPI clock and the
10 ms clock libwebrtc pulls on. It is where drift becomes a decision: above the
watermark it drops the oldest block, below it delivers silence, and after a
starvation it waits to refill rather than alternating audio and silence every
10 ms.

The application list in Settings comes from `IAudioSessionManager2`, with this
process always excluded from it.

## 3. What the design costs, and what was done about it

Mixing into one track is cheap and it is not neutral. Four things break if
ignored; three of them have a fix in the mixer and one does not.

**Muting the microphone would have killed the sound.** `set_microphone_muted`
used to disable the whole track. Now it does that only when no screen audio is
running; with one in progress the track stays enabled and the microphone is
zeroed *inside* the mixer. The mute got more accurate as a result: it is exact
rather than a property of the track's state.

**The local speech indicator would have been lit permanently.** It used to come
from `outbound-rtp`'s `audio_level`, which is measured after the mix. It now comes
from the mixer, which measures the microphone alone, before the mute gain and
before anything is added.

**The microphone is not always 48 kHz.** The audio processing module delivers
whatever the capture device produces, and a headset in communications mode
produces 16 kHz. A `webrtc::PushResampler` in the adapter lifts the microphone up
to 48 kHz rather than pulling the screen audio down to 16 — music at 16 kHz has
nothing above 8 kHz, and the reason for sending it is precisely that it is not a
voice.

**Lowering a participant's volume also lowers what they are sharing.** This one
has no fix inside option A, and it is the honest price of it: the two sounds are
encoded together, and no receiver separates them again. The interface says so —
somebody sharing with sound appears as `(sharing with sound)` rather than
`(sharing)`, which is what explains why that person's volume slider now controls
two things.

One thing predicted to break and did not: the **remote** speech indicator.
libwebrtc computes the `audio-level` header extension *before* the frame
processor, so it reports the microphone alone and never the screen audio added
afterwards. Measured: a track carrying 94 kbps of music reported level 0.0002.
Nothing had to be done — and that same measurement nearly led to the opposite
conclusion, that the sound was not arriving at all.

## 4. Volume

`ScreenAudioMixer::mix` applies a gain to the screen side only, in Q8 fixed
point. The microphone is deliberately left out: it already has a gain control,
libwebrtc's, running before any of this, and two automatic controls fighting over
one signal are not a volume control.

Fixed point rather than floating, because this runs once per sample, a hundred
blocks a second, for the whole call, and the arithmetic beside it was already
integer. Eight bits rather than sixteen because the product has to fit in an
`int32`: a full-scale sample at the 200% ceiling is `32767 * 512`, which fits;
Q16 would not.

The ceiling is 200%. A boost clips in `saturate`, so the worst it does is sound
bad, and the alternative is worse: an application playing at a tenth of its scale
has no other way back, and telling somebody to go and turn Windows up is not a
volume control.

**The curve is linear, and that is a choice.** Half on the slider is half the
amplitude. Perceptual response would be different — loudness follows roughly the
cube root of amplitude, so half the amplitude sounds like about four fifths — and
a slider calibrated that way does almost all its audible work in the bottom
quarter of its travel. It stayed linear because this is not a listening volume:
it balances the screen against a microphone, and the number people look for is a
ratio between two sources, "half my own volume", not a position that *feels* like
half. `audio::screen_volume_ratio` is the function, isolated and tested, for the
day that decision is revisited.

**This changes what the whole room hears**, and there is no version of it that
does not. Lowering it lowers it for everyone.

## 5. Surface, configuration and protocol

**Settings**, not a share dialog — there is no share dialog, the button toggles
directly and the monitor is chosen in Settings. Two rows next to it: **Share
sound** (None / Everything but PartyShare / One application) and **Application**,
plus a hint line that explains a disabled control instead of leaving it dead. The
application list is re-read on every change of mode, because it is a list of what
is playing *now*.

**Configuration** is `[screen_audio] mode` and `volume_percent`, in
[chapter 3](03-configuration.md). The default is `system`, because sharing a
video and nobody hearing it is the surprise; and it is not a silent default,
since the dialog shows the choice before anything is shared. A mode this build
does not recognise reads as `none` — falling back to capturing the machine
because a word was not understood is the one unacceptable answer.

**The protocol** gained `has_audio` on `screen_share_started`, and the server
remembers it so it can tell somebody who joins mid-share. It is not needed for
the sound to arrive — it arrives on the voice track regardless. It is what lets
the interface show the speaker icon. It is read with a false fallback: a peer
built before the field existed does not send it, and "does not send" means "no
sound", not "malformed message", which would stop an older client's share being
announced at all.

**Sound never brings the share down.** Somebody who asked to share a video and
was told no because their Windows is a year too old would rather have the picture
than nothing. So the screen goes up, `screen_audio_active()` reports what actually
happened, `screen_audio_failure()` reports why, and the status bar writes
`Sharing without sound: ...`. A silent share that was meant to have sound is
otherwise indistinguishable from one that never would have.

`audio.bitrate_kbps` finally reaches the offer through
`MediaRouter::Options::opus_max_bitrate_kbps`. Its default moved from 48 to 96,
which is not a behaviour change: what the wire already carried was
libdatachannel's own default of 96. Leaving it at 48 while wiring it up for the
first time would have halved the sound of every share.

## 6. Measured

`MediaEndToEndTest.TheSoundOfASharedScreenReachesTheOtherParticipant` stages
nothing: the test process plays a tone, the loopback capture hears it, the mixer
folds it into Ana's track after the echo canceller, Opus encodes it, the SFU
forwards it and Bruno's client receives it.

The instrument is bitrate, not level — for the reason above, level reports the
microphone alone. The comparison is the **same share** with and without sound,
because comparing against no share at all would also be comparing mono against
stereo:

| The sharer's audio track | Bitrate |
| --- | --- |
| Sharing, nothing playing | ~1 kbps |
| Sharing, a tone playing | ~100 kbps |

The first row is worth knowing: a share whose application is quiet costs nothing.
Opus opens the stereo stream at the negotiated ceiling and settles within a couple
of seconds once it sees the content is silence — so the ceiling is what the link
has to be able to carry, not what it will carry. The first version of this test
measured the peak during that ramp and compared 100 kbps against 105.

The metrics line reports `sound shared (N% silent)`. The percentage is the half
that matters: a capture delivering only silence is indistinguishable, from
outside, from one that is working.

The bandwidth arithmetic is in [chapter 12](12-requirements.md). In short: worst
case, 96 kbps more inbound for the sharer and 96 kbps outbound per viewer, which
next to 3.3 Mbps of picture does not change how the machine is sized.

## 7. The defect this was written to catch

With Ana's microphone muted, the track still cost 1 kbps: nothing was being
mixed. Every capture counter was green — 590 blocks delivered, 3 of silence — and
the share was silent for everybody.

What was missing was a counter on the *other* buffer. With both exposed, the
answer was one line: `mixed=0 starved=0 dropped=280260`. The buffer between the
capture and the encoder filled and nobody emptied it, because `mix()` was taking
the pass-through path. The warning left there said why:

```text
Screen audio: not mixing, because a captured frame is 160 frames of 1 channels
and this expects 480 of at most two
```

160 frames is 16 kHz. That is the resampler above, and the test now asserts
`screen_audio_mixed_blocks > 0` — the assertion that would have caught it, which
is why the test is written that way.

## 8. Out of scope

- **Linux and macOS.** A PulseAudio or PipeWire loopback module on one,
  `ScreenCaptureKit` with `SCStreamOutputType.audio` on the other. Whole
  implementations, not caveats: the `LoopbackCapturer` interface is ready for
  them and `loopback_stub.cpp` answers `capture_unavailable` until then. Linux was
  validated to the extent that the stub is correct and the loopback test skips
  itself.

  The configured mode is resolved against the machine at startup, in
  `client/src/main.cpp`, so a client here reads `screen_audio.mode` from the file
  and still starts on `none`.
  Without that step the default of `"system"` reached every share, each one
  ending with a warning about a feature the settings dialog had already greyed
  out.
  The reason is said once instead, in the startup log, and only to a
  configuration that asked for sound.
- **Window capture.** Sharing only the browser window instead of the monitor is a
  video-side change and does not depend on this.
- **Per-source volume at the receiver.** Only option B offers it. The volume here
  is applied by the sender, before encoding, which is why it applies to the whole
  room.
