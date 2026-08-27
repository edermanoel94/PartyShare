# SPEC.md

# PartyShare - desktop screen sharing and voice

## 1. Goal

Build a cross platform desktop application, written in **C++**, able to do:

* Real time screen sharing.
* Good quality audio transmission.
* Voice channel communication.
* Initial support for roughly **5 simultaneous participants**.
* Running on Windows, Linux and macOS.
* Producing native binaries for each platform.

The project must prioritize **low latency, stability, low CPU and memory usage, and audio and video quality**.

---

## 2. Platforms

The application has to produce binaries for:

| Platform | Architecture | Status    |
| -------- | ------------ | --------- |
| Windows  | x64          | Mandatory |
| Linux    | x64          | Mandatory |
| macOS    | ARM64        | Mandatory |
| macOS    | x64          | Desirable |

The code has to be mostly shared across platforms, avoiding operating system specific implementations wherever possible.

---

## 3. Technology stack

### Language

* **C++20**
* Compiled with:

  * MSVC on Windows
  * GCC/Clang on Linux
  * Clang on macOS

### Build

Use:

* CMake
* CMake Presets
* Ninja

Expected structure:

```text
project/
├── CMakeLists.txt
├── CMakePresets.json
├── cmake/
├── src/
├── include/
├── tests/
├── assets/
├── third_party/
└── docs/
```

---

## 4. Interface framework

The graphical interface has to be cross platform.

Initial preference:

**Qt 6**

Reasons:

* Native support for Windows, Linux and macOS.
* Good integration with C++.
* A window, event and UI component system.
* The ability to produce native applications.
* Good integration with threads and multimedia facilities.

The UI layer has to stay separate from the layer responsible for capturing, processing and transmitting media.

---

# 5. Main features

## 5.1 Room / channel

The application has to allow creating or joining a channel.

Example:

```text
Channel: dev-room
ID: 8F42A1
```

Each channel initially has to allow:

* Up to 5 participants with simultaneous audio.
* 1 participant sharing a screen at a time.
* Participants joining and leaving dynamically.

---

## 5.2 Screen sharing

The user has to be able to start and stop sharing their screen.

Initial configuration:

```text
Resolution: 1280x720
FPS: 30
```

Characteristics:

* Real time screen capture.
* Monitor selection when there are multiple monitors.
* The future possibility of sharing a single window.
* A control to start and stop the transmission.
* A visual indicator that sharing is active.

### Initial configuration

```text
Resolution: 1280x720
FPS: 30
```

### Quality goals

The system has to prioritize:

1. Low latency.
2. Stable FPS.
3. Good visual quality.
4. Efficient CPU usage.
5. Adaptation to the available bandwidth.

---

# 6. Video codec

The system has to use a modern video codec from the start.

Preference:

**H.264**

Reasons:

* Excellent cross platform support.
* Hardware encoding available on many GPUs and CPUs.
* A good quality to bitrate ratio.
* A mature ecosystem.

The system has to be architected so that these can be added later:

* VP9
* AV1

### Initial bitrate

Suggested initial configuration:

```text
Resolution: 1280x720
FPS: 30
Bitrate: 1.5 Mbps ~ 3 Mbps
```

The bitrate has to be configurable, and adaptive later on.

---

# 7. Screen capture

Capture has to use native APIs where necessary.

### Windows

Preference:

* Windows Graphics Capture

Fallback:

* Desktop Duplication API

### macOS

Use:

* ScreenCaptureKit

### Linux

Support initially:

* PipeWire
* X11

Wayland support has to be considered from the start of the architecture.

---

# 8. Audio

The application has to allow real time voice communication.

Requirements:

* Real time microphone.
* Playback of the participants.
* Mute and unmute.
* Individual volume control.
* Input device selection.
* Output device selection.
* Echo cancellation.
* Noise suppression.
* Automatic Gain Control, where appropriate.

---

# 9. Audio codec

Use:

**Opus**

Initial configuration:

```text
Sample Rate: 48 kHz
Channels: Mono
Bitrate: 32 ~ 64 kbps
Frame Duration: 20 ms
```

The codec has to support stereo audio later on.

Opus is preferred for its low latency and good quality on voice calls.

---

# 10. Real time communication

The architecture has to separate:

```text
Application
    │
    ├── UI
    │
    ├── Screen Capture
    │
    ├── Audio Capture
    │
    ├── Video Encoder
    │
    ├── Audio Encoder
    │
    └── Network Layer
```

For real time media transport, evaluate:

**WebRTC**

WebRTC has to be the primary option for:

* Audio transport.
* Video transport.
* Congestion control.
* NAT traversal.
* Jitter buffering.
* RTP/RTCP.
* Encryption of the communication.

---

# 11. Network architecture

The system has to have a client/server architecture.

```text
                ┌──────────────┐
                │    Server    │
                │              │
                │ Signaling    │
                │ Media        │
                │ Room Manager │
                └──────┬───────┘
                       │
          ┌────────────┼────────────┐
          │            │            │
       Client A     Client B     Client C
```

Initially the server has to be responsible for:

* Authentication.
* Room creation.
* Users joining and leaving.
* Signaling.
* Participant management.
* Coordinating the WebRTC connections.

---

# 12. Media model

For the MVP, use an **SFU (Selective Forwarding Unit)** architecture.

Example:

```text
User A ───────┐
User B ───────┤
User C ───────┼──> SFU
User D ───────┤      │
User E ───────┘      │
                     │
             ┌───────┼───────┐
             │       │       │
             ▼       ▼       ▼
           User A  User B  User C
```

The SFU has to forward the streams without transcoding wherever possible.

That reduces:

* CPU usage.
* Latency.
* Server cost.

---

# 13. Signaling

Signaling is responsible for negotiating the connections.

It may use:

* WebSocket
* JSON

Example:

```json
{
  "type": "join_room",
  "room_id": "8F42A1",
  "user_id": "user123"
}
```

Expected messages:

```text
join_room
leave_room
user_joined
user_left
offer
answer
ice_candidate
screen_share_started
screen_share_stopped
mute
unmute
```

---

# 14. Server

The server may initially be implemented in C++ to keep the ecosystem homogeneous.

Even so, the architecture has to keep the protocol language independent, so the backend can be implemented in another language later.

Responsibilities:

```text
Server
├── Authentication
├── Room Management
├── Signaling
├── SFU
├── User Management
└── Connection Management
```

---

# 15. Project structure

```text
partyshare/
│
├── client/
│   ├── src/
│   │   ├── app/
│   │   ├── ui/
│   │   ├── audio/
│   │   ├── video/
│   │   ├── screen/
│   │   ├── network/
│   │   └── webrtc/
│   │
│   └── CMakeLists.txt
│
├── server/
│   ├── src/
│   │   ├── signaling/
│   │   ├── rooms/
│   │   ├── sfu/
│   │   └── network/
│   │
│   └── CMakeLists.txt
│
├── shared/
│   ├── protocol/
│   └── models/
│
├── tests/
│
├── cmake/
│
├── docs/
│
├── CMakeLists.txt
└── CMakePresets.json
```

---

# 16. Threads

The application must not do heavy processing on the main UI thread.

Suggestion:

```text
Main/UI Thread
      │
      ├── Audio Capture Thread
      │
      ├── Audio Processing Thread
      │
      ├── Screen Capture Thread
      │
      ├── Video Encoding Thread
      │
      ├── Network Thread
      │
      └── WebRTC Thread
```

Communication between components has to use thread safe structures.

Avoid:

* Excessive locking.
* Blocking operations.
* I/O on the UI thread.
* Unnecessary frame copies.

Prefer:

* Move semantics.
* RAII.
* Smart pointers.
* Lock free queues where justified.
* Zero copy where possible.

---

# 17. Security

All media communication has to be encrypted.

Use the security primitives WebRTC provides wherever possible.

The system has to avoid:

* Unencrypted audio or video communication.
* Unnecessary storage of streams.
* Plain text credentials.
* Tokens persisted without protection.

---

# 18. User identity

Every user has to have:

```text
User
├── ID
├── Display Name
└── Avatar
```

For the MVP, authentication may be simple.

Example:

```text
Username
Password
```

These may be added later:

* OAuth.
* Google.
* GitHub.
* Email login.

---

# 19. Initial interface

The application has to have at least:

### Home screen

```text
┌─────────────────────────────────┐
│            PartyShare           │
│                                 │
│        [ Create Room ]          │
│                                 │
│        [ Join Room ]            │
│                                 │
└─────────────────────────────────┘
```

### Room screen

