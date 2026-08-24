# PLAN.md

**This is a record, not a plan.**
The ten milestones it was written to schedule, M0 through M9, have all been executed.
What is left here is the part that is still consulted: the decisions and why they were taken, what each milestone actually delivered, and — the only section that still describes work — what remains unverified.

Three documents divide the job between them:

| Document | What it is | Still authoritative? |
| --- | --- | --- |
| [SPEC.md](SPEC.md) | The product specification. Cited by section number from 57 files across the tree. | **Yes.** Normative. |
| PLAN.md | This file. The milestone record and the decisions behind it. | Historical, except section 4. |
| [docs/postmortems.md](docs/postmortems.md) | The bugs that cost real time, and what each turned out to be. | Yes, and it is where new ones go. |

---

## 1. Stack decisions

Taken before M3, and everything after depends on them.

| Area | Decision | Reason |
| --- | --- | --- |
| Language | C++20 | Set by the SPEC. |
| Build | CMake + CMakePresets + Ninja | Set by the SPEC. |
| UI | Qt 6 (Widgets) | Widgets rather than QML: the UI is dense in controls and needs no heavy animation. |
| WebRTC on the client | libwebrtc, built from source | Delivers AEC3, noise suppression, AGC, jitter buffer, congestion control, SRTP, ICE and `modules/desktop_capture` ready made. |
| Screen capture | `webrtc::DesktopCapturer` | Already covers Windows Graphics Capture, DXGI Desktop Duplication, ScreenCaptureKit, X11 and PipeWire — exactly section 7 of the SPEC. |
| Video codec | H.264 through OpenH264, with hardware encoders behind `webrtc::VideoEncoderFactory` | Cross platform with no GPU dependency, and NVENC or Media Foundation when a card answers. |
| Audio codec | Opus through libwebrtc | Set by the SPEC. |
| SFU on the server | libdatachannel | Small, compilable anywhere, with DTLS-SRTP, ICE and packet level RTP forwarding. libwebrtc is not built to forward media N to N. |
| Signaling | WebSocket + JSON | The WebSocket comes from libdatachannel rather than Boost.Beast, so the server has one network stack instead of two. `nlohmann::json` for the messages. |
| Persistence | MongoDB, optional at build time | Accounts, roles, persistent rooms, chat and the audit log. Without it the server keeps them in memory. |
| Logging | spdlog | The TRACE to FATAL levels of section 23. |
| Tests | GoogleTest | Unit and integration. |
| Dependencies | vcpkg in manifest mode | The same versions on all three platforms and in CI. libwebrtc stays outside vcpkg, as an external binary tree. |

### The libwebrtc decision, which is the one that shaped the rest

libwebrtc has no official release in library form.
The plan started from public prebuilt binaries, pinned at `m152.7977.0.0` and verified by checksum, behind a `Findlibwebrtc.cmake` of our own — and the spike found the reason that could not last.

On Linux the published binaries are compiled against Chromium's libc++, with the `std::__Cr` ABI namespace, while Qt 6 uses libstdc++.
libwebrtc's public API exchanges `std::string` and `std::vector` constantly, so a single binary cannot use both.
**Decision: build libwebrtc from source with `use_custom_libcxx=false`**, automated in `scripts/build_webrtc.sh`.

Windows answered differently and better: the prebuilt package there carries no `std::__Cr::` symbol at all, because it is built against MSVC's own STL.
What it has instead is the same conflict in another currency — `webrtc.lib` compiled against the static C runtime, while Qt ships against the dynamic one and no `/MT` build of Qt exists.
A source build answers that too, and needed a patch of its own, since GN ties the dynamic CRT to `is_component_build`.

The cost accepted: this project maintains and distributes that binary on three platforms.
The alternative it bought its way out of — libdatachannel on the client as well, plus libopus, standalone `webrtc-audio-processing`, OpenH264 and our own screen capture and bandwidth estimation — is significantly more work for less quality.

Everything about that build is in [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md); how to validate it on a new platform is in [docs/webrtc-validation.md](docs/webrtc-validation.md).

### The dependency rule

`ui` depends on `app`, and `app` depends on the media and network modules.
**No media or network module may include a Qt header.** Checked in code review.

---

## 2. What each milestone delivered

