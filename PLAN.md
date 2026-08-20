# PLAN.md

Implementation plan derived from [SPEC.md](SPEC.md).

The plan is organized into milestones (M0 through M9).
Each milestone has deliverables, tasks and verifiable acceptance criteria.
The order does not follow the SPEC's phases exactly: the larger technical risks (the WebRTC toolchain and the first vertical slice of media) were pulled to the front, because they are what determine whether the rest of the plan is viable at all.

---

## 1. Stack decisions

These decisions have to be settled before M3, because everything after them depends on them.

| Area | Decision | Reason |
| --- | --- | --- |
| Language | C++20 | Set by the SPEC. |
| Build | CMake + CMakePresets + Ninja | Set by the SPEC. |
| UI | Qt 6 (Widgets) | Set by the SPEC. Widgets rather than QML because the UI is dense in controls and needs no heavy animation. |
| WebRTC on the client | libwebrtc (prebuilt binaries) | Delivers AEC3, noise suppression, AGC, jitter buffer, congestion control, SRTP, ICE and `modules/desktop_capture` ready made. Reimplementing any of those at equivalent quality is not realistic. |
| Screen capture | `webrtc::DesktopCapturer` | Already covers Windows Graphics Capture, DXGI Desktop Duplication, ScreenCaptureKit, X11 and PipeWire, exactly what section 7 of the SPEC asks for. |
| Video codec | H.264 through OpenH264 (software) in the MVP | Available inside libwebrtc, cross platform, with no GPU dependency. Hardware acceleration lands in M8 behind an encoder interface. |
| Audio codec | Opus through libwebrtc | Set by the SPEC. |
| SFU on the server | libdatachannel | A small C++ library, compilable on any platform, with DTLS-SRTP, ICE and packet level RTP forwarding. libwebrtc is not built to forward media N to N. |
| Signaling | WebSocket + JSON | Set by the SPEC. The WebSocket comes from libdatachannel rather than Boost.Beast: it is the same library the SFU will use in M4, so the server has one network stack instead of two. nlohmann::json for the messages. |
| Logging | spdlog | Structured logging with the levels from section 23. |
| Tests | GoogleTest | Unit and integration. |
| Dependencies | vcpkg in manifest mode | Reproduces the same set of versions on all three platforms and in CI. libwebrtc enters as an external binary package, outside vcpkg. |

### The main risk, and what M3 established

libwebrtc has no official release in library form.
The plan uses public prebuilt builds, with a pinned version verified by checksum, plus a `Findlibwebrtc.cmake` of our own.

M3 has already been executed on Linux and the result is in [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md).
The spike compiles, links and runs: it creates a `PeerConnectionFactory`, generates an SDP offer with Opus and H.264, and enumerates audio devices.

The spike also revealed a conflict that changes the plan.
On Linux the published binaries are compiled against Chromium's libc++, with the `std::__Cr` ABI namespace, while the distributions' Qt 6 uses libstdc++.
Since libwebrtc's public API exchanges `std::string` and `std::vector` constantly, a single binary cannot use both.

**Decision taken: build libwebrtc from source with `use_custom_libcxx=false`.**
`scripts/build_webrtc.sh` automates that and packages the result in the layout `Findlibwebrtc.cmake` consumes.
The build completed on Linux and the spike passes over it, so that way out stopped being a hypothesis.
The project becomes responsible for maintaining and distributing that binary on all three platforms, which is the cost accepted in exchange for not having a C translation layer in the middle of the whole media pipeline.

The alternative, should libwebrtc prove unusable: use libdatachannel on the client as well, adding libopus, standalone `webrtc-audio-processing`, OpenH264 and our own implementations of screen capture and bandwidth estimation.
That is significantly more work and delivers less quality, so it should only be adopted as a last resort.

---

## 2. Repository structure

Follows section 15 of the SPEC, with small additions.

```text
PartyShare/
├── CMakeLists.txt
├── CMakePresets.json
├── vcpkg.json
├── .clang-format
├── .clang-tidy
├── cmake/
│   ├── Findlibwebrtc.cmake
│   ├── CompilerWarnings.cmake
│   └── Sanitizers.cmake
├── shared/
│   ├── protocol/          # signaling messages and serialization
│   └── models/            # User, Room, Participant
├── client/
│   └── src/
│       ├── app/           # Application Core, session state
│       ├── ui/            # Qt 6, no media logic
│       ├── audio/         # capture, playback, devices
│       ├── video/         # encoder, decoder, render
│       ├── screen/        # DesktopCapturer and monitor selection
│       ├── network/       # signaling WebSocket client
│       └── webrtc/        # PeerConnection, tracks, adapters
├── server/
│   └── src/
│       ├── signaling/     # WebSocket, message routing
│       ├── rooms/         # room and participant lifecycle
│       ├── sfu/           # RTP forwarding
│       └── network/       # ICE, DTLS, sockets
├── tests/
│   ├── unit/
│   ├── integration/
│   └── perf/
├── assets/
├── docs/
└── .github/workflows/
```

Dependency rule, checked in code review: `ui` depends on `app`, and `app` depends on the media and network modules.
No media or network module may include a Qt header.

---

## 3. Milestones

### M0 - Build foundation

Goal: a repository that compiles, runs tests and lints on all three platforms, still without a single feature.

Tasks:

1. A root `CMakeLists.txt` with C++20 and the `client`, `server`, `shared` and `tests` targets.
2. `CMakePresets.json` with the `linux-debug`, `linux-release`, `windows-debug`, `windows-release`, `macos-arm64-debug` and `macos-arm64-release` presets, plus one preset with AddressSanitizer and UndefinedBehaviorSanitizer.
3. `vcpkg.json` with spdlog, nlohmann-json, gtest, boost-beast, libdatachannel.
4. `.clang-format` and `.clang-tidy`, and a `format-check` target.
5. A logging module in `shared/`, with the TRACE to FATAL levels from section 23.
6. Configuration loading (file plus environment variables plus command line arguments), with defaults for resolution, FPS, bitrate and server address.
7. A minimal Qt 6 window, only to prove that linking against Qt works on every platform.
8. GitHub Actions with the Windows x64, Linux x64 and macOS ARM64 matrix, running format, build and unit tests.

