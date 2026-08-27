# 12. Hardware requirements

What a machine needs to run the client and the server.

Measured numbers come from [chapter 11](11-benchmarks.md), on the reference
machine described there, and every one of them was taken on Linux. The Windows
and macOS figures follow from what libwebrtc and Qt 6 demand on those platforms
rather than from a measurement taken there — which is a different claim from the
platform being untested. Anything not measured is marked as an estimate, and
estimate here means arithmetic from the configured bitrate, not a guess.

Ports, firewalls and bandwidth per room are in
[chapter 4](04-server-and-database.md).

## Client

### Minimum

| | |
| --- | --- |
| CPU | x64 with 2 cores, or Apple Silicon |
| Memory | 2 GB free |
| GPU | Anything able to compose the window. The hardware encoder is optional |
| Audio | One input and one output device, or `DV_AUDIO_NULL_DEVICE=1` to run without a sound card |
| Network | 4 Mbps down, 4 Mbps up |
| Disk | 200 MB for the binary and the libraries |

Two cores is the minimum because capture, encoding, networking and the UI run on
separate threads, as section 16 of the SPEC requires. One core makes the call work
and makes the interface stutter.

### Recommended

| | |
| --- | --- |
| CPU | x64 with 4 cores or more |
| Memory | 4 GB free |
| GPU | NVIDIA with NVENC, to take encoding off the processor |
| Network | 10 Mbps down, 5 Mbps up |

### Measured usage

In a room of five, one sharing at 1280x720 and 30 FPS, encoding in software:

| | Measured | SPEC section 22 target |
| --- | --- | --- |
| CPU per client | 15.6% to 17.2% of one core | low usage |
| Memory per client | 35 to 38 MiB | under 500 MB |
| Startup | 18 to 26 ms | under 3 s |

The caveat that matters: those five clients run inside one test process, without
a Qt interface. The real process carries the UI and the toolkit on top, so treat
35 MiB as the cost of the media layer and not as the size of the application.

### Bandwidth per client

Video is configured at 1500 to 3000 kbps. Audio is offered a ceiling of 96 kbps,
which is what a stereo screen share can reach; a mono voice sits at around half
of it, which is the number in the table.

| Situation | Up | Down |
| --- | --- | --- |
| Only listening and talking | ~48 kbps | ~192 kbps, the other four |
| Sharing a screen | ~3 Mbps | ~192 kbps |
| Watching someone share | ~48 kbps | ~3.2 Mbps |

Congestion control lowers the video on its own when the link cannot keep up, down
to a floor of 300 kbps. A link below that does not drop the call, it makes the
picture bad.

**When the share carries sound**, it travels inside the sharer's own audio track
rather than in one of its own, so it adds no stream to the table above and no work
to the SFU. What it changes is the size of a stream already there: that
participant's audio goes from a mono voice to a stereo mix, with Opus offered a
ceiling of `audio.bitrate_kbps`, 96 kbps by default.

Measured in `MediaEndToEndTest.TheSoundOfASharedScreenReachesTheOtherParticipant`:

| The sharer's audio track | Bitrate |
| --- | --- |
| Voice only | tens of kbps |
| Share on, nothing playing | ~1 kbps |
| Share on, a tone playing | ~100 kbps |

The middle row is the one worth knowing: a share whose application is quiet costs
nothing at all. Opus opens the stereo stream at the ceiling and settles within a
couple of seconds once it sees the content is silence, so **the ceiling is what
the link has to be able to carry, not what it will carry**.

## Hardware encoder

Optional on every machine: without it the screen share is encoded by the
processor, at the CPU cost above.

| | |
| --- | --- |
| Backend | NVENC |
| Card | NVIDIA with NVENC, which today means Kepler or newer |
| Driver | Has to expose NVENC API 13.1 or newer, the header version in `third_party/nvcodec` |
| Libraries | `libnvidia-encode.so.1` and `libcuda.so.1`, both from the driver |

None of it is linked: the libraries are opened at runtime, so the same binary runs
on a machine without an NVIDIA card. When there is no hardware, the reason is
stated once in the log, when the engine is created.

On Windows, Media Foundation covers Intel QuickSync and AMD VCN as well. On Linux
and macOS those machines encode in software.

## Screen capture

Capture comes from libwebrtc itself, so what it demands of the system is what
PartyShare demands.

| System | Backend | Requirement |
| --- | --- | --- |
| Windows | Windows Graphics Capture, DXGI Desktop Duplication as fallback | Windows 10 1903 or newer |
| macOS | ScreenCaptureKit | macOS 13 or newer, and screen recording permission |
| Linux X11 | XComposite and XDamage | An X server with those extensions loaded |
| Linux Wayland | XDG portal over PipeWire | `xdg-desktop-portal` with a backend installed, and user consent every session |

X11 capture has been validated with a graphics server attached. Wayland validation
is pending, per section 7 of [chapter 7](07-webrtc-toolchain.md).

Shared screen audio needs Windows 10 build 20348 or newer, and exists on no other
platform yet. [Chapter 9](09-screen-audio.md).

## Server

Signaling plus an SFU that routes media without transcoding: an arriving packet
is copied to its destinations and leaves. It spends bandwidth and almost no
processor.

### Minimum, for a room of five

| | |
| --- | --- |
| CPU | 1 x64 core |
| Memory | 512 MB |
| Network | 20 Mbps out, 5 Mbps in |
| Disk | 100 MB, plus whatever the logs take |
| System | Linux x64, no graphics server and no sound card |

### MongoDB

One document per account, one per persistent room, one per chat message and one
per administrative action, none of which carries media. A deployment with a
hundred accounts and a year of administrative history is measured in megabytes,
so the database is not what sizes the machine — the outbound media is.

It may live on the same machine or another one. The server holds its lock while
it talks to it, so what matters is latency and not throughput: the default two
second timeout is what stops an unreachable database from holding up every call
on the server.

## Build machine

Building is heavier than running, because of libwebrtc.

| | |
| --- | --- |
| CPU | The more cores the better, the build is parallel |
| Memory | 16 GB. Linking libwebrtc is the part that consumes it |
| Disk | 30 GB for the libwebrtc checkout, plus the project build |
| Time | Tens of minutes for libwebrtc, the first time |

The client without the media layer, the server and the tests build without any of
that. The tooling is in [chapter 2](02-build.md).
