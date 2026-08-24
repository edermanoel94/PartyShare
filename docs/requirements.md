# Hardware requirements

What a machine needs in order to run the PartyShare client and server.

The numbers in the "measured" column come from [benchmarks.md](benchmarks.md), on the reference machine described there.
Anything that was not measured is marked as an estimate, and estimate here means arithmetic from the configured bitrate, not a guess.

## Platform status

Worth reading before the tables.

| Platform | Status |
| --- | --- |
| Linux x64 | Built and run, and where every number on this page was measured |
| Windows x64 | Code and presets exist, never built and never run |
| macOS ARM64 | Code and presets exist, never built and never run |

The Windows and macOS requirements below follow from what libwebrtc and Qt 6 demand on those platforms, and not from a real run.

## Client

### Minimum

| | |
| --- | --- |
| CPU | x64 with 2 cores, or Apple Silicon |
| Memory | 2 GB free |
| GPU | Anything able to compose the window, the hardware encoder is optional |
| Audio | One input and one output device, or `DV_AUDIO_NULL_DEVICE=1` to run without a sound card |
| Network | 4 Mbps down and 4 Mbps up |
| Disk | 200 MB for the binary and the libraries |

Two cores is the minimum because capture, encoding, networking and the UI run on separate threads, as required by section 16 of the SPEC.
One core makes the call work and makes the interface stutter.

### Recommended

| | |
| --- | --- |
| CPU | x64 with 4 cores or more |
| Memory | 4 GB free |
| GPU | NVIDIA with NVENC, to take encoding off the processor |
| Network | 10 Mbps down and 5 Mbps up |

### Measured usage

In a room of five, with one person sharing a screen at 1280x720 and 30 FPS, encoding in software:

| | Measured | Target from section 22 of the SPEC |
| --- | --- | --- |
| CPU per client | 15.6% to 17.2% of one core | low usage |
| Memory per client | 35 to 38 MiB | under 500 MB |
| Startup | 18 to 26 ms | under 3 s |

The important caveat: those five clients run inside the same test process, without a Qt interface.
The real process carries the UI and the toolkit on top of that, so treat the 35 MiB as the cost of the media layer and not as the size of the application.

### Bandwidth per client

Video is configured at 1500 to 3000 kbps and audio at 48 kbps, values from `dv::config` and section 6 of the SPEC.

| Situation | Up | Down |
| --- | --- | --- |
| Only listening and talking | ~48 kbps | ~192 kbps, the other four |
| Sharing a screen | ~3 Mbps | ~192 kbps |
| Watching someone share | ~48 kbps | ~3.2 Mbps |

Congestion control lowers the video on its own when the link cannot keep up, down to a floor of 300 kbps.
A link below that does not drop the call, it makes the picture bad.

## Hardware encoder

Optional on every machine: without it the screen share is encoded by the processor, at the CPU cost in the table above.

| | |
| --- | --- |
| Backend | NVENC |
| Card | NVIDIA with NVENC, which today means Kepler or newer |
| Driver | Has to expose NVENC API 13.1 or newer, the header version in `third_party/nvcodec` |
| Libraries | `libnvidia-encode.so.1` and `libcuda.so.1`, both from the driver |

None of it is linked: the libraries are opened at runtime, so the same binary runs on a machine without an NVIDIA card.
When there is no hardware, the reason is stated once in the log, when the engine is created.

Intel and AMD have no backend for now, and on those machines encoding is always in software.

## Screen capture

Capture comes from libwebrtc itself, and what it demands of the system is what PartyShare demands.

| System | Backend | Requirement |
| --- | --- | --- |
| Windows | Windows Graphics Capture, with DXGI Desktop Duplication as fallback | Windows 10 1903 or newer |
| macOS | ScreenCaptureKit | macOS 13 or newer, and screen recording permission |
| Linux X11 | XComposite and XDamage | An X server with those extensions loaded |
| Linux Wayland | XDG portal over PipeWire | `xdg-desktop-portal` with a backend installed, and user consent every session |

On Linux, X11 capture has been validated with a graphics server attached.
Wayland validation is pending, per M3 in [../PLAN.md](../PLAN.md).

## Server

The server does signaling and routes media as an SFU, without transcoding.
That means it spends bandwidth and almost no processor: an arriving packet is copied to its destinations and leaves.

### Minimum, for a room of five

| | |
| --- | --- |
| CPU | 1 x64 core |
| Memory | 512 MB |
| Network | 20 Mbps out and 5 Mbps in |
| Disk | 100 MB, plus whatever the logs take |
| System | Linux x64, no graphics server and no sound card |

### MongoDB