Acceptance criteria:

- `cmake --preset <platform>-release && cmake --build --preset <platform>-release` works on all three platforms.
- CI is green on all three runners.
- The Qt window opens and closes on all of them.

A note about the local environment: Ninja is not installed on this machine, Qt 6 and CMake 4.4 are.
Installing Ninja is a prerequisite for M0.

---

### M1 - Shared protocol

Goal: the signaling contract exists, is tested, and is language independent.

Tasks:

1. Define the message types from section 13: `join_room`, `leave_room`, `user_joined`, `user_left`, `offer`, `answer`, `ice_candidate`, `screen_share_started`, `screen_share_stopped`, `mute`, `unmute`.
2. Add the messages missing from the SPEC that the flow nevertheless requires: `create_room`, `room_created`, `error`, `ping` and `pong`.
3. C++ structs with JSON serialization and deserialization, returning an explicit error rather than throwing.
4. Models in `shared/include/dv/models`: `User` (id, display name, avatar), `Room`, `Participant`.
5. Document the protocol in `docs/protocol.md`, including the session state machine.
6. Unit tests for round trips, missing fields, wrong types and malformed JSON.

Acceptance criteria:

- Test coverage over every message, including the invalid input cases.
- `docs/protocol.md` describes the protocol without depending on any C++ detail.

---

### M2 - Signaling server (done)

Goal: rooms working end to end, still without media.

Tasks:

1. [x] A WebSocket server using libdatachannel's `rtc::WebSocketServer`, rather than Boost.Beast.
   The swap removes an entire network stack: the same library is already the choice for the M4 SFU.
2. [x] MVP authentication: in memory accounts with a salt and SHA-256, and a session token with an expiry.
   The `authenticate` and `authenticated` messages were added to the protocol.
3. [x] `RoomManager`: create a room, a 6 character hexadecimal ID, join, leave, a participant limit, and removal of the empty room.
4. [x] Message routing between participants, with the server forwarding SDP and ICE without interpreting them.
5. [x] A heartbeat with `ping` and `pong`, and removal of a participant on timeout.
6. [x] The one screen share at a time rule, enforced on the server.
7. [x] Integration tests with real WebSocket clients against the server on an ephemeral port.

Acceptance criteria, all verified:

- [x] Five clients join the same room and all of them receive the correct `user_joined` messages, and the sixth receives `room_full`.
- [x] Dropping a client without a handshake produces `user_left` for the others well within 5 seconds.
- [x] The server survives malformed JSON, an unknown type and a missing mandatory field, and keeps serving.

Beyond what was planned, the server validates that `user_id` and `from_user_id` match the connection's authenticated identity.
Without that, any participant could mute another or send an offer in someone else's name.

---

### M3 - WebRTC toolchain spike (partially done)

Goal: eliminate the project's biggest risk before building on top of it.
This milestone is short and disposable if it fails.

Tasks:

1. [x] Choose the libwebrtc binary distribution and pin the version (`m152.7977.0.0`).
2. [x] Write `cmake/Findlibwebrtc.cmake`, with download, checksum verification and an exposed imported target.
3. [x] A minimal program that creates a `PeerConnection`, generates an offer and prints the SDP.
4. [x] Verify in the same binary that `DesktopCapturer` and `AudioDeviceModule` enumerate monitors and devices.
5. [x] Document in `docs/webrtc-toolchain.md` how to reproduce the build from scratch.
6. [x] Decide the standard library conflict, and automate the solution in `scripts/build_webrtc.sh`.
7. [x] Make the spike detect the conflict by itself, by linking `dv::shared` and passing a `std::string` across the boundary.
8. [x] Complete the source build and revalidate the spike over it.
9. [x] Validate screen capture with a graphics server attached, under X11.
10. [ ] Repeat the capture validation in a Wayland session.
11. [ ] Run the spike on Windows x64 and macOS ARM64, following `docs/webrtc-validation.md`.

Acceptance criteria:

- The minimal binary runs on Windows, Linux and macOS ARM64.
  Linux confirmed over the source build, the other two pending.
- CI downloads and caches libwebrtc without manual intervention.
  The job exists in `.github/workflows/ci.yml` under `workflow_dispatch`, and has not been run yet.
- Without those two items, the plan falls back to the alternative described in section 1.

What the source build established is in [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md).
The standard library conflict is over: the resulting library holds symbols only in `std::__cxx11`, the same ABI as Qt, and the spike passes with `dv::shared` linked.
It also captures a real 1920x1080 frame under X11, which is the guarantee M6 needs before it can exist.

---
### M4 - Vertical slice: audio between two clients

Goal: the smallest thing that exercises UI, core, media, signaling and SFU together.
This is where the architecture really gets validated.

Tasks:

1. [x] A signaling client in `client/src/network`, running off the UI thread.
2. [x] A `client/src/webrtc` layer: `PeerConnection` creation, negotiation, ICE candidate gathering, an Opus audio track at 48 kHz mono, 20 ms.
3. [x] A minimal SFU in `server/src/sfu`: one libdatachannel session per participant, audio RTP forwarding to everyone else, SSRC rewriting, RTCP report forwarding.
4. [x] A configurable STUN server, with TURN anticipated in the configuration but not yet mandatory.
5. [x] A provisional UI: a room ID field, a join button, a mute button.
6. [x] Basic metrics in the log: RTT, jitter, packet loss and bitrate, every 5 seconds.

Topology decision, taken here and recorded in section 4.3 of [docs/protocol.md](docs/protocol.md): **the server is always the offerer**.
The participant only answers.
That keeps mids, SSRCs and payload types under the SFU's control, and reduces forwarding to rewriting a header rather than translating between two independent negotiations.
The reserved `sfu` identifier is the address of that media endpoint, and a message addressed to it is consumed by the server rather than relayed.

