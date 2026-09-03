# 16. Audio plan

Twelve steps, one pull request each, in the order of the audio review of
3 September 2026. Each step starts only after the previous one has been merged,
tested with two clients and published in a tag.

The review found the happy path well built: Opus in stereo at 48 kHz, AEC3, the
screen audio mixed in after the processing module. What it found weak was five
things. The audio has no real packet loss repair, despite `useinbandfec=1`. Loss
on the SFU to listener leg never reaches the sender. The gain control is the
legacy AGC1 and the noise suppressor is fixed at its high level. A microphone
that opens at 16 kHz goes unnoticed. And nothing measures concealment, so a
complaint about audio cannot be triaged.

Line numbers below are from master at 0.1.47 and from the libwebrtc m152 source
in `~/.cache/partyshare/webrtc/src`.

## 1. How the plan moves

**One step, one pull request.** A `feat/…` branch from master. The next step
starts from master with the previous one already in it.

**Four gates between steps.** CI green on all ten jobs, the two client manual
test, merge into master, tag published. None is skipped.

**Before opening the pull request.** clang-format writing CRLF, clang-tidy run
locally, and the `build/full` tree actually compiling.

**Going back is a key, not a revert.** Every step that changes behaviour is
born with a configuration key that turns it off. The key is written in the
step.

**Documentation closes the step.** Chapters 3, 9 and 12 change in the same pull
request as the code, and so does the commented `config.ini`.

**The instrument before the repair.** The concealment counters of step 9 go
into the first commit of step 1. Without them, neither of the first two steps
has an acceptance criterion.

## 2. The sequence

| # | Step | Where | Effort | Key to go back |
| --- | --- | --- | --- | --- |
| 1 | RED | Server | ~2 days | `audio.redundancy = false` |
| 2 | Audio NACK | Server | ~2 days | `audio.retransmission = false` |
| 3 | Listener loss in the sender's RR | Decision | Only if FEC comes back | — |
| 4 | AGC2 | Client | 1 day | `automatic_gain_control = false` |
| 5 | Noise suppression level | Client | 1 day | `noise_suppression_level = high` |
| 6 | 16 kHz microphone warning | Client | Half a day | No key: it is a warning |
| 7 | 128 kbps ceiling | Server config | 1 hour | `audio.bitrate_kbps = 96` |
| 8 | Limiter in the mixer | Client | 1 day | `screen_audio.limiter = false` |
| 9 | Concealment metrics | Client | 1 day | No key |
| 10 | Audio in congestion control | Spike | 2 days, with a stop | — |
| 11 | A second track for the screen sound | Client and server | 2 to 3 weeks | Stop criterion in the spike |
| 12 | The new Windows ADM | Toolchain | 1 week | `DV_AUDIO_LEGACY_ADM=1` |

## 3. Step 1: RED, every packet carries the previous frame

Server. Medium, about two days. Key `audio.redundancy`.

An isolated loss, on either leg, stops being a hole. It depends on nobody's
feedback: the redundancy travels inside the stream itself.

**Why first.** Today the audio has no repair at all. `useinbandfec=1` does
nothing in CELT mode, which is where Opus sits at 96 kbps stereo, and when loss
is reported it trades the full band for SILK at 8 kHz. Verified in
`opus_encoder.c:1441` and `:1559`.

### Changes

- `server/src/sfu/media_router.hpp`: `Options::red_payload_type = 63`, the
  number Chrome uses. libwebrtc only turns RED on when it comes before Opus in
  the m-line (`webrtc_voice_engine.cc:1546`), and libdatachannel writes the
  codecs in the order they were added, so RED is added first.
- `media_router.cpp`: a function `add_red(rtc::Description::Audio&, int red_pt,
  int opus_pt)` that builds `RtpMap(63)` with `format = "red"`,
  `clockRate = 48000`, `encParams = "2"`, fmtp `111/111`, and calls
  `addRtpMap`. Used on both audio m-lines: inbound at line 267, outbound at
  line 520.