```text
┌────────────────────────────────────────────┐
│ Room: 8F42A1                               │
├────────────────────────────────────────────┤
│                                            │
│              Screen Share                  │
│                                            │
│          ┌───────────────────┐             │
│          │                   │             │
│          │     1280x720      │             │
│          │                   │             │
│          └───────────────────┘             │
│                                            │
├────────────────────────────────────────────┤
│ Participants                               │
│                                            │
│ ● User 1     🔊                            │
│ ● User 2     🔊                            │
│ ● User 3     🔇                            │
│ ● User 4     🔊                            │
│ ● User 5     🔊                            │
│                                            │
├────────────────────────────────────────────┤
│ [🎤 Mute] [🖥 Share Screen] [🚪 Leave]    │
└────────────────────────────────────────────┘
```

---

# 20. MVP

The first version has to contain only:

* [ ] A C++ desktop application.
* [ ] Windows.
* [ ] Linux.
* [ ] macOS.
* [ ] Creating a room.
* [ ] Joining a room.
* [ ] Up to 5 users.
* [ ] Voice communication.
* [ ] Mute and unmute.
* [ ] Screen sharing.
* [ ] 1280x720.
* [ ] 30 FPS.
* [ ] H.264.
* [ ] Opus.
* [ ] WebRTC.
* [ ] Signaling over WebSocket.
* [ ] SFU.
* [ ] Microphone selection.
* [ ] Output device selection.
* [ ] Monitor selection.
* [ ] Encryption.
* [ ] Automated builds for all three platforms.

---

# 21. Out of scope for the MVP

Do not implement initially:

* Call recording.
* File sharing.
* Public streaming.
* More than 5 participants.
* Webcam video.
* 4K transmission.
* Background blur.
* Virtual camera.
* Virtual microphone.
* Advanced AI based noise suppression.
* Mobile applications.

Chat was on this list and is not any more.
It is implemented, persisted with the rooms, and described in section 4.5 of docs/06-protocol.md.

These may be added later.

---

# 22. Performance

Initial goals:

| Metric        |             Goal |
| ------------- | ---------------: |
| Screen Share  |         1280x720 |
| FPS           |               30 |
| Participants  |                5 |
| Audio         |           48 kHz |
| Audio codec   |             Opus |
| Video codec   |            H.264 |
| Latency       |  < 150 ms ideal  |
| CPU           |        Low usage |
| Memory        | < 500 MB client  |
| Startup       |    < 3 seconds   |

The numbers have to be treated as targets and validated through real benchmarks.

---

# 23. Observability

The application has to have structured logging.

Levels:

```text
TRACE
DEBUG
INFO
WARN
ERROR
FATAL
```

Example:

```text
[INFO] Connected to signaling server
[INFO] Joined room: 8F42A1
[INFO] Screen sharing started
[INFO] Video encoder: H264
[INFO] Resolution: 1280x720
[INFO] FPS: 30
```

Important metrics:

* FPS.
* Bitrate.
* Packet loss.
* RTT.
* Jitter.
* CPU usage.
* Memory usage.
* Audio latency.
* Video latency.
* Connection state.

---

# 24. Tests

There have to be:

### Unit tests

Covering:

* Protocols.
* Room management.
* Serialization.
* Configuration.
* Connection states.
* Participant management.

### Integration tests

Covering:

* Signaling.
* WebRTC.
* Room creation.
* Users joining and leaving.
* The audio pipeline.
* The video pipeline.

### Performance tests

Covering:

* 5 users.
* 720p at 30 FPS.
* Different network conditions.
* Packet loss.
* High latency.
* CPU usage.
* Memory usage.

---

# 25. CI/CD

Use GitHub Actions or equivalent.

Pipeline:

```text
Push
 │
 ├── Format
 ├── Static Analysis
 ├── Build
 ├── Unit Tests
 ├── Integration Tests
 └── Package
```

Builds:

```text
Windows x64
Linux x64
macOS ARM64
macOS x64
```

Artifacts:

```text
Windows → .exe / installer
Linux   → AppImage
macOS   → .app / .dmg
```

---

# 26. Code quality

The project has to follow:

* C++20.
* RAII.
* SOLID where applicable.
* A preference for composition.
* Well defined interfaces.
* Separation between UI and core.
* Explicit error handling.
* Smart pointers.
* `std::unique_ptr` by default.
* `std::shared_ptr` only where there is real shared ownership.
* Avoiding manual `new`/`delete`.
* Avoiding global state.
* Automated tests.

