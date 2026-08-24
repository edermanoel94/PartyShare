# Post-mortems

Bugs that cost real time, and what each one turned out to be.

These were extracted from `PLAN.md`, where they had accumulated milestone by milestone.
They are here because they outlived the plan: the milestones are done, and these are still the answer to "why is this code like this".

One theme runs through half of them and is worth naming before the list: **a check that reports success while measuring nothing**.
A green test suite that registered no tests, an installer whose verification looked for the two files that happened to be there, a GN argument answered with a warning and a zero exit, a `.gitignore` rule that made `git add` refuse a file without a word.
None of those fail. They pass, and the thing they were meant to protect is simply absent.

| # | The bug | Where it lived |
| --- | --- | --- |
| 1 | The second call in a process had no microphone | libwebrtc, PulseAudio backend |
| 2 | The level bar that never moved off zero | `AudioTrackInterface::GetSignalLevel` |
| 3 | Two copies of libsrtp, and a call that carried nothing | libwebrtc + libdatachannel in one binary |
| 4 | The bundle that asked macOS for a microphone it had not declared | `Info.plist` |
| 5 | The SFU deadlock | `MediaRouter` against libdatachannel |
| 6 | The screen share freeze under 5% loss | Missing NACK in both directions |
| 7 | One padded RTP packet takes the server down | A libdatachannel `assert` |
| 8 | The server took UDP ports the firewall did not open | `--ice-port-range` unset |
| 9 | NVENC spending 1795 kbps on a screen that does not move | CBR, and no quantizer floor |
| 10 | Media Foundation doing the same, plus writes that did not stick | `ICodecAPI` ordering |
| 11 | 272 unit tests absent, and the suite green | `gmock.dll` with its own registry |
| 12 | The installer shipped a program that could not start | `cmake --install` installs the target and nothing else |
| 13 | Two things `.gitignore` quietly deleted from the build | An unanchored `build/`, and `third_party/` |
| 14 | The first AppImage came up with no media layer | A missing flag in the packaging script |
| 15 | The AppImage that will not start on Ubuntu | glibc cannot be bundled |
| 16 | Three workflow defects, one tag each | Only reachable from a tag |
| 17 | Smart App Control and an unelevated `msiexec` | Two failures read as one |
| 18 | The first MSVC build | Four defects that stop the build |

---

## 1. The second call in a process had no microphone

**Symptom.** The first call in a process worked. Every call after it took exactly ten seconds to negotiate, and carried no audio.

**Cause.** `AudioDeviceLinuxPulse::Terminate()` sets `quit_ = true`, and `Init()` never clears it.
The second session's capture thread dies on its first pass, and `StartRecording()` waits ten seconds for an event nobody will signal.
A libwebrtc bug, not a project one.

**Fix.** `patches/webrtc/src/0002-pulse-adm-reset-quit-on-init.patch`, described in section 5.2 of [webrtc-toolchain.md](webrtc-toolchain.md).
Successive sessions in the same process went from 10.2 s to 1.2 s.

## 2. The level bar that never moved off zero

**Symptom.** The microphone level indicator read zero forever. The virtual device test is what exposed it, because a real microphone in a quiet room reads nearly zero too.

**Cause.** `AudioTrackInterface::GetSignalLevel` is the obvious way to ask, and the source of a *local* track never implements it.
It answers `false` for every call, so anyone building an indicator on top of it gets silence.

**Fix.** The local level now comes from `RTCAudioSourceStats::audio_level`, which is where libwebrtc actually publishes it.
The bar is also read in decibels: normal speech sits near a twentieth of full scale, and on a linear scale it would barely lift off the floor.

## 3. Two copies of libsrtp, and a call that carried nothing

The most expensive one, and it reproduces in loopback with no network anywhere near it.

**Symptom.** ICE connected, DTLS completed, the SDP was correct on both sides, the screen was captured at 30 fps and OpenH264 encoded it — and the SFU received zero audio and zero video packets.
Everything a log usually shows was healthy. The client simply put nothing on the wire.

**Cause.** With `DV_WEBRTC_LOG=warning`, libwebrtc says it in three lines:

```text
(srtp_session.cc:115): Failed to init SRTP, err=2
(srtp_transport.cc:294): The params in SRTP transport are reset.
(dtls_srtp_transport.cc:199): DTLS-SRTP key installation for RTP failed
```