- `forward_audio`, line 769: stop stamping the Opus payload type on every
  packet. The SFU dictates the payload types and they are the same for
  everybody, so the type that arrived is the type that leaves.
  `Outbound.payload_type` keeps serving the sender report.
- A counter `audio_red_packets_received_` in the router, by payload type. The
  libwebrtc stats report the primary codec, not RED, so it is the SFU that says
  whether RED is on the wire.
- The Opus profile: drop `useinbandfec=1` when RED is on. With the redundancy
  in RED the encoder no longer needs to fall to SILK and lose the high band
  under loss. `stereo`, `sprop-stereo`, `minptime` and `maxaveragebitrate`
  stay.
- Configuration: `[audio] redundancy = true`, read in `shared/src/config` and
  applied in `server/src/main.cpp:365` like the other SFU options. It is the
  key to go back.
- Client, required: nothing. m152 lists `red/48000/2` by itself and matches
  the fmtp in `CheckRedParameters`.
- Client, first commit: read `concealed_samples`, `concealment_events` and
  `total_samples_received` from inbound-rtp in `collect()`
  (`libwebrtc_media_session.cpp:1252`) into `AudioStats`. It is the step's
  instrument.
- Docs: chapter 12 (worst case becomes 192 kbps per leg), chapter 9 sections 1
  and 6 (the offer now carries RED), chapter 3 (the new key).

### Tests

- Unit: move the audio m-line construction into pure functions in
  `sfu/audio_description.hpp` and test them: RED before Opus, fmtp `111/111`,
  no `useinbandfec` with RED, `useinbandfec` without RED.
- Integration, in `ACallSurvivesFivePercentPacketLoss`:
  `audio_red_packets_received() > 0`, and `concealment_events` per second
  below a third of the value without RED. Measure the baseline first, with the
  key off, and write the number into the test.
- Manual: two clients, shared music, injected loss. Listen. With
  `DV_WEBRTC_LOG=info`, check that the send codec shows RED.

### Acceptance

The 5% test shows concealment falling, the audio bitrate stays at most 2× what
it was, and an old client against a new server keeps talking. The reverse too:
an old server simply does not offer RED.

### Risk and way back

Doubles the audio: up to 192 kbps per leg with the 96 ceiling.
`redundancy = false` on the server, no rebuild.

## 4. Step 2: audio NACK on both legs

Server. Medium, about two days. Key `audio.retransmission`.

What RED does not cover, bursts of two packets or more, is asked for again.
The client already knows how to ask and how to retransmit; what is missing is
the SFU on both sides.

**Verified.** With `nack` on the send codec, libwebrtc keeps 5 s of history to
retransmit from (`kNackRtpHistoryMs`); with `nack` on the receive codec it
turns on NetEq's NackTracker (`webrtc_voice_engine.cc:322` and `:2074`).
Nothing to do on the client.

**Verified while building it, and easy to misread.** libwebrtc gives its own
audio codecs no `nack` feedback (`media/base/codec.cc` adds it to video only)
and intersects feedback parameters on the way to an answer
(`pc/codec_vendor.cc:725`), so the client's answer never echoes
`a=rtcp-fb:111 nack`. That is not a refusal. In `SetRemoteContent_w`
(`pc/channel.cc:753`) the send parameters are read off the remote description,
which is this server's offer, and `SetReceiveNackEnabled` follows
`SenderNackEnabled()`. The offer alone turns on both halves; the SFU must never
wait for the line to come back.

### Changes

- SDP: `rtpMap(111)->addFeedback("nack")` on both audio m-lines, in the same
  pure function as step 1.
- SFU outbound: `RtcpNackResponder` in the audio outbound chain
  (`media_router.cpp:536`), as the video has at `:607`. The responder caches
  packets as they left, and since step 1 they leave with their original
  payload type.
