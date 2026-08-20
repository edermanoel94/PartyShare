# PartyShare

Screen sharing and voice rooms for small groups on the desktop.

A C++20 application with a Qt 6 client and a signaling server with its own SFU.
Media is WebRTC end to end: Opus for audio, H.264 for the screen, DTLS-SRTP over both.
The target is a room of up to five people, sharing a screen at 720p and 30 FPS, with audio latency below what anyone notices in conversation.

## Status

The MVP is close to done: the ten milestones in [PLAN.md](PLAN.md), M0 through M9, have all been executed.
What is missing is stated plainly, because a number nobody measured is worth no more than a blank space.

| Platform | Status |
| --- | --- |
| Linux x64 | Built, run and measured. Every number in the documentation comes from here. |
| Windows x64 | Code, presets and the NSIS installer exist. Never built, never run. |
| macOS ARM64 and x64 | Code, presets and DMG packaging exist. Never built, never run. |

Two known gaps, both in M3:
repeating the screen capture validation on a Wayland session,
and running the libwebrtc spike on Windows and macOS following [docs/webrtc-validation.md](docs/webrtc-validation.md).

## How it works

Three pieces, with a clear boundary between them.

The **client** (`client/`) captures the screen and audio, speaks WebSocket to the signaling server and WebRTC to the SFU.
The UI layer depends on `app`, and `app` depends on the media and network modules.
No media or network module includes a Qt header, and that rule is checked in code review.

The **server** (`server/`) is two halves in one process.
Signaling routes JSON messages over WebSocket and owns the room lifecycle.
The SFU forwards RTP between participants without decoding anything.

The **shared code** (`shared/`) holds the protocol, the models and the configuration.
The protocol is defined in [docs/protocol.md](docs/protocol.md), and the C++ implementation follows that document rather than the other way around.
That is what makes it possible to reimplement the server in another language without touching the clients.

## Building

Prerequisites: CMake 3.25, Ninja 1.11, a C++20 compiler, and Qt 6.5 for the client.
The server also needs libdatachannel and OpenSSL, neither of which is header only.

```sh
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

Binaries land in `build/<preset>/bin/`.
Without Qt, pass `-DDV_BUILD_CLIENT=OFF`.
Without libdatachannel or OpenSSL, pass `-DDV_BUILD_SERVER=OFF`.

Four presets exist for Linux.
Windows has `windows-debug`, `windows-release` and `windows-asan`; macOS has `macos-arm64-debug`, `macos-arm64-release`, `macos-arm64-asan` and `macos-x64-release`.

| Preset | Generator | Build type | Sanitizers |
| --- | --- | --- | --- |
| `linux-debug` | Ninja | Debug | off |
| `linux-release` | Ninja | RelWithDebInfo | off |
| `linux-asan` | Ninja | Debug | AddressSanitizer and UndefinedBehaviorSanitizer |
| `linux-make` | Unix Makefiles | Debug | off |

Every preset except `linux-make` generates for Ninja, so without `ninja` on the path they fail at configure time with `unable to find a build program corresponding to "Ninja"`.
Install Ninja, or use `linux-make`, which is there for exactly that case.

The client builds without libwebrtc by default, and in that mode the interface and signaling work but there is no call.
Building with media requires the tree that `scripts/build_webrtc.sh` produces:

```sh
cmake -S . -B build/media \
  -DDV_BUILD_CLIENT_MEDIA=ON \
  -DDV_WEBRTC_ROOT=$HOME/.cache/partyshare/webrtc/dist
cmake --build build/media
```

Why that separate libwebrtc build exists is covered in section 5 of [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md).
Everything else, including presets, CMake options, debugging environment variables, virtual audio and network impairment, is in [docs/build.md](docs/build.md).

## Running

The server first:

```sh
./build/linux-release/bin/partyshare-server \
  --config=config.json --port=8080 --log-level=debug \
  --users-file=dev-users.json
```

`--users-file` points at a list of development accounts:

```json
[
  {"username": "ana", "password": "test-password", "display_name": "Ana"}
]
```

That file stores passwords in plain text and exists only so the MVP has users.
Section 17 of the SPEC forbids it in production, and the server logs a warning on every startup.
Before any deployment the hash has to become Argon2id and the accounts have to come from a real user store.

Then the client:

```sh
./build/linux-release/bin/partyshare
```

Configuration precedence is: built-in defaults, then the config file, then environment variables prefixed with `DV_`, then the command line.
The full list of variables lives in `shared/src/config/config.cpp`.

## Tests

```sh
ctest --preset linux-release            # everything
ctest --test-dir build/linux-release -L unit
ctest --test-dir build/linux-release -L integration
```

The integration tests start a real server on an ephemeral port and connect real WebSocket clients.
The media tests need audio, and `scripts/virtual_audio.sh` creates a virtual sound card for machines and CI runners without one.

CI runs clang-format, clang-tidy, cppcheck, workflow linting, and the whole suite under AddressSanitizer and UndefinedBehaviorSanitizer.

## Releases

A `vx.y.z` tag triggers `.github/workflows/release.yml`, which builds, tests and publishes.
No artifact is ever built on a development machine, for the simple reason that a binary published from a laptop is a binary nobody can reproduce afterwards.
A tag produces an AppImage on Linux, an NSIS installer and a zip on Windows, DMGs for both macOS architectures, and a `SHA256SUMS` over whatever was produced.
Only the Linux artifact is verified.
The full procedure is in [docs/release.md](docs/release.md).

## Layout

```text
PartyShare/
├── shared/     # protocol, models, configuration, logging
├── client/     # app, ui (Qt 6), video, network, webrtc, media
├── server/     # signaling, rooms, sfu
├── tests/      # unit, integration
├── scripts/    # libwebrtc build, AppImage, netem, virtual audio
├── cmake/      # Findlibwebrtc, warnings, sanitizers, packaging
├── patches/    # fixes applied on top of the libwebrtc source
├── assets/     # icons, .desktop entry, Windows resource
└── docs/
```

## Documentation

| Document | Subject |
| --- | --- |
| [SPEC.md](SPEC.md) | The product specification and the acceptance criteria |
| [PLAN.md](PLAN.md) | The milestones, what each one delivered, and the bugs found along the way |
| [docs/build.md](docs/build.md) | Building, presets, options and media debugging |
| [docs/requirements.md](docs/requirements.md) | The hardware needed to run the client and the server |
| [docs/protocol.md](docs/protocol.md) | The normative definition of the signaling protocol |
| [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md) | Why a custom libwebrtc build exists, and how to rebuild it |
| [docs/webrtc-validation.md](docs/webrtc-validation.md) | How to validate the toolchain on a new platform |
| [docs/benchmarks.md](docs/benchmarks.md) | Latency, CPU, memory, and behaviour under an impaired network |
| [docs/security-review.md](docs/security-review.md) | The section 17 review, and what remains open |
| [docs/release.md](docs/release.md) | Cutting a release, and what each platform produces |