`srtp_session.cc:115` is `srtp_init()`, and `err=2` is `srtp_err_status_bad_param`, which that function returns from one place: `srtp_crypto_kernel_load_debug_module`, when a module of that name is already registered.
libwebrtc carries its own copy of libsrtp and libdatachannel links another; both export the symbols, and a static link resolves the two to one.
One crypto kernel, and a second `srtp_init()` on it fails by construction.
libdatachannel calls it first, from its global init, on the first WebSocket — which in this client is the signaling connection — and ignores the return value, so it never notices.
libwebrtc calls it second, treats the failure as fatal, and never creates the SRTP session. The rest of the stack carries on as if a call were in progress.

**Fix.** `webrtc::ProhibitLibsrtpInitialization()`, which is the API libwebrtc offers embedders in exactly this position, paired with an explicit `rtc::Preload()` so that libdatachannel initialises libsrtp at a moment we choose rather than at whichever socket happens to come first.
The two tests that had never passed on macOS — audio forwarded between two clients, and a screen share arriving decoded — now pass in under a second each, where before they spent 41 and 31 seconds reaching zero.

**Worth stating plainly:** this is not macOS specific in its cause.
Any platform where both libraries end up in one binary with those symbols exported has the same single crypto kernel.
It went unnoticed because the media job is `workflow_dispatch` and had never run.

## 4. The bundle that asked macOS for a microphone it had not declared

**Symptom.** The application received silence from the microphone on macOS, with no permission dialog shown.

**Cause.** `Info.plist` carried no `NSMicrophoneUsageDescription`.
Without that string the system does not ask the person: it refuses on their behalf.

**Fix.** `assets/Info.plist.in`, configured from `client/CMakeLists.txt`, which keeps the identity keys next to the target properties that set them.

## 5. The SFU deadlock

**Symptom.** The media suite hung until ctest's 180 s limit, in a different case each run, roughly once every three or four rounds.
It was recorded once as "seen and never reproduced" before it was understood.

**Cause.** A lock cycle between a lock of ours and an internal libdatachannel lock, taken in opposite orders by two threads:

```text
on_participant_joined  ->  takes MediaRouter::mutex_, calls addTrack, waits for the PeerConnection lock
forward_audio          ->  arrives holding the PeerConnection lock, waits for MediaRouter::mutex_
```

One RTP packet arriving at the instant someone joins the room is enough, and the whole server stops.
Five participants talking make that window common rather than rare.

**Fix.** An immutable route table: built under `mutex_` and published whole, read through an atomic pointer on the forwarding path.
No libdatachannel callback takes a lock of ours, so the cycle does not exist — and the hot path stops contending on a global mutex for every packet.
Eight consecutive runs of the full suite passed after it.

## 6. The screen share freeze under 5% loss

**Symptom.** The first run of the packet loss test delivered **4 frames in 15 seconds** of shared screen. That is not degradation, it is a freeze.

**Cause.** An intra frame of a 1280x720 screen is over a hundred packets, and at 5% loss the chance of all hundred arriving intact is under 1%.
The only repair the viewer had was to ask for another intra frame, which arrived broken, and the request started over.
The SFU carried eight keyframe requests in fifteen seconds, and none of them produced a picture.

**Fix.** Both ends of retransmission, neither of which existed:

- `rtc::RtcpNackResponder` on the outbound track, which resends the viewer the packet it lost from a cache of recent ones.
- `dv::server::sfu::VideoFeedback` on the inbound track, written here because libdatachannel answers NACKs and never sends one.
  A hole in the sequence becomes a generic RFC 4585 NACK, the sharer resends, and the SFU forwards an already patched stream rather than spreading one loss to every viewer.

The same case then delivered **443 frames in 15 seconds**, with zero keyframe requests.
The numbers are in [benchmarks.md](benchmarks.md), with the caveat that the screen being measured was static.

## 7. One padded RTP packet takes the server down

**Symptom.** The server aborts.

**Cause.** libdatachannel has an `assert` that fires while building the sender report for a padded packet.
Any participant can send one, so a single packet ends everyone's call.

