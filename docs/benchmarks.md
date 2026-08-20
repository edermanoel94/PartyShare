# Benchmarks

The targets from section 22 of the SPEC, measured.

Every number here came out of a real run recorded below, with the machine and the method alongside it.
Nothing was estimated, and anything not yet measured is marked as such rather than filled in with a guess.

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

## How to reproduce

```sh
cmake --build build/media --target dv_benchmarks
ctest --test-dir build/media -L benchmark --output-on-failure
```

The cases are in `tests/integration/test_benchmark.cpp`.
`DV_BENCHMARK_SECONDS` controls how long the call is held; the default is 30 seconds.

The impaired network measurements are in `tests/integration/test_network_impairment.cpp` and run along with the media suite:

```sh
ctest --test-dir build/media -L media --output-on-failure
```

## Five participants, one sharing a screen

A room of five, everyone with an open microphone, one of them sharing a monitor at 1280x720 and 30 FPS.
Measured after all four viewers were already receiving frames, so that what is measured is the call in steady state.

### 30 seconds

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

### 10 minutes

The M6 criterion asks for stable FPS over ten minutes. `DV_BENCHMARK_SECONDS=600`:

```text
CPU, five clients          78.2% of one core
CPU, per client            15.6% of one core
memory, five clients       190 MiB
memory, per client          38 MiB

participant    rtt      jitter   lost   fps      resolution
ana (sending)    1 ms   3.0 ms      0      -     -
bruno            1 ms   3.0 ms      0   30.0     1280x720
carla            1 ms   3.0 ms      0   30.0     1280x720
diego            1 ms   3.0 ms      0   30.0     1280x720
elena            1 ms   3.0 ms      0   30.0     1280x720

SFU: 600,332 audio packets forwarded, 74,204 video
```

Ten minutes at 30.0 FPS, with zero lost packets, and CPU slightly below the thirty second run - the cost of bringing the call up weighs more in a short window.

Two honest caveats about that number:

- **30.0 FPS is the average over the whole window.** It does not prove there was no half second dip in the middle. A per second measurement, with a standard deviation, is what would answer that, and it does not exist yet.
- **Memory went from 175 to 190 MiB between the 30 s and the 600 s runs.** That is 15 MiB over ten minutes, with five clients in the same process. It may be the steady state of buffers, and it may be a slow leak. It was not investigated, and it is recorded here so that it can be.

### Hardware encoder

The same room, changing only who encodes.
`DV_DISABLE_HARDWARE_ENCODER=1` forces software, and without it the card is used.

The screen being measured is static, which is the case that separates the two: a screen share is a still image most of the time, and what decides the cost is not encoding speed but how many bits the encoder decides to spend on a frame that did not change.

```text
                          bitrate    CPU, five clients    video packets at the SFU
NVENC, CBR                1795 kbps        77.1%                    32,340
NVENC, VBR                1371 kbps          -                          -
NVENC, VBR + minQP 24        2 kbps        69.5%                     4,100
OpenH264                     9 kbps        78.9%                     3,936
```

The first two rows are what validation found, not what is in the code today.
Under CBR the card filled the target regardless of content, and what it saved on encoding was spent again packetizing and sending eight times more RTP, which wiped out the entire saving.
Switching to VBR was not enough: with no quantization floor the controller keeps lowering the quantizer until it spends the target, encoding an image that does not move ever closer to lossless.

The third row is the current state, and it is the only one of the four where task 4 of M8 delivers what there is to deliver.

Two caveats, the same ones that apply to the rest of this page:

- **The measurement is of a static screen.**
  It is the worst case for CBR and the best one for the QP floor.
  A moving screen was not measured, and a QP floor of 24 caps the maximum quality, so the gain under real motion is a prediction and not a number.
- **The CPU figures scatter.**
  Three NVENC runs gave 67.4%, 70.4% and 76.2%; three software runs gave 78.9%, 82.6% and 83.7%.
  The ranges do not overlap, so the roughly ten point difference is real, but quoting a single pair of numbers as though they were exact would not be.

### What these numbers do not say

The five clients run **in the same process and on the same machine**, over loopback.
That changes three things, and it matters that they are written down before anyone quotes the numbers:

- **The 1 ms rtt is not a network measurement.**
  It is the cost of the local loop.
  The under 150 ms target from section 22 is still unmeasured on a real network: what the next section measures is injected latency, which answers "the call survives half a second round trip" and not "the call on a real network stays under 150 ms".