- SFU inbound: a NACK generator for the incoming audio. `VideoFeedback`
  already does this, but together with REMB. Split the NACK part into its own
  class, `sfu/loss_repair.hpp` with `observe` and `request`, and have
  `VideoFeedback` use it. The audio installs only that, with the same initial
  parameters: `retry_after 40 ms`, `give_up_after 500 ms`, `max_requests 3`.
- The retransmission arrives with the same SSRC and sequence number. NetEq
  discards the duplicate; `forward_audio` forwards it like any packet.
- Stats: `nack_count` from inbound and `retransmitted_packets_sent` from
  outbound in `AudioStats`; `audio_nacks_sent` and `audio_nacks_answered`
  counters in the router.
- Configuration: `[audio] retransmission = true`, the key to go back.

### Tests

- Unit: `test_video_feedback.cpp` now covers the extracted class; new cases
  for the audio pace, 50 packets per second.
- Integration, in the 5% test: `retransmitted_packets_sent > 0` on Ana's side,
  `audio_nacks_sent > 0` and `answered > 0` on the SFU. Concealment no worse
  than in step 1.
- Manual: burst loss, if the impairment layer has the mode. Otherwise 10%.

### Acceptance

The three counters above leave zero in the test, and concealment is equal to
or lower than in step 1.

### Risk and way back

A NACK storm under heavy loss, bounded by `max_requests`. A late
retransmission is discarded; NetEq widens its buffer by itself under loss.
`retransmission = false` on the server.

## 5. Step 3: listener loss in the sender's receiver report

A decision, not code. Medium if it is ever done.

The idea was for the SFU to rewrite the fraction lost of the receiver report it
sends to the sender with the maximum of the loss it sees itself and the loss
the listeners report, so that Opus in-band FEC reacts to the right leg.

**After steps 1 and 2 it loses its object.** `useinbandfec` left the profile in
step 1, and without it there is no in-band FEC to react to anything. The rule:
implement only if step 1 is reverted and FEC comes back. Otherwise record the
decision in chapter 9 and move on to step 4.

**Decided on 3 September 2026: not done.** Steps 1 and 2 landed with
`useinbandfec` out of the profile, and chapter 9 section 8 records it.

If it is ever done:

- A handler in the outbound chain, after `RtcpSrReporter`, that reads the
  report blocks of each listener's receiver report and keeps the worst
  fraction lost per source.
- A handler in the inbound chain that intercepts the receiver report generated
  by `RtcpReceivingSession` and replaces the field with the maximum.
- Acceptance: in the 5% test with loss only on the listener's side, the sender
  reports a non-zero loss.

## 6. Step 4: AGC2 in place of AGC1

Client. Small, one day. Key `automatic_gain_control`.

An input volume controller plus an adaptive digital gain driven by a voice
detector. Less pumping, and up to 50 dB of gain for whoever speaks far from the
microphone.

**Verified.** The voice engine's `ApplyOptions`, on seeing `auto_gain_control`
in the AudioOptions, turns AGC1 back on in analog mode
(`webrtc_voice_engine.cc:734`). The option has to stay unset, as is already
done with `echo_cancellation`, and the AGC is then moved only through
`ApplyConfig`.

### Changes

- `libwebrtc_media_session.cpp:413`: `gain_controller1.enabled = false`;
  `gain_controller2.enabled = true`, `input_volume_controller.enabled = true`,
  `adaptive_digital.enabled = true`. m152 defaults: 5 dB headroom, 50 dB
  maximum gain, 15 dB initial, 6 dB/s, noise floor at −50 dBFS. Do not touch
  the numbers before listening.
- `set_audio_processing`, line 111: the toggle now moves
  `gain_controller2.enabled` and `input_volume_controller.enabled`. The AGC1
  fields go.
- `audio_options()`, line 178: `auto_gain_control` is no longer set.
- Docs: chapter 3, the `automatic_gain_control` row; the `config.ini` comment.