| | Milestone | Delivered | Left open |
| --- | --- | --- | --- |
| M0 | Build foundation | Presets for three platforms, sanitizers, clang-format and clang-tidy, logging, configuration, a Qt window, CI on Windows, Linux and macOS | — |
| M1 | Shared protocol | The section 13 messages plus `create_room`, `room_created`, `error`, `ping`, `pong` and `authenticate`; models; [docs/protocol.md](docs/protocol.md); round trip and malformed input tests | — |
| M2 | Signaling server | `rtc::WebSocketServer`, in memory accounts with salt and SHA-256, `RoomManager`, message routing, heartbeat, the one share at a time rule, integration tests with real clients | — |
| M3 | WebRTC toolchain | Version pinned and checksummed, `Findlibwebrtc.cmake`, source builds on Linux and Windows, the spike passing on Linux and Windows, real capture under X11 | Capture on Wayland, through the XDG portal |
| M4 | Audio between two clients | Signaling client off the UI thread, the client media layer, the SFU, STUN, a provisional UI, metrics every 5 s | Mouth to ear latency on a real network |
| M5 | Complete audio | Five participants, device switching mid call, per participant volume, AEC3 + noise suppression + AGC, level indicator and speech detection, mute propagation, a virtual device that runs in CI | Five people on five machines judging echo |
| M6 | Screen sharing | `ScreenCapturer`, monitor selection, a bounded frame queue, 720p at 30 FPS, H.264, SFU video forwarding with PLI, Qt rendering off the UI thread, the controls | Five simultaneous viewers; time to the first keyframe; FPS stability over ten minutes |
| M7 | Final interface | Login, home, room, settings dialog, network quality indicator, visual error handling, reconnection with exponential backoff | The UI thread profile; startup under 3 s |
| M8 | Hardening | Benchmarks at 5×720p30, `tc netem` and an in-process impairment path, bitrate adaptation, NVENC behind a fallback wrapper, sanitizers and static analysis, the security review, crash reporting | Latency on a real network; minidumps on Windows |
| M9 | Packaging and release | MSI and zip on Windows, AppImage on Linux, `.app` and `.dmg` on macOS, signing steps, automatic publishing on a tag, [docs/release.md](docs/release.md) | macOS x64 never built; no signing secret configured; the DMG has never been run |

The mapping to the fifteen MVP acceptance criteria of section 29 of the SPEC:

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

## 3. Decisions taken along the way

The ones that are not obvious from the code, and that would otherwise be rediscovered by argument.

**The server is always the offerer.** Recorded in section 4.3 of [docs/protocol.md](docs/protocol.md).
The participant only answers, which keeps mids, SSRCs and payload types under the SFU's control and reduces forwarding to rewriting a header rather than translating between two independent negotiations.
The reserved `sfu` identifier is the address of that media endpoint: a message addressed to it is consumed by the server rather than relayed.

**Every participant's video m-lines exist from the moment they join**, before any share request: one recvonly carrying their screen upstream, one sendonly bringing down the screen of whoever is sharing.
That is what makes "stop and restart the share without restarting the call" true by construction rather than dependent on getting a mid call renegotiation right, and it costs one idle m-line per participant, which costs nothing on the wire.
Who holds the floor arrives over signaling, in `ScreenShareStarted`, and not from the msid.

**There is no `VideoEncoder` interface of ours, and that is deliberate.**
The plan asked for one so VP9 and AV1 could be added later; that interface already exists and it is `webrtc::VideoEncoderFactory`, which libwebrtc consults per negotiated codec and where the hardware encoders plug in.
What guarantees the extensibility is the SDP: the server offers `addH264Codec` today, and `addVP9Codec` sits one line alongside it.
What the project did have to decide is that the content is a screen and not a camera — `is_screencast()` returns `true`, which is what makes OpenH264 preserve text and let the frame rate fall rather than blurring everything.

**The media layer is a separate library, switched on with `-DDV_BUILD_CLIENT_MEDIA=ON`.**
Without it the client still compiles and runs, which is what keeps the server, the tests and CI free of a library that has to be built from source — and what allows the whole order of operations of a call to be tested with fake media, without a sound card.