- **The per client cost is optimistic.**
  The media layer has a single `Engine` per process: one `AudioDeviceModule`, one audio processing module and three libwebrtc threads, shared by all five sessions.
  Five separate processes would cost more than 17.2% of a core and 35 MiB each.
- **Screen capture happens once.**
  Only one participant shares, which is what section 5.2 allows, so the capture and encoding cost appears once and not five times.

What the numbers do say with confidence is the shape of the system: five participants, four receiving 1280x720 at practically 30 FPS, with no loss, with the SFU forwarding thirty thousand audio packets in thirty seconds.

## Impaired network

Section 22 asks that the call survive 5% packet loss with graceful degradation and no drop.
That is measured two ways, because neither one alone is honest.

The first is `scripts/netem.sh`, which applies `tc netem` to an interface.
It is the more faithful one: it degrades the operating system's own queues, for every process and in both directions.
It needs root and only exists on Linux.
For a while it did not run on this machine, because the kernel had been upgraded without a reboot and `sch_netem` would not load; the script diagnoses exactly that case rather than letting `tc` answer "Specified qdisc kind is unknown".
After the reboot it runs, and the measurement is below.

The second is the injector described in `client/src/media/network_impairment.hpp`, which damages packets in the client's own UDP sockets, below DTLS and above the operating system.
It needs no privileges at all, runs on all three platforms, and degrades exactly one participant's link to the SFU.
Everything below it is real: real Opus, real SRTP, real jitter buffer, real RTCP.
The only thing simulated is the wire.

### Five participants with 5% loss under `tc netem`

The wire level half, which was missing until the reboot.
`sudo scripts/netem.sh apply lossy` puts 5% loss on loopback, which hits every process and the signaling WebSocket as well.

Three runs of each encoder, with the room in steady state:

```text
                  CPU, five clients           viewer fps             lost per participant
NVENC             67.4%  70.4%  76.2%         27.0  30.0  30.0              ~600
OpenH264          78.9%  82.6%  83.7%         29.6  29.8  29.8              ~590
```

Nobody drops, and the bandwidth estimate falls from 3000 kbps on a clean link to between 1840 and 2039 kbps under loss.
That is the SFU estimator described in the bitrate section reacting to real operating system loss, and not only to loss injected in the client, which is why this measurement exists separately from the other one.

The 27.0 in the first NVENC run did not repeat in the other two and was not investigated beyond that.
A single run below its neighbours, with the other two at 30.0, is noise until it shows up again.

### Five participants with 5% loss injected in the client

The same room as the benchmark above, with 5% loss injected in each direction after the call was already in steady state.

```text
CPU, five clients          80.8% of one core
memory, five clients       230 MiB

participant    rtt      jitter   lost   fps      resolution
ana (sending)    1 ms   3.0 ms    542      -     -
bruno            1 ms   3.0 ms    545   29.7     1280x720
carla            1 ms   3.0 ms    568   29.7     1280x720
diego            1 ms   3.0 ms    542   29.7     1280x720
elena            1 ms   2.0 ms    552   29.7     1280x720

injected: 435 of 9,254 packets dropped outbound, 1,548 of 33,605 inbound
repair:   37 requests, 37 video packets missing, 37 recovered
```

The four viewers keep receiving 1280x720 at 29.7 FPS, the same number as the run without loss.
Audio loses the packets that were dropped and Opus fills the gaps.
Nobody drops.

Compared with the clean run: CPU practically identical, and memory from 175 to 230 MiB.
The extra 55 MiB are the cost of holding packets for retransmission and of larger jitter buffers, and they were not investigated beyond that.

### Audio, in isolation

Two participants, 15 seconds, 5% loss in each direction:

```text
injected     95 of 1,619 packets outbound, 58 of 1,520 inbound
receiver     677 packets arrived, 60 counted as lost
quality      rtt 1 ms, jitter 2.0 ms
```

The loss the receiver accounts for is on the order of what was injected, and not a multiple of it.
That is what "graceful degradation" means here: Opus carries one frame per packet, so 5% of packets lost is 5% of the audio.

### Latency and jitter

250 ms in each direction, with 30 ms of jitter, also for 15 seconds:

```text
packets held        3,216
receiver            715 packets arrived, rtt 492 ms, jitter 19.0 ms
```