### Tests

- Integration: `TheAudioPipelineWorksOnAVirtualDevice` and
  `AudioLevelsAreReported` stay green. New test: with the AGC turned off
  through `set_audio_processing`, the module's `GetConfig()` shows
  `gain_controller2.enabled == false`, exposed through a test accessor the way
  the echo return loss exposes the AEC.
- Manual: speak close to and far from the microphone, compare with the
  previous version; background music to listen for pumping.

### Acceptance

The voice level stays steady between close and far with no audible pumping,
and the AEC's echo return loss is the same as before: AGC2 may not make the
canceller worse.

### Risk and way back

The input volume controller moves the Windows microphone volume, as AGC1 did.
If somebody complains about the volume moving by itself,
`input_volume_controller.enabled = false` while keeping the digital part.
`automatic_gain_control = false` turns both off.

## 7. Step 5: the noise suppression level

Client. Small, one day. Key `noise_suppression_level`.

Choosing how hard the suppressor bites. Today it is fixed at `kHigh`, and the
only alternative is turning it off.

**Verified.** `ApplyOptions` forces `kHigh` whenever `noise_suppression` is
present in the AudioOptions (`webrtc_voice_engine.cc:753`). Same recipe as
step 4: the option stays unset and the level goes through `ApplyConfig`.

### Changes

- Configuration: `[audio] noise_suppression_level = low | moderate | high |
  very_high`. Proposed default `moderate`. That is a decision, not a fact: the
  step's manual test decides. If `high` wins, it stays `high` with the key
  exposed.
- `MediaSession::set_audio_processing` gains the level;
  `Engine::set_audio_processing` applies `config.noise_suppression.level`.
- `audio_options()`: `noise_suppression` is no longer set.
- Settings: the "Remove background noise" checkbox becomes a selector, "Noise
  suppression: Off / Low / Moderate / High / Very high". It writes
  `noise_suppression` and `noise_suppression_level`. "Off" keeps
  `noise_suppression = false`, so an old file stays valid.
- Docs: chapter 3, `config.ini`.

### Tests

- `test_config.cpp`: the four values parse, an unknown value fails with the
  line number.
- Manual: the same sentence with a fan running, at the four levels, heard on
  the other client. An instrument at `low`.

### Acceptance

The level changes mid-call, and the default is decided and written into
chapter 3 with the reason.

### Risk and way back

Low. `noise_suppression_level = high`.

## 8. Step 6: a warning for a microphone below 32 kHz

Client. Small, half a day. No key: it is only a warning.

Whoever is at 16 kHz gets to know, and knows what to do.

**Fact.** The legacy ADM opens the device in the format Windows mixes it at. A
headset in communications mode or on Bluetooth HFP delivers 16 kHz, and nothing
above 8 kHz reaches Opus. The client resamples in silence in
`screen_audio_frame_processor.hpp`. There is no opening it at 48 kHz from
code: in shared mode the format is Windows' own.

### Changes

- `ScreenAudioFrameProcessor::Process` already sees `sample_rate_hz()` and
  `num_channels()`: publish them into a `std::atomic<int>` on the mixer and
  expose `AudioStats::microphone_sample_rate_hz`.
- Log at INFO once, and again if it changes: `Audio: the microphone is
  delivering 16000 Hz; nothing above 8 kHz will be sent`.
- Settings: a hint line under "Microphone" when the rate is below 32000: "This
  microphone runs at 16 kHz. Voice above 8 kHz is lost. Change its default
  format in Windows sound settings (24 bit, 48000 Hz)". Only during a call,
  because that is when the capture exists.
- Bluetooth HFP has no fix by configuration. When the device name contains
  "Hands-Free", the hint says so instead of sending people into Windows.

### Tests

- Unit: the pure function that produces the hint,
  `microphone_rate_hint(rate, name)`.
