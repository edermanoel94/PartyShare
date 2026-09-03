# 11. Benchmarks

The targets from section 22 of the SPEC, measured.

Every number here came out of a run recorded below, with the machine and the
method alongside it. Nothing was estimated, and anything not yet measured is
marked as such rather than filled in with a guess.

## Reference machine

| | |
| --- | --- |
| CPU | AMD Ryzen 7 7435HS, 16 threads |
| Memory | 15 GiB |
| System | Arch Linux, kernel 7.1.8 |
| GPU | NVIDIA GeForce RTX 4050 Laptop, driver 610.57.04 |
| Qt | 6.11.1 |
| libwebrtc | m152.7977.0.0, built from source against the system libstdc++ |
| Network | loopback |

## Reproducing

```sh
cmake --build build/media --target dv_benchmarks
ctest --test-dir build/media -L benchmark --output-on-failure   # DV_BENCHMARK_SECONDS
ctest --test-dir build/media -L media --output-on-failure       # impairment cases
```

The cases are in `tests/integration/test_benchmark.cpp` and
`tests/integration/test_network_impairment.cpp`.

## Five participants, one sharing a screen

A room of five, everyone with an open microphone, one sharing a monitor at
1280x720 and 30 FPS. Measured after all four viewers were already receiving
frames, so what is measured is the call in steady state.

**30 seconds**

```text
CPU, five clients          86.0% of one core
CPU, per client            17.2% of one core
memory, five clients       175 MiB
memory, per client          35 MiB

participant    rtt      jitter   lost   fps      resolution
ana (sending)    1 ms   3.0 ms      0      -     -
bruno            1 ms   3.0 ms      0   29.7     1280x720
carla            1 ms   3.0 ms      0   29.7     1280x720
diego            1 ms   2.0 ms      0   29.7     1280x720
elena            1 ms   3.0 ms      0   29.7     1280x720

SFU: 30,361 audio packets forwarded, 3,912 video
```

**10 minutes**, which is what the M6 criterion asks for
(`DV_BENCHMARK_SECONDS=600`):

```text
CPU, five clients          78.2% of one core
CPU, per client            15.6% of one core
memory, five clients       190 MiB
memory, per client          38 MiB

all four viewers           30.0 fps, 1280x720, 0 lost
SFU: 600,332 audio packets forwarded, 74,204 video
```

Ten minutes at 30.0 FPS with zero lost packets, and CPU slightly below the thirty
second run — bringing the call up weighs more in a short window.

Two honest caveats about that:

- **30.0 FPS is the average over the whole window.** It does not prove there was
  no half second dip in the middle. A per-second measurement with a standard
  deviation would answer that, and it does not exist yet.
- **Memory went from 175 to 190 MiB between the two runs.** 15 MiB over ten
  minutes, with five clients in one process. It may be the steady state of
  buffers and it may be a slow leak. It was not investigated, and it is recorded
  here so that it can be.

### What these numbers do not say

The five clients run **in the same process and on the same machine**, over
loopback. That changes three things, and they belong here rather than in a
footnote somebody quotes without:

- **The 1 ms rtt is not a network measurement.** It is the cost of the local
  loop. The under-150 ms target is still unmeasured on a real network: what the
  impairment section measures is injected latency, which answers "the call
  survives half a second round trip", not "the call stays under 150 ms".
- **The per-client cost is optimistic.** The media layer has one `Engine` per
  process — one `AudioDeviceModule`, one audio processing module, three libwebrtc
  threads — shared by all five sessions. Five separate processes would cost more.
- **Screen capture happens once.** Only one participant shares, so capture and
  encoding appear once and not five times.

What the numbers do say with confidence is the shape of the system: five
participants, four receiving 1280x720 at practically 30 FPS, no loss, and the SFU
forwarding thirty thousand audio packets in thirty seconds.

## Hardware encoder

The same room, changing only who encodes. `DV_DISABLE_HARDWARE_ENCODER=1` forces
software.

The screen being measured is **static**, which is the case that separates the
two: a screen share is a still image most of the time, and what decides the cost
is not encoding speed but how many bits the encoder spends on a frame that did
not change.

```text
                          bitrate    CPU, five clients    video packets at the SFU
NVENC, CBR                1795 kbps        77.1%                    32,340
NVENC, VBR                1371 kbps          -                          -
NVENC, VBR + minQP 24        2 kbps        69.5%                     4,100
OpenH264                     9 kbps        78.9%                     3,936
```