It is also what blocks half of the bandwidth estimation: the viewer reporting how much its own link can take requires libwebrtc to negotiate the `abs-send-time` header extension, negotiating it makes libwebrtc probe bandwidth, and probing means sending padding.
The extension stays out until that is resolved upstream.

**Fix.** The SFU drops padded packets instead of forwarding them.
Found in the section 17 security review, where it was one of the two high severity findings; the other was the password hash without cost.

## 8. The server took UDP ports the firewall did not open

**Symptom.** The first call between two machines over a server on the public internet: no voice and no screen in either direction, while the room, the participant list and every signaling message worked.

**Cause.** The server was started without `--ice-port-range`, which leaves libdatachannel asking the system for an ephemeral port per participant — on Linux, 32768 to 60999 — while the host firewall opened the documented `50000:50100/udp` and nothing else.
The startup log said so, in a line phrased as a note, and a note is what it was read as.

**Fix.** That line is a warning now, and it states the consequence.
On any machine with a firewall in front of it, the setting is the difference between a call and a room where nobody hears anybody.

## 9. NVENC spending 1795 kbps on a screen that does not move

**Symptom.** The hardware encoder was genuinely running — the RTX 4050 encoded and the far side decoded — and the first CPU measurement showed **no saving at all**: 77.1% of one core against OpenH264's 78.9%.

**Cause.** The bitrate, not the encoding.
Against a static screen OpenH264 spends 9 kbps and NVENC was spending 1795: what the card saved on encoding was spent again packetizing and sending eight times more RTP.
CBR fills the target regardless of what the image is doing, which contradicts the design where the number the SFU sends over REMB is a ceiling and not a quota.

**Fix, in two steps, because the first was not enough.**
VBR on its own went to 3137 kbps and fell slowly to 1371: with no quantizer floor the controller has no reason to stop, and keeps lowering the quantizer until it spends the target, encoding a still image ever closer to lossless.
With VBR *and* a QP floor of 24, the static screen came to cost 2 kbps and CPU fell to 69.5%.

Two caveats on those numbers: the measurement is of a static screen, and the QP floor caps maximum quality, so the gain under real motion is a prediction rather than a measurement.

## 10. Media Foundation doing the same, plus writes that did not stick

The same bug in another currency, found on Windows in release v0.1.13 by forcing the backend with `DV_HARDWARE_ENCODER=mediafoundation`.

**Symptom.** 720p30 with the screen parked: Media Foundation sent between 1.5 and 2.9 Mbps, gluing itself to whatever ceiling congestion control allowed, where NVENC on the same screen sent 25 kbps.
In a room of five that is the whole link, for an image that does not move.

**Cause, in two halves, and only the first was already understood.**

- The mode: `eAVEncCommonRateControlMode_CBR`, which fills the target whatever the image does. Now `PeakConstrainedVBR`, with CBR back if the encoder refuses.
- The second half only appeared by instrumenting `ICodecAPI`: **writes made after `SetOutputType` do not take**. The transform answers success and carries on with its own defaults. Read back on the test machine, a session configured for 1500 kbps was draining a 6912 kbps bucket with no floor under the quantizer. Mode, bitrate, bucket size and QP floor all moved to before the output type, which is where the transform reads them.

The floor is `CODECAPI_AVEncVideoMinQP` at 24 — the same number and the same reason as NVENC: it is where libwebrtc's quality scaler puts the good end of the H.264 range.

The peak is the one value that cannot be corrected afterwards, which is why it is not set equal to the mean as it is on NVENC.
There the mean is rewritten on every estimate; here a peak equal to the first permission would pin the share to the bitrate the call started at.
So the peak is the ceiling the user configured, and the mean — which the transform accepts hot — is what follows congestion control, verified by writing and reading it back once a second.

**Measured after, same screen, same 80 seconds, same path:**

| | |
| --- | --- |
| release v0.1.13 | 235 to 2102 kbps, never below 235 |
| with the fix | 21 to 31 kbps parked, around 980 kbps with the screen moving |

The log line now says which mode the encoder ended up in, because which one the transform accepted is invisible in a log somebody sends afterwards.

## 11. 272 unit tests absent, and the suite green

**Symptom.** The first run of the suite on Windows reported **63 tests passed out of 63**, and meant it.