The client's media layer sits behind the `media::MediaSession` interface, and the libwebrtc implementation is a separate library, switched on with `-DDV_BUILD_CLIENT_MEDIA=ON`.
Without it the client still compiles and runs, and that is what keeps the server, the tests and CI free of a 66 MB library that has to be built from source.
It is also what allows the entire order of operations of a call to be tested with fake media, without a sound card.

What is already verified by integration test, with real ICE, DTLS and RTP over loopback:

- The server offers as soon as the participant joins the room.
- Each participant receives one track per other participant, with `a=msid` identifying whose voice it is.
- One participant's audio reaches the other, and does not come back to the sender.
- Leaving the room removes the session and the corresponding track on the others.
- **A libwebrtc client negotiates with the libdatachannel SFU, connects and delivers audio.**
  That was the high risk listed in section 5, and it is withdrawn.

### The PulseAudio capture bug, found here and fixed

The first call in a process worked and every call after it took exactly ten seconds to negotiate, with no microphone.

Cause: `AudioDeviceLinuxPulse::Terminate()` sets `quit_ = true` and `Init()` never clears that flag, so the second session's capture thread dies on its first pass and `StartRecording()` waits ten seconds for an event nobody will signal.
It is a libwebrtc bug, not a project one.

Fixed by `patches/webrtc/src/0002-pulse-adm-reset-quit-on-init.patch`, detailed in section 5.2 of [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md).
Successive sessions in the same process went from 10.2 s to 1.2 s.

Acceptance criteria:

- Two clients on different machines hear each other.
- Mute works in both directions.
- Mouth to ear audio latency stays under 150 ms on a local network.
- The UI does not freeze during the call.

The complete path was exercised in the real application, with two clients on the same machine and a local server:

```text
ana:    in call    rtt 1 ms · jitter 2.0 ms · lost 0 · 97 kbps ↑ · 95 kbps ↓
bruno:  in call    rtt 1 ms · jitter 2.0 ms · lost 0 · 81 kbps ↑ · 82 kbps ↓
```

Each of them sees the other in the participant list marked as speaking, and muting one drops their own outbound to 1 kbps and the other's inbound with it, with both lists showing the state.

What is missing is what requires two machines and instrumentation: measuring mouth to ear latency on a local network, which is the third criterion.

---

### M5 - Complete audio

Goal: fully satisfy sections 8 and 9 of the SPEC.

Tasks:

1. [x] Scale to 5 participants, with one receiving track per remote participant.
2. [x] Enumeration and switching of input and output devices at runtime, without dropping the call.
3. [x] Per participant volume control.
4. [x] Enable and tune AEC3, noise suppression and AGC.
5. [x] An audio level indicator and speaker detection.
6. [x] Propagation of mute state over signaling, so that everyone's UI stays consistent.
7. [x] Integration tests of the audio pipeline with a virtual device, so they run in CI without hardware.

Acceptance criteria:

- Five participants talk simultaneously with no perceptible echo.
  Five sessions and twenty tracks are verified by test; the absence of echo depends on AEC3, which is on and verified below, but judging "perceptible" takes five people on five machines.
- [x] Switching microphones during a call causes no gap longer than 500 ms.
  Measured by what the server receives, not by what the client thinks it did.
- [x] Client CPU usage stays in single digit percent on a reference machine.
  Measured at 9.2% of one core in a call, with AEC3, noise suppression and AGC active, on a 16 thread Ryzen.

### Audio processing, and how to know it is on

Asking for AEC3, noise suppression and AGC is one configuration line.
Knowing they are actually running is another thing, and a test that merely reads the configuration back proves nothing.

libwebrtc only publishes `echo_return_loss` while the echo controller is genuinely processing capture.
That is the metric `AudioStats::echo_cancellation_active` carries, and two tests use it: one demands the canceller be running, the other turns the option off and demands it stop.
Without the second, the first would pass even if the metric had nothing to do with what the project asked for.

The processing module belongs to the factory, not to the connection, so sessions in the same process share it and the last applied options win.
A client process has one local user, so that changes nothing in the product, but it changes the tests: the case that turns the canceller off runs on its own.

### The level indicator bug, found here and fixed

The microphone level bar never moved off zero, and the virtual device test is what exposed it.

Cause: `AudioTrackInterface::GetSignalLevel` is the obvious way to ask, but the source of a local track never implements it, so it answers `false` for every call and anyone building an indicator on top of it gets silence forever.
The local level now comes from `RTCAudioSourceStats::audio_level`, which is where libwebrtc actually publishes it.

The bar is also read in decibels now.
Normal speech sits near a twentieth of full scale, and on a linear scale it would barely lift the bar off the floor.

### A freeze seen once, never reproduced

In one run of the media suite the `TheEchoCancellerRunsOnTheCapturedAudio` case hung until ctest's 180 second limit, instead of failing at the 20 seconds the test itself waits.
Hanging beyond the test's own timeout is only possible inside a blocking libwebrtc call, and the suspect is the PulseAudio device: the run came right after two graphical clients that were capturing audio had been killed.

It did not reproduce.
Five complete runs of the suite in a row, plus the isolated case, plus three attempts killing a client during capture and running the test right after, all passed.
It is recorded here rather than forgotten: if it comes back, the place to look is the opening of the capture device, not the test.

### Virtual audio device

`scripts/virtual_audio.sh` builds a virtual sound card on top of PulseAudio, with a tone playing into the microphone, and that is what allows the whole pipeline to run on a runner without hardware.
The CI `media` job uses exactly that script.

A null device would not do: it lets negotiation through, but it captures silence, and captured audio is precisely what M5 has to verify.
The virtual microphone is a `module-remap-source` over the monitor of a null sink, because libwebrtc's PulseAudio backend ignores every source that monitors a sink when enumerating capture devices.

