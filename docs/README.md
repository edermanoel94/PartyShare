# The PartyShare book

Everything written about this project, in one order, with one entry point.

Each chapter answers a different question, and they are numbered so that reading
them front to back takes you from "what is this" to "why is this code like this".
Nothing here is duplicated: a subject lives in exactly one chapter, and the other
chapters link to it.

## Start here

| | Document | Read it when |
| --- | --- | --- |
| | [README.md](../README.md) | You want to know what PartyShare is in two minutes |
| | [INSTALL.md](../INSTALL.md) | You want it built and running, on a server or on your own machine |

## Chapters

| | Chapter | Subject |
| --- | --- | --- |
| 1 | [Overview](01-overview.md) | The three pieces, how a call is put together, what the platform status actually is |
| 2 | [Build](02-build.md) | Toolchain, presets, CMake options, the media layer, static analysis |
| 3 | [Configuration](03-configuration.md) | Every knob: files, environment, command line, and which wins |
| 4 | [Server and database](04-server-and-database.md) | Running the server, MongoDB, ports and firewalls |
| 5 | [Administration](05-administration.md) | Roles, restrictions, the audit log, and the two ways to apply them |
| 6 | [Signaling protocol](06-protocol.md) | The normative definition of the wire protocol |
| 7 | [libwebrtc toolchain](07-webrtc-toolchain.md) | Why a custom libwebrtc build exists, and how to rebuild it |
| 8 | [libwebrtc validation](08-webrtc-validation.md) | How to validate that toolchain on a new platform |
| 9 | [Shared screen audio](09-screen-audio.md) | Sending the sound of the shared machine, and what that design costs |
| 10 | [Join and leave alerts](10-join-leave-alerts.md) | The notification and the chime, and why they take turns |
| 11 | [Benchmarks](11-benchmarks.md) | Latency, CPU, memory, and behaviour on an impaired network |
| 12 | [Hardware requirements](12-requirements.md) | What a machine needs to run the client and the server |
| 13 | [Security review](13-security.md) | The section 17 review, and what is still open |
| 14 | [Release](14-release.md) | Cutting a release, and what each platform produces |
| 15 | [Post-mortems](15-postmortems.md) | The bugs that cost real time, and what each turned out to be |

## Reference

Kept outside the numbered chapters because they are consulted rather than read.

| Document | Subject |
| --- | --- |
| [SPEC.md](../SPEC.md) | The product specification and the acceptance criteria. **Normative**, and cited by section number from across the tree |
| [PLAN.md](../PLAN.md) | The milestone record, the decisions behind it, and what is still not verified |
| [tools/dbadmin/README.md](../tools/dbadmin/README.md) | The terminal front end for accounts, restrictions and the audit log |

## Suggested paths

**I want to run it.** [INSTALL.md](../INSTALL.md), then chapter 4 for the server
and chapter 3 for pointing the client at it.

**I want to change it.** Chapter 1, then chapter 2 to build, then chapter 6 if
the change crosses the wire. Chapter 15 before touching the media layer.

**I want to operate it.** Chapters 4, 5 and 12, plus chapter 13 for what the
system does and does not promise.

**I want to know why.** [PLAN.md](../PLAN.md) section 3, chapter 15, and chapter
7 for the decision that shaped everything after it.