The first two rows are what validation found, not what is in the code today.
Under CBR the card filled the target regardless of content, and what it saved on
encoding it spent again packetising and sending eight times more RTP, which wiped
out the whole saving. VBR alone was not enough either: with no quantizer floor the
controller keeps lowering the quantizer until it spends the target, encoding an
image that does not move ever closer to lossless. The third row is the current
state. Entries 9 and 10 of [chapter 15](15-postmortems.md) are the full account.

Two caveats:

- **The measurement is of a static screen** — the worst case for CBR and the best
  one for the QP floor. A moving screen was not measured, and a QP floor of 24
  caps maximum quality, so the gain under real motion is a prediction.
- **The CPU figures scatter.** Three NVENC runs gave 67.4%, 70.4% and 76.2%;
  three software runs gave 78.9%, 82.6% and 83.7%. The ranges do not overlap, so
  the roughly ten point difference is real — quoting a single pair as though they
  were exact would not be.

## Impaired network

Section 22 asks that the call survive 5% packet loss with graceful degradation
and no drop. That is measured two ways, because neither one alone is honest.

`scripts/netem.sh` applies `tc netem` to an interface. It is the faithful one: it
degrades the operating system's own queues, for every process and in both
directions. It needs root and only exists on Linux.

The injector in `client/src/media/network_impairment.hpp` damages packets inside
the client's own UDP sockets, below DTLS and above the operating system. It needs
no privileges, runs on all three platforms, and degrades exactly one
participant's link to the SFU. Everything below it is real — real Opus, real
SRTP, real jitter buffer, real RTCP. The only thing simulated is the wire.

**5% loss under `tc netem`**, three runs of each encoder, room in steady state:

```text
                  CPU, five clients           viewer fps             lost per participant
NVENC             67.4%  70.4%  76.2%         27.0  30.0  30.0              ~600
OpenH264          78.9%  82.6%  83.7%         29.6  29.8  29.8              ~590
```

Nobody drops, and the bandwidth estimate falls from 3000 kbps on a clean link to
between 1840 and 2039 kbps under loss — the SFU estimator reacting to real
operating system loss and not only to loss injected in the client, which is why
this measurement exists separately. The 27.0 in the first NVENC run did not
repeat and was not investigated beyond that; one run below its neighbours is
noise until it shows up again.

**5% loss injected in the client**, in each direction, after the call reached
steady state:

```text
CPU, five clients          80.8% of one core
memory, five clients       230 MiB

all four viewers           29.7 fps, 1280x720, ~550 lost each

injected: 435 of 9,254 packets dropped outbound, 1,548 of 33,605 inbound
repair:   37 requests, 37 video packets missing, 37 recovered
```

The four viewers keep 29.7 FPS, the same as the run without loss. Audio loses the
packets that were dropped and Opus fills the gaps. Nobody drops. Against the clean
run: CPU practically identical, memory from 175 to 230 MiB — the cost of holding
packets for retransmission and of larger jitter buffers, not investigated beyond
that.

**Audio in isolation**, two participants, 15 seconds, 5% each direction:

```text
injected     95 of 1,619 packets outbound, 58 of 1,520 inbound
receiver     677 packets arrived, 60 counted as lost
```

The loss the receiver accounts for is on the order of what was injected and not a
multiple of it. That is what "graceful degradation" means here: Opus carries one
frame per packet, so 5% of packets lost is 5% of the audio.

**Latency and jitter**, 250 ms each way with 30 ms of jitter:

```text
packets held        3,216
receiver            715 packets arrived, rtt 492 ms, jitter 19.0 ms
```

The 492 ms libwebrtc measures against the 500 ms injected is the cross-check that
the injector does what it says. The call stays up, with the uncomfortable
conversation half a second of round trip produces.

### The screen share freeze, found here

Before any fix, the same screen over a link with 5% loss delivered **4 frames in
15 seconds**. That is not degradation, it is a freeze.

An intra frame of a 1280x720 screen is over a hundred packets, and at 5% loss the
chance of all hundred arriving intact is under 1%. The only repair a viewer had
was to ask for a new intra frame, which arrived broken, and the request started
over. The SFU carried eight keyframe requests in fifteen seconds and not one
produced a picture.