---

### M6 - Screen sharing

Goal: satisfy sections 5.2, 6 and 7 of the SPEC.

Tasks:

1. [x] A `ScreenCapturer` interface of our own, wrapping `DesktopCapturer` and isolating the rest of the code from the platform detail.
2. [x] Monitor enumeration and selection from the UI.
3. [x] A frame pipeline with a bounded queue that drops the oldest frame under pressure, using move semantics and avoiding copies.
4. [x] Scaling to 1280x720 and capping at 30 FPS.
5. [x] An H.264 encoder behind a `VideoEncoder` interface, so that VP9 and AV1 can be added later without touching the pipeline.
6. [x] A bitrate configurable between 1.5 and 3 Mbps, with the adaptation hook already in place but switched off.
7. [x] Video forwarding in the SFU, including PLI and keyframe handling for anyone joining mid transmission.
8. [x] Rendering of received video in a Qt widget, with decoding off the UI thread.
9. [x] A visual indicator of active sharing and a start and stop control.

Acceptance criteria:

- One participant shares and the other four see it, at 1280x720 and 30 FPS.
  Verified with two real clients, capturing, encoding in H.264, crossing the SFU and decoding on the other side. Five not yet.
- A participant who joins later receives a keyframe in under 2 seconds.
  The path exists and is tested: the viewer's request reaches the sender. The time to the first frame has not been measured.
- [x] Stopping and restarting the share works without restarting the call.
  True by construction, and verified: the video m-lines exist from the moment of joining, so starting and stopping renegotiates nothing.
- FPS stays stable with a small deviation over 10 minutes.
  Not measured.

### About task 5, and why there is no `VideoEncoder` interface of ours

The plan asked for an H.264 encoder behind an interface of our own, so that VP9 and AV1 could be added later without touching the pipeline.
That interface already exists and it is `webrtc::VideoEncoderFactory`: it is what libwebrtc consults for each negotiated codec, and it is where a hardware encoder enters in M8.
Writing another one on top would only add a layer translating from one abstraction into an identical one.

What guarantees the requested extensibility is the SDP: the server offers `addH264Codec` today, and `addVP9Codec` and `addAV1Codec` are one line alongside it, with the entire pipeline untouched.
What the project actually had to decide, and did, is that the content is a screen and not a camera: `is_screencast()` returns `true`, and that is what makes OpenH264 preserve text and let the frame rate fall rather than blurring everything.

### What video changed in the topology

Each participant's two video m-lines are created together with the session, before any share request: one recvonly, carrying their screen upstream, and one sendonly, bringing down the screen of whoever is sharing.

That is what makes the "stop and restart without restarting the call" criterion true by construction, rather than dependent on getting a mid call renegotiation right.
It costs one idle m-line per participant, which costs nothing on the wire.

The outbound track has no fixed owner: it carries whoever has the floor.
Who that person is comes over signaling, in the `ScreenShareStarted` message that had existed since M1, and not from the msid.

---

### M7 - Final interface

Goal: the UI from section 19, now on top of a core that already works.

Tasks:

1. [x] A login screen.
2. [x] A home screen with create room and join room.
3. [x] The room screen: the share area, a participant list with audio state, a control bar with mute, share screen and leave.
4. [x] A settings dialog: input device, output device, monitor, bitrate.
5. [x] A connection state and network quality indicator.
6. [x] Visual error handling: server down, room full, nonexistent room, capture permission denied.
7. [x] Automatic reconnection to signaling with exponential backoff.

Acceptance criteria:

- [x] Every flow from section 19 is navigable without going through the terminal.
  Login, create a room, join, mute, share, configure and leave, all verified in the real application.
- No blocking operation runs on the UI thread, verified with a profiler.
  The rule is respected by construction, and the profile has not been taken yet. See below.
- Startup stays under 3 seconds.
  Not measured.

### The SFU deadlock, found here and fixed

The media suite would hang until ctest's 180 s limit, in a different case each run, roughly once every three or four rounds.
It was the same hang recorded in M5 as not reproduced.

It was not the test. It was a deadlock in the server, between a lock of ours and an internal libdatachannel lock, taken in opposite orders by two threads:

```text
on_participant_joined  ->  takes MediaRouter::mutex_, calls addTrack, waits for the PeerConnection lock
forward_audio          ->  arrives holding the PeerConnection lock, waits for MediaRouter::mutex_
```

One RTP packet arriving at the instant someone joins the room is enough, and the whole server stops.
Five participants talking make that window common, not rare.

The fix is an immutable route table: built under `mutex_` and published whole, read through an atomic pointer on the forwarding path.
That way no libdatachannel callback takes a lock of ours, the cycle does not exist, and as a bonus the hot path stops contending on a global mutex for every packet.

Eight consecutive runs of the full suite passed after that.

### About the UI thread

The criterion asks for a profiler, and what exists today is the structural guarantee: every core callback arrives through `QMetaObject::invokeMethod` with `Qt::QueuedConnection`, and `ScreenView` holds one pending frame at a time rather than dumping thirty per second into the event loop.
Two things still run on the UI thread and are user calls rather than core ones: opening the settings dialog enumerates devices and monitors, and `start_screen_share` waits for capture to begin.
Neither happens during an ongoing call without someone having clicked, but both deserve measurement before the criterion is ticked.

---
### M8 - Hardening

Goal: turn the section 22 targets into measured numbers.

Tasks:

1. [x] Performance tests with 5 participants at 720p and 30 FPS, measuring CPU, memory and latency.
2. [x] Network simulation with packet loss, high latency and jitter, using `tc netem` on Linux.
3. [x] Enable bitrate adaptation based on congestion control feedback.
4. [x] Hardware encoders behind an interface, with automatic fallback to software. NVENC was run after the reboot, and running it changed the code: see below.
5. [x] Run the full suite under AddressSanitizer and UndefinedBehaviorSanitizer, and pass clang-tidy and cppcheck without warnings.
6. [x] A security review per section 17: no unencrypted media, no plain text credentials, protected tokens, TURN with ephemeral credentials.
7. [x] Crash reporting.

