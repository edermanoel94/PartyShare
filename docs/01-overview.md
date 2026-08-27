# 1. Overview

PartyShare is screen sharing and voice for a room of up to five people on the
desktop. A C++20 codebase: a Qt 6 client, and a signaling server with its own
SFU. Media is WebRTC end to end - Opus for audio, H.264 for the screen,
DTLS-SRTP over both.

## The three pieces

Three parts, with a boundary between them that is enforced rather than intended.

**The client** (`client/`) captures the screen and the microphone, speaks
WebSocket to the signaling server and WebRTC to the SFU. The `ui` layer depends
on `app`, and `app` depends on the media and network modules. **No media or
network module includes a Qt header**, and that rule is checked in code review.

**The server** (`server/`) is two halves in one process. Signaling routes JSON
messages over WebSocket and owns the room lifecycle. The SFU forwards RTP
between participants without decoding anything, which is why it spends bandwidth
and almost no processor.

**The shared code** (`shared/`) holds the protocol, the models and the
configuration. The protocol is defined in [chapter 6](06-protocol.md), and the
C++ implementation follows that document rather than the other way around. That
is what makes it possible to reimplement the server in another language without
touching the clients.

## How a call is put together

The server is always the offerer. A participant only answers, which keeps mids,
SSRCs and payload types under the SFU's control and reduces forwarding to
rewriting a header instead of translating between two independent negotiations.
The reserved identifier `sfu` is the address of that media endpoint: a message
addressed to it is consumed by the server rather than relayed.

Every participant's video m-lines exist from the moment they join, before anyone
asks to share: one `recvonly` carrying their screen upstream, one `sendonly`
bringing down the screen of whoever holds the floor. Starting and stopping a
share therefore renegotiates nothing. It costs one idle m-line per participant,
which costs nothing on the wire.

The SFU decides the share bitrate, because only it sees both links - the sender
sees its own upstream, the viewer its downstream, the server both. The number
travels as REMB, and libwebrtc treats it as a ceiling for its own congestion
controller. The measurements are in [chapter 11](11-benchmarks.md).

## Rooms, chat and roles

A **room** is six uppercase hexadecimal characters, short enough to read out
loud. A room also carries a name; one created without a name is named after its
own identifier, so a name is never empty. Two rooms may not wear the same name.

An ordinary room is deleted the moment it empties. A **persistent** room outlives
its last participant, so its identifier keeps working across restarts. Only an
administrator may create one.

Each room has a **chat**, kept by the server and replayed to whoever joins, so
that somebody arriving late reads what was already said. A room's conversation
lives exactly as long as its room - that is a correctness rule and not
housekeeping, because room identifiers are handed out again.

Accounts carry a **role**, `user` or `admin`, and a set of **restrictions**:
whether the account may sign in, use a microphone, write in the chat, or share a
screen. Restrictions outlast the room, the session and the process, which is what
separates them from a kick and a forced mute. [Chapter 5](05-administration.md)
is the whole subject.

## Persistence

Accounts, roles, persistent rooms, conversations and the audit log are persisted
in **MongoDB**, which is optional at build time. Without it the server keeps all
of them in memory and loses them when it stops.

MongoDB is the intended deployment shape for a server anyone else logs into.
[Chapter 4](04-server-and-database.md) covers it, and [INSTALL.md](../INSTALL.md)
is the path from nothing to a running one.

## Stack, and why

| Area | Choice | Reason |
| --- | --- | --- |
| Language, build | C++20, CMake + presets + Ninja | Set by the SPEC |
| UI | Qt 6 Widgets | The interface is dense in controls and needs no heavy animation |
| Client WebRTC | libwebrtc, built from source | Brings AEC3, noise suppression, AGC, jitter buffer, congestion control, SRTP, ICE and `modules/desktop_capture` ready made |
| Screen capture | `webrtc::DesktopCapturer` | Already covers Windows Graphics Capture, DXGI, ScreenCaptureKit, X11 and PipeWire |
| Video codec | H.264 via OpenH264, hardware encoders behind `webrtc::VideoEncoderFactory` | Cross platform with no GPU dependency, and NVENC or Media Foundation when a card answers |
| SFU | libdatachannel | Small, compiles anywhere, does packet level RTP forwarding. libwebrtc is not built to forward media N to N |
| Signaling | WebSocket + JSON | The WebSocket comes from libdatachannel, so the server has one network stack instead of two |
| Persistence | MongoDB, optional at build time | Accounts, roles, persistent rooms, chat, audit log |
| Dependencies | vcpkg in manifest mode | The same versions on three platforms and in CI. libwebrtc stays outside it, as an external binary tree |

The libwebrtc decision is the one that shaped the rest, and it is the subject of
[chapter 7](07-webrtc-toolchain.md).

## Platform status

Stated plainly, because a number nobody measured is worth no more than a blank
space.

| Platform | Status |
| --- | --- |
| Linux x64 | Built, run and measured. Every number in this book comes from here |
| Windows x64 | Built, run and packaged on Windows 11 with MSVC 19.44, Qt 6.7.3 and vcpkg. The media layer builds and runs over a source build of libwebrtc, and the media suite passes; the virtual audio device test skips, because its script is Linux only. The MSI installs and the client it installs opens |
| macOS ARM64 | Built and run, media layer included, over a source build of libwebrtc. 22 of 25 media tests pass, one skips for want of a virtual device, and two fail on the audio device rather than on transport. The `.dmg` the pipeline publishes still ships a client that cannot make a call |
| macOS x64 | Code, presets and DMG packaging exist. Never built: the matrix entry is commented out because Homebrew's OpenSSL headers in `/usr/local` reach the compiler ahead of vcpkg's, and `-Wold-style-cast -Werror` kills the build |

Development and *release* are two different questions, and this table answers the
first. What a tagged release actually carries — three files, with Linux and macOS
x64 switched off — is in [chapter 14](14-release.md).

One capture gap remains: screen capture on a Wayland session, through the XDG
portal. Everything not yet verified is listed in section 4 of
[PLAN.md](../PLAN.md).

## Layout

```text
PartyShare/
├── shared/     # protocol, models, configuration, logging
├── client/     # app, ui (Qt 6), video, audio, network, webrtc, media
├── server/     # signaling, rooms, sfu, store
├── tests/      # unit, integration
├── scripts/    # libwebrtc build, AppImage, netem, virtual audio
├── tools/      # dbadmin, and the two spikes
├── cmake/      # Findlibwebrtc, warnings, sanitizers, packaging
├── patches/    # fixes applied on top of the libwebrtc source
├── assets/     # icons, sounds, config.ini, .desktop entry, Windows resource
└── docs/       # this book
```