The fix is both ends of retransmission, in the SFU: `rtc::RtcpNackResponder` on
the outbound track, and `dv::server::sfu::VideoFeedback` on the inbound one, which
makes the same request upstream rather than letting one hole propagate to every
viewer. libdatachannel answers NACKs and never sends one, so that half did not
exist.

After it: **443 frames in 15 seconds**, 22 packets missing, 22 recovered, **zero**
keyframe requests.

The caveat: the screen measured is an idle work session, where delta frames fit in
one packet each. A screen with moving video has frames spanning several packets,
and one lost packet costs the whole frame until the retransmission arrives. The
repair still holds, and the frame rate under loss would be lower — not measured.

### Bitrate under congestion

How much a share may send is decided by the SFU and obeyed by the client. One
shared screen, with 20% loss switched on and then off:

```text
clean link        2,568 kbps
with 20% loss     1,613 kbps, and the SFU asking for 1,613
afterwards        2,193 kbps
```

The two middle numbers are the two ends of the same loop: the SFU decides, REMB
carries it, libwebrtc's congestion controller obeys. If they disagreed, the loop
would be broken somewhere between one and the other.

The drop takes seconds and the recovery tens of them, which is deliberate:
backing off has to be faster than probing, or congestion lasts longer than it
needs to.

Before this the number did not move at all. Without `transport-cc` and without
REMB the sender's estimate climbed to the ceiling and stayed there even with a
fifth of the packets being dropped — libwebrtc has no way of learning about a loss
nobody tells it about.

### The audio does not follow, yet

Step 10 of [chapter 16](16-audio-plan.md) asked whether the audio could be made
to do the same. The experiment is in the tree behind two switches, both off:
`[audio] adaptive = true` on the server puts a REMB on every incoming audio
track, and `DV_AUDIO_ADAPTIVE=1` in a client's environment turns on the two
libwebrtc trials that let the audio send stream join the bitrate allocation
without transport-wide feedback and asks the sender for `adaptive_ptime`, which
is the one thing that opens the stream's minimum below its target. Measured on
3 September 2026, two clients, 20% loss switched on for fifteen seconds:

```text
clean link        128 kbps
with 20% loss     128 kbps, the SFU asking for 32 in 30 reports
```

Every piece is wired — the SFU asks, the client accepts the trials and the
parameter — and the encoder's target does not move. Where the loop breaks is
inside libwebrtc's congestion controller, which in this configuration treats a
REMB as a cap on an estimate that nothing else is producing, and the audio
allocation never sees a number below its ceiling. Closing that gap means either
transport-wide feedback from the SFU, which libdatachannel does not generate, or
a deeper change to how the client's estimate is driven. Neither is worth it for
a product that runs on a LAN, so the switches stay off and the record stays
here. `ImpairedNetworkAdaptiveAudioTest` is this table made executable: the day
its last assertion fails is the day the audio started following.

### One thing that could not be enabled

The other direction of the loop is missing: the viewer telling the SFU how much
its own link can take. The code is there and a test exercises it, and this
client never sends that report, because producing it requires the `abs-send-time`
RTP header extension. Negotiating that makes libwebrtc probe bandwidth, probing
means sending padded packets, and libdatachannel has an `assert` that takes the
whole process down while building the sender report for one. Entry 7 of
[chapter 15](15-postmortems.md).

## Client startup

Measured from the start of `main` until the window is on screen, which is as close
as it gets to what a person waits for. The client logs it every run:

```text
PartyShare client ready in 18 ms
PartyShare client ready in 25 ms
PartyShare client ready in 26 ms
```

## State of the section 22 targets

| Metric | Target | Measured |
| --- | --- | --- |
| Screen share | 1280x720 | 1280x720 |
| FPS | 30 | 29.7 over 30 s, 30.0 over 10 min |
| Participants | 5 | 5 |
| Audio | 48 kHz Opus | 48 kHz Opus |
| Video | H.264 | H.264, NVENC on the card with OpenH264 as fallback |
| Latency | < 150 ms | 1 ms on loopback; survives 492 ms injected; **no measurement on a real network** |
| Packet loss | survive 5% | 29.7 FPS with 5% injected both ways, 30.0 with 5% under `tc netem` |
| CPU | low usage | 13.9% to 17.2% of one core per client, with the caveat above |
| Memory | < 500 MB per client | 35 to 38 MiB per client, with the caveats above |
| Startup | < 3 s | 18 to 26 ms |