**The SFU decides the share bitrate, because only it sees both links.**
The sender sees its own upstream, the viewer its downstream, the server both.
`server/src/sfu/bandwidth_estimator.hpp` uses the loss based half of Google's congestion control: above 10% loss the target falls in proportion to the loss, below 2% it grows 8% per second, and between the two nothing happens, because reacting to noise is how a rate oscillates instead of settling.
The number travels as REMB, which `a=rtcp-fb:96 goog-remb` had already negotiated and nobody was using, and libwebrtc treats it as a ceiling for what its own controller may aim at.
Measured: 2568 kbps clean, 1613 at 20% loss with the SFU asking for exactly 1613, and 2193 once the loss stops.

Two client side changes were needed before any of that could work: `SetBitrate` on the whole connection with a floor, a start and a ceiling — without a start the estimate begins at libwebrtc's default 300 kbps and spends tens of seconds probing upward, which on a shared screen reads as half a minute of blur — and **removing the per encoding minimum**, which is the one thing that makes adaptation impossible.
What section 6 of the SPEC calls a minimum is where the encoder starts and what it aims for on a healthy link, not a floor it may never go below.

**The network is degraded in two ways, and neither replaces the other.**
`scripts/netem.sh` applies named profiles to an interface and is the faithful one, because it degrades the operating system's own queues for every process — and it needs root, only exists on Linux, and does not run on a machine whose kernel was upgraded without a reboot.
`client/src/media/network_impairment.hpp` damages packets inside the client's own UDP sockets, below DTLS and above the operating system, which is the only seam in the stack where a packet can be dropped after the encoder and before the OS without privileges.
That cost swapping `CreatePeerConnectionFactory` for the modular form, the only one that accepts a `PacketSocketFactory`.
The injector is inert by default and nothing in the interface, the configuration or the command line switches it on.

**Asking for AEC3 is one configuration line; knowing it runs is another.**
libwebrtc only publishes `echo_return_loss` while the echo controller is genuinely processing capture, and that is what `AudioStats::echo_cancellation_active` carries.
Two tests use it: one demands the canceller be running, the other turns the option off and demands it stop — without the second, the first would pass even if the metric had nothing to do with what was asked for.
The metric also answers `true` when the *platform's* canceller is the one running, which is the case on Windows and macOS, where libwebrtc disables AEC3 in favour of the system's own.
Answering `false` on a machine that is cancelling echo is worse than answering nothing.

**The audio processing module belongs to the factory, not to the connection**, so sessions in the same process share it and the last applied options win.
A client process has one local user, so this changes nothing in the product — but the test that turns the canceller off has to run on its own.

**A virtual sound card, not a null device.**
`scripts/virtual_audio.sh` builds one on top of PulseAudio with a tone playing into the microphone, and the CI `media` job uses it.
A null device lets negotiation through and captures silence, and captured audio is precisely what M5 has to verify.
The virtual microphone is a `module-remap-source` over the monitor of a null sink, because libwebrtc's PulseAudio backend ignores every source that monitors a sink when enumerating capture devices.

**A signal handler runs between two instructions of a program that has already gone wrong**, so almost nothing may be called there — not `malloc`, not `printf`, nothing that takes a lock the broken thread may already hold.
Everything that allocates is prepared at install time and the handler only writes bytes that already exist: `open`, `write`, `close`, `time` and `backtrace_symbols_fd`, the one backtrace function that does not allocate.
The handler does not decide the process's fate either: it restores the default disposition and re-raises, so core dumps still happen and the exit code still says what killed the program.

---

## 4. What is still not verified

The only section here that describes work rather than history.
A number nobody measured is worth no more than a blank space, so these are stated rather than rounded off.

**Measurements that need machines or time**

- Mouth to ear audio latency under 150 ms **on a real network**. The call was measured surviving 492 ms of injected round trip, which answers "it holds up", not "it is fast".
- Five participants watching one shared screen at once. Two verified; five sessions and twenty tracks exist by test.
- Time to the first keyframe for somebody joining a share in progress. The path exists and the request is tested end to end; the time has not been measured.
- FPS stability over ten minutes.
- Startup under three seconds.
- The UI thread profile. The rule is respected by construction — every core callback arrives through `QMetaObject::invokeMethod` with `Qt::QueuedConnection`, and `ScreenView` holds one pending frame rather than dumping thirty a second into the event loop. Two things do run on the UI thread and both are user calls rather than core ones: the settings dialog enumerates devices and monitors, and `start_screen_share` waits for capture to begin.
- Five people on five machines judging whether echo is perceptible.
- The hardware encoders against a screen that moves. Every number so far is a static screen, and the QP floor caps maximum quality, so the gain under real motion is a prediction.