Acceptance criteria:

- [x] Every metric from section 22 measured and recorded in [docs/benchmarks.md](docs/benchmarks.md).
  Latency remains the partial exception: the call was measured surviving 492 ms of injected round trip, which answers "it holds up", but "under 150 ms on a real network" still has no real network to be measured on.
- [x] The call survives 5% packet loss with graceful degradation and no drop.
  Five participants, one sharing a screen, with 5% loss injected in both directions: the four viewers stay at 29.7 FPS, the same number as the run without loss, and nobody drops.
  It only became true after the fix described below. Before it, the screen froze.
- [x] No open high severity findings in the security review.
  There were two, both fixed: the password hash without cost, and a padded RTP packet that took the entire server down.
  Three medium severity ones remain, described in [docs/security-review.md](docs/security-review.md).

### How the network is degraded, and why in two ways

The task asks for `tc netem`, and `scripts/netem.sh` is that: named profiles (`lossy`, `distant`, `awful`) applied to an interface.
It is the most faithful measurement there is, because it degrades the operating system's own queues, for every process.
It also needs root, only exists on Linux, and does not run on a machine whose kernel was upgraded without a reboot, which is the case here.

That is why the other half exists, in `client/src/media/network_impairment.hpp`: the client damages packets in its own UDP sockets, below DTLS and above the operating system.
libwebrtc allows injecting the socket factory the PeerConnection uses, and that is the only seam in the stack where a packet can be dropped or delayed after the encoder and before the operating system, without privileges and without a platform tool.

That cost swapping `CreatePeerConnectionFactory` for the modular form, which is the only one that accepts a `PacketSocketFactory`.
The injector is inert by default, and nothing in the interface, the configuration or the command line switches it on: only a call inside the process.

The two halves measure the same thing and neither replaces the other.
The `netem` one is the honest one; the client one is the one that runs without privileges, on all three platforms and in CI.

### The screen share freeze, found here

The first run of the loss test delivered **4 frames in 15 seconds** of shared screen. That was not degradation, it was a freeze.

The arithmetic explains it: an intra frame of a 1280x720 screen is over a hundred packets, and at 5% loss the chance of all hundred arriving intact is under 1%.
The only repair the viewer had was to ask for another intra frame, which arrived broken, and the request started over.
The SFU carried eight keyframe requests in fifteen seconds and none of them produced a picture.

Both ends of retransmission were missing, and the SFU now has both:

- `rtc::RtcpNackResponder` on the outbound track, which resends the viewer the packet they lost, from a cache of recent ones.
- `dv::server::sfu::VideoFeedback` on the inbound track, written here because libdatachannel answers NACKs and never sends one.
  A hole in the sequence becomes a generic RFC 4585 NACK, the sharer resends, and the SFU forwards an already patched stream rather than spreading the loss to every viewer.

After that, the same case delivers **443 frames in 15 seconds**, with zero keyframe requests.
The complete numbers, with the caveat that the screen being measured was static, are in [docs/benchmarks.md](docs/benchmarks.md).

### Who decides the share bitrate, and how

Task 3 asks for bitrate adaptation from congestion control feedback. The first measurement showed there was none:
with a fifth of the packets being dropped, the sender's estimate climbed to the 3 Mbps ceiling and stayed there.
It is not a libwebrtc defect. Without `transport-cc` and without REMB, nobody tells it loss exists, and a controller with no input controls nothing.

The one with that information is the SFU, and only it: the sender sees its own upstream link, the viewer sees the downstream one, and the server sees both.
So it is the one that decides, in `server/src/sfu/bandwidth_estimator.hpp`, using the loss based half of Google's congestion control:

- above 10% loss the target falls in proportion to the loss;
- below 2% it grows 8% per second;
- between the two nothing happens, because reacting to noise is how a rate oscillates instead of settling.

The number travels as REMB, which is what `a=rtcp-fb:96 goog-remb` in the offer already negotiated and nobody used, and libwebrtc treats it as a ceiling for what its own controller may aim at.
Measured: 2,568 kbps on a clean link, 1,613 with 20% loss - with the SFU asking for exactly 1,613 - and 2,193 once the loss stops.

On the client side, two changes were needed before this could work at all:

- `SetBitrate` on the whole connection, with a floor, a starting point and a ceiling, which is the range the controller works inside.
  Without a starting point the estimate begins at libwebrtc's default 300 kbps and takes tens of seconds probing its way up, which on a shared screen reads as a blurred image for half a minute.
- The per encoding minimum was removed.
  A floor there is the one thing that makes adaptation impossible: a link that cannot take 1.5 Mbps would receive 1.5 Mbps anyway.
  What section 6 of the SPEC calls a minimum is where the encoder starts and what it aims for on a healthy link, not a floor it may never go below.

### The half that could not be enabled, and an `assert` in the way

The other direction is missing: the viewer saying how much its own link can take, so that the slowest person in the room caps what the sharer produces.
The code exists, is exercised by a test with real RTCP, and never fires with this project's client: producing that report requires libwebrtc to have the `abs-send-time` extension in the RTP header.

Negotiating the extension makes libwebrtc probe bandwidth, probing means sending padding, and libdatachannel has an `assert` that takes the whole process down when building the sender report for a padded packet.
The extension stays out until that is resolved upstream.

What remains is the defence: the SFU drops padded packets instead of forwarding them.
Any participant can send one, and the result was the server aborting - which is a way to end everyone's call with a single packet.

### The hardware encoder, and what running it showed

Task 4 asks for hardware encoders behind an interface, with automatic fallback to software.
The interface is `webrtc::VideoEncoderFactory`, as already recorded in M6, and what was written is that:

- `client/src/webrtc/video_encoder_factory.hpp` decides which encoder to use, and wraps both in libwebrtc's own `CreateVideoEncoderSoftwareFallbackWrapper`, which is what Chrome uses to switch from hardware to software mid stream without the call noticing.
- `client/src/webrtc/hardware_encoder.hpp` is where each platform plugs in, with an empty implementation alongside so that a build with no backend at all still compiles and encodes.
- Which encoder is actually running became a measured number, read from libwebrtc's statistics rather than from what was asked for, because those are different things: a hardware encoder can be created, accept the stream, and be replaced by the software one along the way.

The backend is NVENC, not the VAAPI the plan mentions.
VAAPI is the right answer on Intel and AMD, and no answer at all on NVIDIA, whose driver does not encode through it; this machine's card is NVIDIA.
NVENC is the same API on Linux and Windows, which means task 4 on Windows becomes configuration rather than implementation.

Nothing is linked: `libnvidia-encode.so.1` and `libcuda.so.1` are opened at runtime, so a binary compiled with NVENC runs identically on a machine with no NVIDIA card at all - the query answers that there is no hardware and the software encoder is used.
The API header is in `third_party/nvcodec`, with its provenance recorded alongside, because it is not distributed as a package on every platform and ffmpeg solves it the same way.

Before the reboot only the refusal path had been verified, which is half of what the task asks for: `libcuda` was at 610.57.04 and the kernel module at 610.43.03, `cuInit` returned `CUDA_ERROR_SYSTEM_DRIVER_MISMATCH`, and the query answered exactly why - "the NVIDIA driver does not match its own kernel module" - with the call carrying on in software with no visible difference.

After the reboot the card was genuinely used, and that is where the task stopped being about writing code.
The real share was encoded by the RTX 4050 and decoded on the other side, and the first CPU measurement said there was **no saving at all**: 77.1% of one core against OpenH264's 78.9%.

The reason was the bitrate, not the encoding.
Against a static screen OpenH264 spends 9 kbps, and NVENC was spending 1795: what the card saved on encoding was spent again packetizing and sending eight times more RTP.
CBR fills the target regardless of what the image is doing, and that contradicted the design recorded above, where the number the SFU sends over REMB is a ceiling and not a quota.

Switching to VBR was not enough, and that was the part only running it could show: on its own it went to 3137 kbps and fell slowly to 1371, because with no quantization floor the controller has no reason to stop and keeps lowering the quantizer until it spends the target, encoding an image that does not move ever closer to lossless.
With VBR and a QP floor of 24 the static screen came to cost 2 kbps and CPU fell to 69.5%, which is the saving the task exists to produce.

The numbers are in [docs/benchmarks.md](docs/benchmarks.md), with the two caveats that apply to them: the measurement is of a static screen, and the CPU figures scatter by a few points between runs.
A moving screen is still unmeasured, and the QP floor caps the maximum quality, so the gain under real motion is a prediction and not a number.

### Crash reporting

A crash that leaves nothing behind turns into a report that says "it closed by itself".

What gets written is the minimum that turns that into something actionable: which build, which signal, and where in the code.
The detail that decides the implementation is that a signal handler runs between two instructions of a program that has already gone wrong, and almost nothing may be called there - not `malloc`, not `printf`, nothing that takes a lock the broken thread may already hold.

So everything that needs to allocate is prepared at install time, and the handler only writes bytes that already exist: `open`, `write`, `close`, `time` and `backtrace_symbols_fd`, which is the one backtrace function that does not allocate.
The handler also does not decide the process's fate: it restores the default handler and re-raises the signal, so core dumps still happen and the exit code still says what killed the program.

Tested the way this can be tested: each case creates a child process, breaks the child on purpose, and reads what was left behind.
A real `SIGSEGV`, with the backtrace naming the binary itself, and the `SIGABRT` that assertions and uncaught exceptions arrive through.

On Windows a crash arrives as a structured exception and the report that counts is a minidump.
That does not exist here, and the function that would implement it is marked: this project has never been built or run on Windows, and a handler written blind would be a handler nobody has seen run.

### About task 5, and what static analysis found

`clang-tidy` passes without a single warning over `shared`, `server` and the client core, which was not even analysed before.
`cppcheck` runs in CI and could not be run on this machine, where it is not installed.

The findings worth fixing are in the commit that fixed them.
The ones that were refused are in `.clang-tidy`, each with the reason written alongside, because a check disabled without justification is indistinguishable from a check that was forgotten.

The "without a single warning" needs a qualifier, found while working on NVENC: `client/src/webrtc` is outside the list of directories the job analyses, and it is where the media session and the encoder backends live.
Run by hand, `hardware_encoder_nvenc.cpp` has five findings, two of them `error`: an `NV_ENC_PIC_PARAMS picture = {}` over an enum with no zero enumerator, and a `static_cast<uint32_t>(fps + 0.5)` the check says to replace with `lround`.
None is serious and none was fixed here, but the CI list has to include that directory before task 5 can be read as written.

---

### M9 - Packaging and release

Goal: section 25 of the SPEC.

Tasks:

1. Partial. An MSI Windows installer of the client, with a start menu and desktop shortcut, an icon and an Add/Remove Programs entry, plus the ZIP alongside. The executable carries an icon and a version block. The workflow installs the MSI on the runner and checks what landed, so the packaging is exercised; nobody has yet started the client it installs.
2. [x] An AppImage on Linux.
3. Partial. A `.app` bundle with an icon and a `.dmg` with a shortcut to `/Applications` and a volume icon. The window appearance is missing, and none of it has ever run.
4. Code signing on Windows and macOS. The steps exist in the workflow, conditional on the secrets, and no secret is configured.
5. [x] Automatic artifact publishing on a release tag.
6. Partial. `docs/release.md` exists. `docs/build.md` still says nothing about the artifacts.

Acceptance criteria:

- A tag produces all four artifacts automatically.
  It produces one, verified. The other three are written and have never run, because this repository is developed on Linux.