- Integration: `MetricsAreCollectedFromTheRealConnection` asserts
  `microphone_sample_rate_hz > 0`.

### Acceptance

With a headset at 16 kHz the hint appears; at 48 kHz nothing appears.

## 9. Step 7: a 128 kbps ceiling

Server configuration. Trivial, one hour. Key `audio.bitrate_kbps`.

Stereo music from the share close to transparent. At 96 kbps stereo Opus is
good; near 128 it is nearly transparent.

### Changes

- `shared/include/dv/config/config.hpp:56`: 96 becomes 128.
- Chapter 3, the `[audio]` table; chapter 9 section 5, where the paragraph
  telling of the move from 48 to 96 gains the move from 96 to 128; chapter 12
  lines 57 and 75. With RED the worst case becomes 256 kbps per leg.

### Tests

- None new. `flow.sent > 40` in
  `TheSoundOfASharedScreenReachesTheOtherParticipant` does not change.
- Optional: record in the chapter 9 section 6 table the bitrate measured with
  the tone at 128.

### Acceptance

The end-to-end suite is green and a known track has been listened to at both
ceilings.

### Risk and way back

32 kbps more per leg; 64 with RED. `audio.bitrate_kbps = 96` on the server.

## 10. Step 8: a limiter in the mixer

Client. Small, one day. Key `screen_audio.limiter`.

Voice and screen together stop crackling. Today the sum saturates in
`saturate`, at `screen_audio_mixer.cpp:198`.

### Changes

- In `ScreenAudioMixer::mix`: sum in int32 and a peak limiter with a Q15 gain.
  Instant attack: if |sum·g| exceeds 32767, g becomes 32767/|sum|. Release
  back to 1.0 in about 300 ms, one constant per block. `saturate` stays as the
  last resort.
- No lookahead. The cost is a rare click instead of a constant crackle, and
  the latency does not change.
- The microphone-only path, line 172, does not go through the limiter: g stays
  at 1 and the audio is identical to today's.
- A `blocks_limited` counter in the mixer stats, exposed as
  `screen_audio_limited_blocks`.
- Chapter 9 section 4: "A boost clips in saturate" now describes the limiter
  and keeps the point: 100 is still the untouched sound when there is no voice.

### Tests

- `test_screen_audio_mixer.cpp`: full scale microphone plus full scale tone,
  zero samples at ±32767 after the first block. Silence: identity. Microphone
  only: identity bit for bit. Release: one loud block followed by quiet blocks
  returns to g = 1 within 30 blocks.
- Manual: a loud voice over loud music, no crackle.

### Acceptance

The four unit tests pass and the manual test does not crackle.

### Risk and way back

Pumping if the release is too short. Measure by ear and adjust the constant.
`[screen_audio] limiter = false`, a client key.

## 11. Step 9: concealment metrics

Client. Small, one day. No key.

An audio complaint becomes a number: network, buffer or capture. The basic
counters went in at step 1; here the rest and the screen go in.

### Changes

- `AudioStats`: `silent_concealed_samples`, `jitter_buffer_delay` with
  `jitter_buffer_emitted_count` (average delay in ms), `packets_discarded`,
  `fec_packets_received`, plus the ones from step 2. All exist in m152's
  `RTCInboundRtpStreamStats` (`rtcstats_objects.h:239` to `:261`).
- `MetricsHistory::observe`: derive per interval `concealment_percent` =
  Δconcealed / Δtotal_samples, and `jitter_buffer_ms`.
- `MetricsDialog`: a fourth chart, "Concealment (%)", with thresholds, fair at
  1% and poor at 3% to begin with, calibrated on the 5% test; and the buffer
  delay line.
- `network_quality.hpp`: decide whether `quality_of` starts looking at
  concealment. Proposal: not in this pull request; only show it.
- The 5 s log line and the status bar gain `conceal N%`.

### Tests