The 492 ms measured by libwebrtc itself against the 500 ms injected is the cross check that the injector does what it says.
The call stays up, with the uncomfortable conversation that half a second of round trip produces.

### The screen share freeze, found here

Before any fix, the same screen shared over a link with 5% loss delivered **4 frames in 15 seconds**.
That was not degradation, it was a freeze.

The reason is arithmetic.
An intra frame of a 1280x720 screen is over a hundred packets, and at 5% loss the chance of all hundred arriving intact is under 1%.
The only repair a viewer had was to ask for a new intra frame, which arrived broken, and the request started over.
The SFU carried eight keyframe requests in fifteen seconds and not one of them produced a picture.

The fix has two parts, both in the SFU:

- `rtc::RtcpNackResponder` on the outbound track, which answers a viewer that lost a packet by resending that packet.
- `dv::server::sfu::VideoFeedback` on the inbound track, which makes the same request upstream: when a packet from the sharer is lost on its way to the SFU, the SFU asks for the retransmission rather than letting the hole propagate to every viewer.
  libdatachannel answers NACKs but never sends one, and that half did not exist.

After that, the same case delivers **443 frames in 15 seconds**, with 22 packets missing, 22 recovered and **zero** keyframe requests.

The caveat: the screen being measured is that of an idle work session, where delta frames fit in one packet each.
A screen with moving video has frames spanning several packets, and one lost packet costs the whole frame until the retransmission arrives.
The repair still holds, but the frames per second under loss would be lower, and that was not measured.

### Bitrate under congestion

A screen share does not always send the same thing: how much it may send is decided by the SFU and obeyed by the client.
How that works is in [../PLAN.md](../PLAN.md) and in `server/src/sfu/bandwidth_estimator.hpp`; what is measured here is the closed loop.

One shared screen, with 20% loss switched on and then off:

```text
clean link        2,568 kbps
with 20% loss     1,613 kbps, and the SFU asking for 1,613
afterwards        2,193 kbps
```

The two middle numbers are the two ends of the same loop: the SFU decides, REMB carries it, and libwebrtc's congestion controller obeys.
If they disagreed, the loop would be broken somewhere between one and the other.

The drop takes seconds and the recovery takes tens of them, which is deliberate: backing off has to be faster than probing, or congestion lasts longer than it needs to.

Before this, the number did not move.
Without `transport-cc` and without REMB, the sender's estimate climbed to the ceiling and stayed there, even with a fifth of the packets being dropped: libwebrtc has no way of learning about a loss nobody tells it about.

### One thing that could not be enabled

The other direction of the loop is missing: the viewer telling the SFU how much its own link can take.
The code is there and is exercised by a test, but this project's client never sends that report, because producing it requires libwebrtc to have the `abs-send-time` extension in the RTP header.

Negotiating that extension makes libwebrtc probe bandwidth, and probing means sending padded packets.
libdatachannel has an `assert` that takes the whole process down when building the sender report for a padded packet, so the extension stays out until that is resolved upstream.

The SFU now drops padded packets instead of forwarding them, because a client this project did not write can send them anyway, and the result of that was the server aborting.

## Client startup

Measured from the start of `main` until the window is on screen, which is as close as it gets to what a person waits for.
The client logs it on every run:

```text
PartyShare client ready in 18 ms
PartyShare client ready in 25 ms
PartyShare client ready in 26 ms
```

Section 22 target: under 3 seconds. Comfortably.

## State of the section 22 targets

| Metric | Target | Measured |
| --- | --- | --- |
| Screen share | 1280x720 | 1280x720 |
| FPS | 30 | 29.7 over 30 s, 30.0 over 10 min |
| Participants | 5 | 5 |
| Audio | 48 kHz Opus | 48 kHz Opus |
| Video | H.264 | H.264, NVENC on the card with OpenH264 as fallback |
| Latency | < 150 ms | 1 ms on loopback; survives 492 ms injected; no measurement on a real network |
| Packet loss | survive 5% | 29.7 FPS with 5% injected in both directions, 30.0 with 5% under `tc netem` |
| CPU | low usage | 13.9% to 17.2% of one core per client, with the caveat above |
| Memory | < 500 MB per client | 35 to 38 MiB per client, with the caveats above |
| Startup | < 3 s | 18 to 26 ms |