Recommended tooling:

```text
clang-format
clang-tidy
cppcheck
AddressSanitizer
UndefinedBehaviorSanitizer
```

---

# 27. High level architecture

```text
                    ┌─────────────────────┐
                    │      Qt 6 UI        │
                    └──────────┬──────────┘
                               │
                    ┌──────────▼──────────┐
                    │    Application      │
                    │       Core          │
                    └──────────┬──────────┘
                               │
        ┌──────────────────────┼──────────────────────┐
        │                      │                      │
        ▼                      ▼                      ▼
┌───────────────┐      ┌───────────────┐      ┌───────────────┐
│ Screen Capture│      │ Audio Capture │      │    Network    │
└───────┬───────┘      └───────┬───────┘      └───────┬───────┘
        │                      │                      │
        ▼                      ▼                      ▼
┌───────────────┐      ┌───────────────┐      ┌───────────────┐
│ Video Encoder │      │ Audio Encoder │      │    WebRTC     │
│     H.264     │      │     Opus      │      │               │
└───────┬───────┘      └───────┬───────┘      └───────┬───────┘
        │                      │                      │
        └──────────────────────┼──────────────────────┘
                               ▼
                         Internet / SFU
                               │
                  ┌────────────▼────────────┐
                  │     Signaling Server    │
                  │        + SFU            │
                  └─────────────────────────┘
```

---

# 28. Roadmap

## Phase 1 - Foundation

* [ ] CMake.
* [ ] Project structure.
* [ ] Qt 6.
* [ ] Logging.
* [ ] Configuration.
* [ ] CI.
* [ ] Cross platform builds.

## Phase 2 - Audio

* [ ] Microphone capture.
* [ ] Playback.
* [ ] Opus.
* [ ] Mute.
* [ ] Device selection.
* [ ] WebRTC Audio Track.

## Phase 3 - Screen share

* [ ] Windows capture.
* [ ] macOS capture.
* [ ] Linux capture.
* [ ] Frame pipeline.
* [ ] H.264.
* [ ] 720p.
* [ ] 30 FPS.
* [ ] WebRTC Video Track.

## Phase 4 - Networking

* [ ] Signaling server.
* [ ] WebSocket.
* [ ] ICE.
* [ ] STUN.
* [ ] TURN.
* [ ] SFU.
* [ ] Room management.

## Phase 5 - UI

* [ ] Login.
* [ ] Creating a room.
* [ ] Joining a room.
* [ ] Participant list.
* [ ] Audio controls.
* [ ] Screen sharing.
* [ ] Connection status.

## Phase 6 - Hardening

* [ ] Tests.
* [ ] Benchmarks.
* [ ] Network simulation.
* [ ] Packet loss testing.
* [ ] Crash reporting.
* [ ] Security review.
* [ ] Performance optimization.

## Phase 7 - Release

* [ ] Windows installer.
* [ ] Linux AppImage.
* [ ] macOS .app.
* [ ] macOS .dmg.
* [ ] Code signing.
* [ ] Release automation.

---

# 29. MVP acceptance criteria

The MVP is considered functional when:

1. A user can create a room.
2. Other users can join it using an ID.
3. Up to 5 users can stay connected simultaneously.
4. All of them can talk over audio.
5. The audio has low latency and adequate quality.
6. A user can start sharing their screen.
7. The other participants can see that screen.
8. Sharing works at **1280x720 and 30 FPS**.
9. The system uses H.264 for video.
10. The system uses Opus for audio.
11. The communication uses WebRTC.
12. The system runs on Windows, Linux and macOS.
13. The project has automated builds for the supported platforms.
14. The application stays responsive during capture, encoding and transmission.
15. The system does not depend on components specific to a single platform.

---

# 30. Architectural principle

The project has to be built thinking first about **real time media**, and not merely as a traditional desktop application.

The fundamental separation has to be:

```text
UI
 ↓
Application Core
 ↓
Media Pipeline
 ↓
WebRTC
 ↓
Network
 ↓
SFU
```

That way the interface can evolve independently of the transmission mechanism, allowing webcam, recording, chat and file sharing to be added later, and the participant count to grow, without rewriting the core of the system.