- Unit in `MetricsHistory` for the derivation.
- Integration: `MetricsAreCollectedFromTheRealConnection` asserts
  `total_samples_received > 0` and zero concealment on a clean network. In the
  5% test, concealment above zero and below the threshold.

### Acceptance

The chart shows, in the 5% test, the difference between RED on and off.

## 12. Step 10: audio in congestion control

A spike first. Two days, with a stop.

The audio bitrate reacting to the link, as the video does. Today it is fixed at
the ceiling, outside the allocation. On a LAN nothing changes; it only matters
on weak links.

**Verified.** `AudioSendStream` only joins the allocation with `transport-cc`
negotiated and feedback arriving (`audio_send_stream.cc:367`). libdatachannel
generates no transport-wide feedback; the SFU only speaks REMB. So
`transport-cc` on the audio m-line alone does nothing.

### The spike

- Turn on the audio network adaptor, `AudioOptions::audio_network_adaptor`
  with its config, fed only by loss and round trip time from the receiver
  reports, and measure whether the bitrate and the frame length move in the
  5% test.
- Evaluate the SFU emitting REMB for the audio too, and
  `WebRTC-Audio-ABWENoTWCC` on the client, which allows allocation without
  transport-wide feedback.
- Stop criterion: if neither moves the bitrate in the test, close the step
  with a record in chapter 11 and do not implement.

### Acceptance of the spike

One number: the audio bitrate before and after, under loss, recorded in
chapter 11.

## 13. Step 11: a second track for the screen sound

Client and server. Large, two to three weeks. Option B of chapter 9.

Per-source volume at the receiver, independent mute, music with no compromise
towards the voice.

**Why it is large.** A `PeerConnectionFactory` has one ADM, and every
`AudioSendStream` receives the same captured frame. A second track needs a
second factory with its own ADM fed by the loopback, and a second
PeerConnection. Two transports share no RTP synchronisation group, so lip sync
stops being guaranteed.

### Phases

- A decision document: what is gained, per-source volume, against what is
  lost, lip sync.
- A spike in `tools/screen_audio_spike`: a custom ADM fed by the
  `LoopbackCapturer`, a second PeerConnection, and the measured drift between
  sound and picture over 10 minutes.
- Server: a second audio m-line for the sharer with its own msid, forwarded to
  the listeners as a separate track.
- Client: the receiver gains a "Shared sound" volume separate from the
  participant's volume.

### Stop criterion

Stop if the drift measured in the spike exceeds 80 ms with no viable
correction. In that case option A stays, and the result goes into chapter 9
section 8.

## 14. Step 12: the new Windows ADM

Toolchain and client. Large, one week, rebuild included. Way back
`DV_AUDIO_LEGACY_ADM=1`.

`CreateWindowsCoreAudioAudioDeviceModule` in place of the legacy ADM:
automatic switching when the default device changes, restart when a device is
removed, and no Voice Capture DSP, the one that muted whoever created the room.

**Fact.** The header exists in the dist, the symbol does not. The likely cause
is the target missing from the list `scripts/build_webrtc.sh:381` bundles, not
that it does not compile.

### Steps

- `gn ls` on the out directory to find the target,
  `modules/audio_device:audio_device_impl` and the Core Audio utility; add it
  to the list; rebuild per chapter 7; `dumpbin /symbols` to confirm.
- In the Engine, create the new ADM when `DV_AUDIO_LEGACY_ADM` is not set.
- `devices()` and `set_device`: the new ADM enumerates "Default" and
  "Communications" at indices 0 and 1. Adjust and re-run
  `SwitchingMicrophoneDoesNotInterruptTheCallForLong`.
- `decline_platform_echo_canceller` becomes a no-op on it.

### Acceptance

Unplugging the headset mid-call brings the audio back by itself on the next
device, and the end-to-end suite is green.

### Risk and way back

Device enumeration with different semantics; the microphone switching test is
what catches it. `DV_AUDIO_LEGACY_ADM=1` in the environment.