Optional. Without it the server keeps accounts, rooms and the audit log in memory, and loses all three when it restarts.

It stores one document per account, one per persistent room and one per administrative action, none of which carries media.
A deployment with a hundred accounts and a year of administrative history is measured in megabytes, so the database is not what sizes the machine; the outbound media is.

It may live on the same machine or another one.
The server holds its lock while it talks to it, so what matters is latency and not throughput: the default two second timeout is what stops an unreachable database from holding up every call on the server.

### Bandwidth per room

With five participants and one sharing a screen, and video at the 3000 kbps ceiling:

| Direction | Arithmetic | Total |
| --- | --- | --- |
| In | 1 video at 3 Mbps plus 5 audio streams at 48 kbps | ~3.3 Mbps |
| Out | 4 copies of the video plus 20 copies of audio | ~13 Mbps |

The outbound side grows with the number of viewers, and it is what sizes the machine.
A rule that works for planning: add 3.3 Mbps per screen viewer and 200 kbps per participant for audio.

#### When the share carries sound

The sound of a shared screen travels inside the sharer's own audio track rather
than in one of its own, so it adds no stream to the table above and no work to
the SFU. What it changes is the size of one of the streams already there: that
participant's audio goes from a mono voice to a stereo mix of a voice and
whatever is playing, and Opus is offered a ceiling of `audio.bitrate_kbps`,
96 kbps by default.

Measured end to end on this project, in
`MediaEndToEndTest.TheSoundOfASharedScreenReachesTheOtherParticipant`:

| The sharer's audio track | Bitrate |
| --- | --- |
| Voice only | tens of kbps |
| Share on, nothing playing | ~1 kbps |
| Share on, a tone playing | ~100 kbps |

The middle row is the one worth knowing: a share whose application is quiet
costs nothing at all. Opus opens the stereo stream at the ceiling and settles
within a couple of seconds once it sees that the content is silence, so the
ceiling is what the link has to be able to carry, not what it will carry.

For planning, the worst case is one sharer at the ceiling: add 96 kbps inbound
for them, and 96 kbps outbound per other participant. In a room of five that is
~0.1 Mbps in and ~0.4 Mbps out on top of the table above - next to the 3.3 Mbps
of the picture, it does not change how the machine is sized.

### Ports

| Port | Protocol | Use |
| --- | --- | --- |
| 8080 | TCP | WebSocket signaling, inbound, configurable with `--port` or `DV_SERVER_PORT` |
| ephemeral | UDP | ICE and media, inbound, chosen by the system unless a range is set |
| 19302 | UDP | STUN, outbound, whatever `network.stun_servers` names |
| 3478 | UDP | TURN, outbound, only when `network.turn_url` is set |
| 27017 | TCP | MongoDB, outbound, only when persistence is on |

The server has to reach the clients over UDP.
Behind NAT it uses the configured STUN servers, plus an optional TURN for the cases STUN does not solve.

### Narrowing the media range

Left alone, the SFU asks the system for an ephemeral port on every connection, and a firewall in front of it
has nothing narrower to allow than the whole ephemeral range: `/proc/sys/net/ipv4/ip_local_port_range`, which
is 32768 to 60999 on most Linux systems. `--ice-port-range` replaces that with a range you choose:

```sh
partyshare-server --ice-port-range=50000-50100
```

The same setting is `network.ice_port_range_begin` and `network.ice_port_range_end` in the configuration file,
and `DV_ICE_PORT_RANGE_BEGIN` and `DV_ICE_PORT_RANGE_END` in the environment.
Both ends have to be given: half a range is refused at startup rather than silently becoming no range at all.

Size it from the load, because the SFU binds one port per participant: `max_participants_per_room` times the
number of rooms running at once. A hundred ports carries twenty full rooms.
The server logs the range it ended up with on startup, or says the ports are ephemeral when none was set.

One range is worth nothing: 1024 to 65535 is libdatachannel's own default, which it reads as "no range" and
answers with an ephemeral port anyway.

```sh
# Linux, with the range above
sudo ufw allow 8080/tcp
sudo ufw allow 50000:50100/udp
```

On AWS, GCP or Azure the security group needs the same two entries; the host firewall alone does not open them.

## Build machine

Building is heavier than running, because of libwebrtc.

| | |
| --- | --- |
| CPU | The more cores the better, the build is parallel |
| Memory | 16 GB, linking libwebrtc is the part that consumes it |
| Disk | 30 GB for the libwebrtc checkout, plus the project build |
| Time | Tens of minutes for libwebrtc, the first time |

The client without the media layer, the server and the tests build without any of that.
The tooling and the dependencies are in [build.md](build.md).