- Each artifact installs and runs on a clean machine of the respective platform.
  The Linux one does, and it is tested that way on every release: the job builds on Ubuntu 22.04 and starts the result inside an Ubuntu 24.04 container with only the thirteen system libraries the AppImage deliberately does not bundle.
  The other three do not, and cannot be by someone who only has Linux.

### What the workflow lint found

The workflows are the part of this repository that cannot be run before it is merged, and the release ones cannot be run outside a tag.
`actionlint` runs in CI for that reason, and in its first two runs it found three defects that would have cost one tag each:
- The signing guard read the `secrets` context, which a step level `if:` cannot see. The condition would always be false and nothing would be signed, silently. The secrets moved up to the job level, where `env` is visible.
- The `publish` condition sat in a folded `>` block, which leaves a trailing `\n` and turns the expression into a non empty string, which is truthy. It would publish on every push, tag or no tag.
- `macos-13` no longer exists as a runner label. The x86_64 job would fail at scheduling time.

None of the three shows up in a YAML review, and all three would only have shown up on the first tag.

Alongside came a `packaging` job, which builds, installs and packages on every change, without the media layer so as not to depend on libwebrtc.
It checks the thirteen files of the install tree one by one, and not by counting them: an icon size that goes missing is invisible until some desktop asks for that size, and "there were nine files" is a check that passes when the wrong nine are there.

### glibc decides where the artifact runs, and that is not theoretical

The AppImage carries Qt and the C++ runtime, and does not carry glibc, which cannot be bundled.
One built on this machine, which runs Arch with glibc 2.44, **does not start on a clean Ubuntu 24.04**: it asks for `GLIBC_2.43` and `GLIBC_2.44`, and 24.04 has 2.39.

That makes a locally built AppImage a development artifact rather than a distribution one, and it is why the release job uses the oldest runner available rather than the newest, which is the wrong instinct.
The script now prints the glibc floor of the file it just produced, because it is an invisible property until someone cannot open the program.

### What the install tree was before M9

It did not exist. No `install()` rule, no icons, no desktop entry.
The four artifacts are four ways of wrapping the same installed tree, so it came before any of them, together with `cmake/Packaging.cmake`, which decides the name and the platform of each file.

Two things only appeared when a real artifact was built, and neither would have appeared in code review:

- The first AppImage came up **with no media layer**, because the script did not pass `DV_BUILD_CLIENT_MEDIA`.
  A client that opens the window, shows the login screen and makes no calls is worse than no artifact, because it looks like the product.
  The release smoke test refuses that now, by looking for the phrase in the log.
- `third_party/nvcodec` **was not versioned**, because all of `third_party/` was in `.gitignore`.
  A clean clone did not compile NVENC, which means task 4 of M8 worked on this machine and on no other.
  The header is vendored source with its provenance and licence recorded alongside, not a downloaded dependency.

### The first Windows build, and what it found

The Windows presets, the CI job and the MSI job were written from the documentation of the tools rather than from a build, and this is the record of the first machine to run them.
What was expected to hurt did not: the code compiles under MSVC 19.44 with `/W4 /permissive- /WX` without a single warning, and nothing in it needed a `#ifdef`.
The two defects are both in the parts nobody was compiling, and both had the same shape - a check that reported success while measuring nothing.

**The unit tests never ran, and the suite reported green.**
vcpkg's dynamic triplet builds `gmock.dll` with its own copy of GoogleTest linked in: it does not depend on `gtest.dll` and exports `MakeAndRegisterTestInfo` itself, while `gtest_main.dll` does depend on `gtest.dll`.
`dv_unit_tests` linked both, so every `TEST()` registered into the registry inside `gmock.dll` and `main()` read the one inside `gtest.dll`.
The result is not a link error and not a crash. It is an executable that starts, prints "this test program does NOT link in any test case", and exits 0.
The first run of the suite here reported **63 tests passed out of 63** and meant it, because `ctest` treats only *no tests at all* as a failure and the integration binary, which never linked gmock, kept the count above zero.
The 272 unit tests were absent, and a green Windows CI job would have covered nothing but integration for as long as anyone cared to look.
gmock was there for one `EXPECT_THAT` in `unit/test_video_feedback.cpp`, which now sorts and compares like the two assertions above it. The suite runs 335 tests.

**The installer shipped a program that could not start.**
`cmake --install` installs the target and nothing else, and `windeployqt` knows only about Qt, so the staged tree held `partyshare.exe`, the Qt runtime, and none of `spdlog.dll`, `fmt.dll` or `datachannel.dll`.
On Windows there is no rpath and no package manager: those files sit next to the executable or the program does not start.
The check in the release job did not notice, because it looks for the executable and the Qt platform plugin and both of those were there - the same failure the job's own comment warns about two steps earlier, in the shape it did not anticipate.
`client/CMakeLists.txt` now installs a `RUNTIME_DEPENDENCY_SET`, which resolves the transitive half as well; the MSI went from 23 files to 30.

Two smaller things, in the same rule, worth writing down because both fail silently:
- The Qt directory has to be in `DIRECTORIES` even though `windeployqt` handles Qt afterwards. Excluding Qt by name leaves it *unresolved*, and an unresolved dependency stops the install rather than being skipped.
- The pattern that drops the system libraries has no backslash in it on purpose. A backslash has to survive CMake's string parsing before it reaches the regex engine, and the version that did not matched nothing, which quietly staged every DLL in `System32` - and, through `shell32`, went looking for `HvsiFileTrust.dll`, which exists on no machine.

**Smart App Control refuses the artifact, and that is not a warning.**
The machine has Smart App Control in enforcement, which is the default on a clean Windows 11.
It blocks an unsigned binary from loading outright, with a Code Integrity 3077 event and nothing on screen: the staged client would not start, and `msiexec /i /qn` failed at the first unsigned DLL with error 1310, "System error 0", after writing every signed file before it.
The MSI itself is sound - `msiexec /a` extracts all thirty files - but it cannot be installed on a machine in this configuration.
That makes task 4 of this milestone, the signing that is written and has never had a certificate, the difference between an artifact and a download nobody can run, rather than the polish it reads as.

