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
| Windows x64 | Built, run and packaged on Windows 11 with MSVC 19.44, Qt 6.7.3 and vcpkg: all 335 tests pass, the server listens, and the MSI installs, writes its start menu and desktop shortcuts, and the client it installs opens on its login screen. No media layer, because libwebrtc is not built there. Two defects came out of the first build, both recorded in M9. |
| macOS ARM64 | Built and tested on macOS 26 with Apple clang 21, Qt 6.11 and vcpkg: all 277 tests pass. The server was driven end to end over the signaling protocol, and the client starts on its login screen, which is as far as an automated check goes without a person at the keyboard. No media layer, because libwebrtc is not built there, and no DMG was produced. |
| macOS x64 | Code, presets and DMG packaging exist. Never built, never run. |

Two known gaps, both in M3:
repeating the screen capture validation on a Wayland session,
and running the libwebrtc spike on macOS following [docs/webrtc-validation.md](docs/webrtc-validation.md).
The Windows half of that second gap is closed: the spike passes there, and what it cost is in
[docs/webrtc-toolchain.md](docs/webrtc-toolchain.md).

## How it works

Three pieces, with a clear boundary between them.

The **client** (`client/`) captures the screen and audio, speaks WebSocket to the signaling server and WebRTC to the SFU.
The UI layer depends on `app`, and `app` depends on the media and network modules.
No media or network module includes a Qt header, and that rule is checked in code review.

The **server** (`server/`) is two halves in one process.
Signaling routes JSON messages over WebSocket and owns the room lifecycle.
The SFU forwards RTP between participants without decoding anything.

Accounts carry a **role**, either `user` or `admin`.
A user joins and creates rooms and shares a screen; an administrator can also remove and mute other participants, and manage the accounts and the rooms from a panel in the client.
Every administrative action is checked on the server, against the role read from the account store at that moment rather than the one the session logged in with, and every one of them is written to an audit log.
Section 4.6 of [docs/protocol.md](docs/protocol.md) is the normative list.

Accounts, roles, persistent rooms and the audit log are persisted in **MongoDB**, which is optional at build time.
Without it the server keeps all four in memory and behaves as it did before persistence existed.

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
Without Qt, pass `-DDV_BUILD_CLIENT_UI=OFF`, which drops the interface and keeps the client core, or `-DDV_BUILD_CLIENT=OFF`, which drops the client entirely.
Without libdatachannel or OpenSSL, pass `-DDV_BUILD_SERVER=OFF`.

MongoDB persistence is off by default and needs both a CMake option and the vcpkg feature that brings the driver:

```sh
cmake -S . -B build/mongo -DDV_ENABLE_MONGO=ON -DVCPKG_MANIFEST_FEATURES=mongo
```

Without it the whole suite still builds and runs, and the server keeps its accounts and rooms in memory.

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

`partyshare-server --help` lists every option the server accepts, and `partyshare --help` does the same for the client.
Options take the `--key=value` form, with the value attached.

The server refuses an option it does not recognise, including a bare `--key` with the value detached.
A server is started by a script nobody is watching, and a typo that is quietly ignored leaves it listening on a default nobody chose.
The client cannot do the same, because Qt reads its own options from that command line, so it passes anything it does not recognise on to Qt.

With a database, and an administrator to start from:

```sh
./build/mongo/bin/partyshare-server \
  --database-uri=mongodb://127.0.0.1:27017 --create-admin=ana:choose-a-password

./build/mongo/bin/partyshare-server \
  --database-uri=mongodb://127.0.0.1:27017 --port=8080
```

`--database-uri` turns the database on by itself, so there is no second switch that has to agree with the first.
A build without `-DDV_ENABLE_MONGO` refuses to start when the database is on rather than falling back to memory, because a server that was told to persist and quietly did not is one whose accounts disappear at the next restart.

`--create-admin` creates that administrator, or promotes an existing account and resets its password, and then exits.
It is also the way back in when the only administrator password is lost.
The password is visible in `ps` while the command runs, so it is worth changing from the client afterwards.

The same is true of a URI that carries a password, which is one reason to supply it through the environment or the config file instead:

```sh
export DV_DATABASE_ENABLED=1
export DV_DATABASE_URI="mongodb://ana:password@127.0.0.1:27017/?authSource=admin"
export DV_DATABASE_NAME=partyshare
```

```json
{
  "database": {
    "enabled": true,
    "uri": "mongodb://127.0.0.1:27017",
    "name": "partyshare",
    "timeout_ms": 2000
  }
}
```

Neither of those turns the database on by naming a URI, unlike the command line option, so `enabled` has to be set as well.
`timeout_ms` defaults to 2000 and is deliberately short: the store is called with the server's lock held, so the driver's own default of thirty seconds would not fail one login, it would hold every call on the server for half a minute.
Writing the configuration back out replaces the credentials in the URI with asterisks, so dumping it is not a way to read the password.

[tools/dbadmin](tools/dbadmin/README.md) does the same job from a terminal, and more of it: it lists, creates, edits and removes accounts, sets passwords, and reads the audit log, against the database and without a running server.
It writes the same documents and the same audit entries the server writes.

`--users-file` points at a list of development accounts:

```json
[
  {"username": "ana", "password": "test-password", "display_name": "Ana"}
]
```

Each entry also takes an optional `"role"`, `"admin"` or `"user"`, and anything else reads as `"user"`.

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

The MongoDB tests are a label of their own and need a database to point at.
Without `DV_TEST_MONGO_URI` each of them skips itself rather than failing, so a machine with no MongoDB still runs the whole suite:

```sh
docker run -d -p 27017:27017 --name partyshare-mongo mongo:7
DV_TEST_MONGO_URI=mongodb://127.0.0.1:27017 ctest --test-dir build/mongo -L mongo
```

They leave behind databases named `partyshare_test_*`, one per test and per run, which are safe to drop.

The integration tests start a real server on an ephemeral port and connect real WebSocket clients.
The media tests need audio, and `scripts/virtual_audio.sh` creates a virtual sound card for machines and CI runners without one.

CI runs clang-format, clang-tidy, cppcheck, workflow linting, and the whole suite under AddressSanitizer and UndefinedBehaviorSanitizer.

## Releases

A `vx.y.z` tag triggers `.github/workflows/release.yml`, which builds, tests and publishes.
No artifact is ever built on a development machine, for the simple reason that a binary published from a laptop is a binary nobody can reproduce afterwards.
A tag produces an AppImage on Linux, an MSI installer and a zip of the client on Windows, DMGs for both macOS architectures, and a `SHA256SUMS` over whatever was produced.
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
├── tools/      # dbadmin, a terminal front end for the database
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
| [docs/protocol.md](docs/protocol.md) | The normative definition of the signaling protocol, roles included |
| [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md) | Why a custom libwebrtc build exists, and how to rebuild it |
| [docs/webrtc-validation.md](docs/webrtc-validation.md) | How to validate the toolchain on a new platform |
| [docs/benchmarks.md](docs/benchmarks.md) | Latency, CPU, memory, and behaviour under an impaired network |
| [tools/dbadmin/README.md](tools/dbadmin/README.md) | The terminal front end for the accounts and the audit log |
| [docs/security-review.md](docs/security-review.md) | The section 17 review, and what remains open |
| [docs/release.md](docs/release.md) | Cutting a release, and what each platform produces |