**Cause.** vcpkg's dynamic triplet builds `gmock.dll` with its own copy of GoogleTest linked in: it does not depend on `gtest.dll` and exports `MakeAndRegisterTestInfo` itself, while `gtest_main.dll` does depend on `gtest.dll`.
`dv_unit_tests` linked both, so every `TEST()` registered into the registry inside `gmock.dll` and `main()` read the one inside `gtest.dll`.

That is not a link error and not a crash.
It is an executable that starts, prints *"this test program does NOT link in any test case"*, and exits 0.
`ctest` treats only *no tests at all* as a failure, and the integration binary — which never linked gmock — kept the count above zero.
A green Windows CI job would have covered nothing but integration for as long as anyone cared to look.

**Fix.** gmock was there for one `EXPECT_THAT` in `unit/test_video_feedback.cpp`, which now sorts and compares like the two assertions above it.
The suite runs 335 tests.

## 12. The installer shipped a program that could not start

**Symptom.** The MSI installed, and the client did not run.

**Cause.** `cmake --install` installs the target and nothing else, and `windeployqt` knows only about Qt.
The staged tree held `partyshare.exe`, the Qt runtime, and none of `spdlog.dll`, `fmt.dll` or `datachannel.dll`.
On Windows there is no rpath and no package manager: those files sit next to the executable or the program does not start.

The release job's own check did not notice, because it looks for the executable and the Qt platform plugin, and both were there — the same failure the job's comment warns about two steps earlier, in the shape it did not anticipate.

**Fix.** `client/CMakeLists.txt` installs a `RUNTIME_DEPENDENCY_SET`, which resolves the transitive half as well. The MSI went from 23 files to 30.

Two smaller things in the same rule, both of which fail silently:

- The Qt directory has to be in `DIRECTORIES` even though `windeployqt` handles Qt afterwards. Excluding Qt by name leaves it *unresolved*, and an unresolved dependency stops the install rather than being skipped.
- The pattern that drops the system libraries has no backslash in it on purpose. A backslash has to survive CMake's string parsing before it reaches the regex engine, and the version that did not matched nothing — quietly staging every DLL in `System32`, and through `shell32` going looking for `HvsiFileTrust.dll`, which exists on no machine.

## 13. Two things `.gitignore` quietly deleted from the build

**The libwebrtc patch was never committed.**
`.gitignore` matched `build/` at any depth, which includes `patches/webrtc/build/`, so the patch this repository documents for the Linux source build was refused by `git add` without a word.
`scripts/build_webrtc.sh` passes two GN arguments that only that patch declares, and GN answers an argument it does not know with a warning and a zero exit rather than an error.

So a build from a fresh clone does not fail. It succeeds and produces the **wrong library**: linked against whatever libstdc++ the machine has instead of the pinned one, with the CREL relocations left on that force every consumer to link with lld.
It worked only where the file happened to sit untracked, and nowhere else, quietly.
The rule is anchored to the root now.

**`third_party/nvcodec` was not versioned**, because all of `third_party/` was ignored.
A clean clone did not compile NVENC, which means the hardware encoder worked on one machine and on no other.
That header is vendored source with its provenance and licence recorded alongside, not a downloaded dependency.

## 14. The first AppImage came up with no media layer

The packaging script did not pass `DV_BUILD_CLIENT_MEDIA`.

A client that opens the window, shows the login screen and makes no calls is worse than no artifact at all, because it looks like the product.
The release smoke test refuses that now, by looking for the phrase in the log.

## 15. The AppImage that will not start on Ubuntu

The AppImage carries Qt and the C++ runtime, and does not carry glibc, which cannot be bundled.
One built on the development machine — Arch, glibc 2.44 — **does not start on a clean Ubuntu 24.04**: it asks for `GLIBC_2.43` and `GLIBC_2.44`, and 24.04 has 2.39.

That makes a locally built AppImage a development artifact rather than a distribution one, and it is why the release job uses the oldest runner available rather than the newest, which is the wrong instinct.
The script prints the glibc floor of the file it just produced, because it is an invisible property until somebody cannot open the program.

## 16. Three workflow defects, one tag each

The workflows are the part of this repository that cannot be run before it is merged, and the release ones cannot be run outside a tag.
`actionlint` runs in CI for that reason, and in its first two runs it found three defects, none of which shows up in a YAML review and all three of which would have surfaced on the first tag:

- The signing guard read the `secrets` context, which a step level `if:` cannot see. The condition would always be false and nothing would be signed, silently. The secrets moved up to the job level, where `env` is visible.
- The `publish` condition sat in a folded `>` block, which leaves a trailing `\n` and turns the expression into a non empty string, which is truthy. It would publish on every push, tag or no tag.
- `macos-13` no longer exists as a runner label. The x86_64 job would fail at scheduling time.

## 17. Smart App Control and an unelevated `msiexec`

Two failures that happened at the same time and were read as one, which cost an afternoon.

**Smart App Control blocks a new binary once.**
The machine has it in enforcement, the default on a clean Windows 11, and it did block the client: the Code Integrity log carries 3033, 3077 and 3118 events naming the staged executable, and the program refused to start with nothing on screen.
Some minutes later the same file, unchanged, started normally.
The block is not a verdict on an unsigned binary — it is the wait while Microsoft's app intelligence service decides about a file it has never seen. It costs a first run, not the artifact.

**The installer failing was a different thing.**
`msiexec /i /qn` was being run from a shell that was not elevated.
The second attempt said so outright — error 1303, *"The installer has insufficient privileges to access this directory: C:\Program Files\PartyShare"* — and the first had left the same fingerprint in a subtler form: error 1310 on the first file it could not write, with the ones before it already on disk.
Run elevated, the same MSI returns 0, installs its thirty files, writes both shortcuts, and the client opens on its login screen from `C:\Program Files`.

**And the signing step would not have been enough either**, which is a third defect found on the way to this one.
It signed `stage\bin\*.exe`, which is two files: the client, and the Visual C++ redistributable that Microsoft already signed and that would have had its publisher replaced by ours.
Of the thirty files in the install tree, twenty-two arrive signed by Microsoft or by Qt and eight do not — and seven of those eight, `datachannel.dll` among them, are libraries that glob never reached.
Smart App Control checks every binary as it is loaded and not only the one that was started, so signing the executable and shipping seven unknown libraries beside it buys nothing.
The step now covers every unsigned `.exe` and `.dll`, skips what already carries somebody else's signature, checks `signtool`'s exit code — a native program that fails does not stop a `pwsh` step by itself — and refuses to finish while anything in the tree is still unsigned.

Two things about the policy stay true, because a certificate that satisfies neither signs without complaint and changes nothing: Smart App Control accepts only certificates from a CA in the Microsoft Trusted Root Program, so a self-signed one is no better than none, and its check does not read ECC at all, so the certificate has to be RSA.

## 18. The first MSVC build

What was expected to hurt did not: the code compiles under MSVC 19.44 with `/W4 /permissive- /WX` without a single warning, and nothing in it needed an `#ifdef`.
The defects were all in the parts nobody had been compiling.

**In the toolchain:**

- `dwmapi` was missing from the Windows library list in `cmake/Findlibwebrtc.cmake`, which `RTC_ENABLE_WIN_WGC` two lines above it requires. Everything else resolved and the link died on `DwmGetWindowAttribute` alone.
- The published `webrtc.lib` is built against the **static** C runtime while everything else defaults to the dynamic one, so the link fails with `LNK2038` across the whole standard library. Free for the spike, which builds its two dependencies from source in the same pass; expensive for the client, whose vcpkg dependencies would all have to move to `x64-windows-static`. The way out was a source build, and GN ties the dynamic CRT to `is_component_build` — which would ship the library as DLLs and put the installer near 90 MB instead of 65. `patches/webrtc/build/0002-win-dynamic-crt.patch` separates the two decisions, and is off by default.

**In `client/src/webrtc`, compiled with MSVC for the first time — four defects, none of them a warning, all four stopping the build:**

- `dlfcn.h` in the NVENC encoder, now off on Windows.
- `CreateClientTcpSocket` in two shapes, because the prebuilt and source trees disagree about a method signature while both calling themselves m152. Measured by a two line probe at configure time.
- `libyuv` in a different place in each tree.
- A missing `#include <array>` in `test_benchmark.cpp`, latent for as long as the file has existed, because libstdc++ brings it in by transitivity and the MSVC standard library does not.