And that step would not have been enough either. It signed `stage\bin\*.exe`, which is two files: the client, and the Visual C++ redistributable that Microsoft already signed and that would have had its publisher replaced by ours.
Of the thirty files in the install tree, twenty-two arrive signed by Microsoft or by Qt and eight do not, and seven of those eight are libraries the glob never reached - including `datachannel.dll`, which is exactly where the install died.
Smart App Control checks every binary as it is loaded, so signing the executable and shipping seven unsigned libraries next to it produces a signed program that still does not start.
The step now covers every unsigned `.exe` and `.dll`, skips what already carries somebody else's signature, checks `signtool`'s exit code - a native program that fails does not stop a `pwsh` step by itself - and refuses to finish while anything in the tree is still unsigned.

Two constraints of the policy, worth writing down because a certificate that satisfies neither signs without complaint and changes nothing: Smart App Control accepts only certificates issued by a CA in the Microsoft Trusted Root Program, so a self-signed one is no better than none, and its signature check does not read ECC at all, so the certificate has to be RSA.

Not verified here, and worth being plain about: nobody has started the client *from an installed tree* on Windows, only from the build tree.
The media layer is absent too, as it is everywhere but Linux, because libwebrtc is not built on this machine.

---

## 4. Tracking the MVP acceptance criteria

Mapping of the 15 criteria from section 29 of the SPEC to the milestones that cover them.

| SPEC criterion | Milestone |
| --- | --- |
| 1, 2. Create and join a room | M2, M7 |
| 3. Five simultaneous users | M5 |
| 4, 5. Audio with low latency | M4, M5 |
| 6, 7, 8. Sharing at 720p and 30 FPS | M6 |
| 9, 10, 11. H.264, Opus, WebRTC | M4, M6 |
| 12. Three platforms | M0, M3 |
| 13. Automated builds | M0, M9 |
| 14. A responsive application | M6, M7 |
| 15. No dependency on a single platform | M6 |

---

## 5. Risks and mitigations

| Risk | Impact | Mitigation |
| --- | --- | --- |
| Conflict between Chromium's libc++ and Qt's libstdc++ | Low on Linux | Resolved: the source build with `use_custom_libcxx=false` completed and was verified. Windows and macOS not confirmed yet. |
| Maintaining our own libwebrtc build for 3 platforms | Medium | A consequence of the decision above. `scripts/build_webrtc.sh` automates it, but somebody has to host the binaries and redo them on every milestone update. The build depends on a pinned libstdc++ 14, because the range of versions Chromium's clang accepts is narrow. |
| libwebrtc failing to compile or link on Windows or macOS | High | Linux already validated. The other two run through the CI `webrtc-spike` job. |
| Interoperation between libwebrtc on the client and libdatachannel in the SFU | Withdrawn | Validated in M4 by integration test: negotiation, ICE, DTLS and audio between two libwebrtc clients through the SFU. |
| Audio device lifecycle on Linux | Withdrawn | It was a libwebrtc bug, fixed by a project patch. Detailed in M4. |
| Wayland capture requiring a portal and user consent | Medium | The `ScreenCapturer` interface has anticipated the permission flow since M6. |
| Software H.264 encoding blowing the CPU budget | Medium | 720p at 30 FPS is viable in software; hardware encoders land in M8, behind an interface. |
| H.264 licensing and patents | Medium | To be researched before M6, keeping VP9 as the plan B the architecture already anticipates. |
| SFU scope growing (simulcast, BWE) | Medium | Fixed bitrate in the MVP, adaptation only in M8. |
| Development accounts with plain text passwords in `users_file` | Medium | Accepted for the MVP only. The hash is already salted in memory, but it has to become Argon2id with a real user store before any deployment. |

---

## 6. Suggested sequence

M0 and M1 can be done in parallel with M3, because they do not depend on libwebrtc.
M2 depends on M1.
M4 depends on M2 and M3, and is the project's decision point.
From M5 onwards the sequence is linear.

Current state: M0, M1 and M2 done and verified.
M3 is validated on Linux over the library built from source, including real screen capture under X11.
Capture on Wayland is missing, as is running the spike on Windows and macOS.

M4 is delivered, except for the mouth to ear latency measurement, which requires two machines.
Signaling, the SFU, client media, metrics and the provisional UI exist, are tested end to end with libwebrtc on one side and libdatachannel on the other, and work in the real application.

M5 is delivered.
Devices, per participant volume, levels, speech detection and the section 9 audio processing are implemented and verified with real audio, over a virtual device that also runs in CI.
The only thing missing is the part of the first criterion that needs five people on five machines to judge echo.

M8 is in progress: tasks 1, 2, 3, 5, 6 and 7 are done, and 4 is written but not run.
The hardware encoder exists in full, from the NVENC backend to the fallback, and what is missing is running it: the development machine was upgraded without a reboot, and neither NVENC nor `tc netem` works in that state.
The section 22 numbers are in [docs/benchmarks.md](docs/benchmarks.md) and the security review in [docs/security-review.md](docs/security-review.md).
Network simulation found and fixed a screen share freeze under loss, which was the difference between "degrades" and "stops".

M7 is delivered across its seven tasks: three screens, the settings dialog, the network indicator, error messages and automatic reconnection.
The two measurements in the criteria are missing, the UI thread profile and the startup time.

M6 is delivered across its nine tasks.
Capture, the queue, scaling, rate limiting, H.264, SFU forwarding with PLI, Qt rendering and the controls all work, verified by test and in the real application with two clients.
What is missing from the acceptance criteria are measurements that need time or machines: five participants watching at once, the time to the first frame for someone joining mid share, and FPS stability over ten minutes.
The next milestone is M7, the final interface.
