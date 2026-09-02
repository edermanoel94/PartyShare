# PartyShare

Screen sharing and voice rooms for small groups on the desktop.

A C++20 application: a Qt 6 client, and a signaling server with its own SFU.
Media is WebRTC end to end — Opus for audio, H.264 for the screen, DTLS-SRTP over
both. The target is a room of a few people - five by default, sized when it is
created - sharing a screen at 720p and 30 FPS, with audio latency below what
anyone notices in conversation.

## Quick start

```sh
./scripts/ci_vcpkg.sh                       # pinned vcpkg + toolchain file
cmake --preset linux-release -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset linux-release
ctest --preset linux-release
```

Binaries land in `build/<preset>/bin/`. [INSTALL.md](INSTALL.md) is the full
path — a MongoDB-backed server on one machine, a client on another, and two
people in a room.

For the server alone, on Debian or Ubuntu, one command installs MongoDB, downloads
the server from the release and leaves it running as a service:

```sh
sudo scripts/install_server.sh --admin=ana
```

## The pieces

| | |
| --- | --- |
| `partyshare-server` | Signaling over WebSocket, plus an SFU that forwards RTP without decoding it |
| `partyshare` | The desktop client: capture, playback, and the interface |
| `tools/dbadmin` | Terminal front end for accounts, restrictions and the audit log, with no server running |

Accounts, roles, persistent rooms, conversations and the audit log live in
**MongoDB**. It is optional at build time; without it the server keeps all of
them in memory and loses them when it stops.

Accounts carry a role — `user` or `admin` — and a set of restrictions: whether
the account may sign in, use a microphone, write in the chat, or share a screen.
Every administrative action is checked on the server against the account store at
that moment, not against what the session logged in with, and every one of them
is written to an audit log.

## Status

| Platform | Development | A tagged release ships |
| --- | --- | --- |
| Linux x64 | Built, run and measured. Every number in the documentation comes from here | **nothing** — build from source |
| Windows x64 | Built, run and packaged. Media layer included | `.msi` and `.zip`, installed and run |
| macOS ARM64 | Built and run, media layer included | `.dmg`, which still ships a client that **cannot make a call** |
| macOS x64 | Code, presets and packaging exist. Never built | **nothing** — the build fails on the Intel runner |

Windows is the one platform where a downloaded release works. The other three
each have a named blocker, and [chapter 14](docs/14-release.md) says what each
one is.

Two gaps remain open in the product itself: screen capture on a Wayland session,
and audio latency measured on a real network rather than on loopback. Everything
not yet verified is listed in section 4 of [PLAN.md](PLAN.md).

## Documentation

Everything is in **[the book](docs/README.md)**, fifteen chapters in reading
order. The ones most people want first:

| | |
| --- | --- |
| [Install](INSTALL.md) | From nothing to two clients in a room |
| [Overview](docs/01-overview.md) | The three pieces and how a call is put together |
| [Build](docs/02-build.md) | Presets, options, the media layer |
| [Configuration](docs/03-configuration.md) | Every setting, and which copy wins |
| [Server and database](docs/04-server-and-database.md) | Running it, MongoDB, ports and firewalls |
| [Protocol](docs/06-protocol.md) | The normative wire definition |

[SPEC.md](SPEC.md) is the product specification and is normative.
[PLAN.md](PLAN.md) is the milestone record and the decisions behind it.

## Releasing

A push to `master` cuts a release: the workflow works out the next version, tags
it, and publishes. No artifact is ever built on a development machine — a binary
published from a laptop is one nobody can reproduce afterwards.

A tag carries three files and a checksum list — the Windows `.msi` and `.zip` and
the macOS arm64 `.dmg`. The Linux and macOS x64 jobs are switched off, so the run
is green with two artifacts missing and nothing in its output says so.
[Chapter 14](docs/14-release.md) is the procedure, and the reason for each.