**Platforms**

- Screen capture on Wayland, through the XDG portal. The development machine runs X11.
- macOS x64: code, presets and DMG packaging exist, and it has never been built or run.
- The DMG has never been installed or started by a person.

**Packaging and release**

- No code signing secret is configured, so the signing steps are written and have never run. What that costs is in entry 17 of [docs/postmortems.md](docs/postmortems.md).
- `docs/build.md` still says nothing about the artifacts.
- Of the four artifacts a tag produces, only the Linux one is verified end to end: built on Ubuntu 22.04 and started inside an Ubuntu 24.04 container with only the thirteen system libraries the AppImage deliberately does not bundle.

**Known open findings**

- Three medium severity items remain in [docs/security-review.md](docs/security-review.md).
- The `users-file` accounts hold plain text passwords. Accepted for the MVP only; the hash has to become Argon2id and the accounts have to come from a real store before any deployment.
- The viewer half of bandwidth estimation is written and tested with real RTCP, and never fires, because negotiating `abs-send-time` makes libwebrtc probe, probing sends padding, and libdatachannel asserts on a padded packet. Entry 7 of [docs/postmortems.md](docs/postmortems.md).
- Crash reporting on Windows writes no minidump. The seam is `install_handlers()` in `shared/src/diagnostics/crash_reporter.cpp`.

---

## 5. Risks, and what became of them

| Risk | Outcome |
| --- | --- |
| Chromium's libc++ against Qt's libstdc++ | **Resolved.** Source build with `use_custom_libcxx=false`, on Linux and on Windows. |
| libwebrtc failing to link on Windows or macOS | **Resolved.** The media layer builds and runs on all three. |
| libwebrtc on the client against libdatachannel in the SFU | **Withdrawn** in M4, by integration test with real ICE, DTLS and RTP. |
| Audio device lifecycle on Linux | **Withdrawn.** A libwebrtc bug, fixed by a patch of ours. Entry 1 of the post-mortems. |
| Software H.264 blowing the CPU budget | **Closed.** 720p30 is viable in software, and hardware encoders exist behind a fallback wrapper. |
| Maintaining our own libwebrtc build for three platforms | **Live, and permanent.** `scripts/build_webrtc.sh` automates it, somebody has to host the binaries, and every milestone update repeats it. The Linux build depends on a pinned libstdc++ 14, because the range Chromium's clang accepts is narrow. |
| Wayland capture through a portal, with user consent | **Live.** The `ScreenCapturer` interface has anticipated the permission flow since M6; nobody has run it. |
| H.264 licensing and patents | **Live.** VP9 stays the plan B the architecture already allows. |
| SFU scope growing | **Contained.** Adaptation landed in M8; simulcast did not, and should not without a reason. |
| Plain text development passwords | **Live.** See section 4. |

---

## 6. After M9

The milestones ended and the project did not, so the work since then is listed here rather than left implied.
None of it is planned work: it is what was built after the plan ran out, through v0.1.18.

| | What |
| --- | --- |
| Server | Room chat, kept by the server and replayed to whoever joins. MongoDB persistence for accounts, roles, persistent rooms, conversations and the audit log, optional at build time. Roles and per account restrictions, checked server side against the account store rather than the session. An ICE port range, after a call that carried nothing through a firewall. |
| Client | `config.ini`, saved settings, a rounded UI, smoothed metric rendering, per participant volume fixes, screen quality controls, automatic bitrate from the resolution, and the audio of the shared screen — the sound coming out of the sharer's machine, described in [docs/audio-da-tela-compartilhada.md](docs/audio-da-tela-compartilhada.md). |
| Platforms | The Windows media layer over a source build, the macOS media layer and DMG packaging, and the libsrtp collision that had been silently breaking every call on a platform where both libraries meet. |
| Tools | [tools/dbadmin](tools/dbadmin/README.md), a terminal front end for the accounts, restrictions and audit log, against the database with no server running. |

Where each of those went wrong on the way is in [docs/postmortems.md](docs/postmortems.md).
