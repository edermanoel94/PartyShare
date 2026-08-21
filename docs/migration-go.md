# Migrating the server to Go

This document is the plan for reimplementing `server/` in Go.
It follows [PLAN.md](../PLAN.md) in shape: decisions with the reason attached, milestones with acceptance criteria that can be run, and the risks stated rather than hoped away.

The client does not change, and that is the whole premise.
[docs/protocol.md](protocol.md) says so already: it is the normative definition, the C++ follows the document rather than the other way around, and section 14 of the SPEC anticipated exactly this.
The migration is the first time that claim is tested, and the plan below is written so that the test comes before the rewrite rather than after it.

---

## 1. Why, and what this is not

The server is 5,208 lines of C++ that need CMake, vcpkg, OpenSSL, libdatachannel and a compiler per platform to produce one binary per platform.
None of that is media processing: the SFU forwards packets without decoding anything, and everything above it is JSON, a hash and four maps.
Go delivers that shape with one static binary, cross compiled from one machine, with `go test -race` over the concurrency and no dependency tree to build first.

What this is not: a redesign.
The topology, the protocol, the roles, the database documents and the bitrate rules stay exactly as they are.
Anything worth changing is worth changing after the migration, in a commit that says so, against a suite that already passes.

---

## 2. Scope

| Component | Today | After |
| --- | --- | --- |
| `server/src/signaling` | C++, libdatachannel WebSocket | Go |
| `server/src/rooms` | C++ | Go |
| `server/src/sfu` | C++, libdatachannel | Go, pion |
| `server/src/store` | C++, mongocxx | Go, reusing `tools/dbadmin/internal/store` |
| `shared/protocol`, `shared/models` | C++, used by both sides | Stays C++ for the client, transcribed into Go for the server |
| `shared/config` | C++, used by both sides | Same: stays, transcribed |
| `client/` | C++, Qt, libwebrtc | Untouched |
| `tools/dbadmin` | Go | Folded into the server's module, see section 5 |

The shared code is the one place the boundary is not clean.
`shared/` serves the client too, so it is not migrated, it is duplicated: the Go server gets its own transcription of the protocol, the models and the configuration.
That is two implementations of the same document, which is a cost, and the mitigation is section 6: a conformance suite that both have to satisfy, so a divergence fails a test rather than a call.

---

## 3. Stack decisions

Settled before G2, because everything after depends on them.

| Area | Decision | Reason |
| --- | --- | --- |
| Language | Go 1.25 | What `tools/dbadmin/go.mod` already asks for. One toolchain version in the repository, not two. |
| WebSocket | `github.com/coder/websocket` | Context aware, small, no global registry. `gorilla/websocket` is the alternative and is equally mature; the deciding factor is that every read here wants a context with the heartbeat deadline on it. |
| SFU | `github.com/pion/webrtc/v4` | The only Go WebRTC stack built for forwarding rather than for being an endpoint. `TrackLocalStaticRTP` rewrites SSRC and payload type on write, which is exactly what `MediaRouter::forward_audio` does by hand today. |
| RTCP feedback | `github.com/pion/interceptor` | `nack.ResponderInterceptor` is `rtc::RtcpNackResponder`, and `nack.GeneratorInterceptor` is the upstream half that `VideoFeedback` had to be written by hand for, because libdatachannel answers NACKs and never sends one. |
| Bandwidth | Ours, ported | `sfu/bandwidth_estimator.hpp` is 102 lines of loss based control with measured constants. pion's `gcc` is send side and solves a different problem. The number still travels as REMB, through `PeerConnection.WriteRTCP`. |
| MongoDB | `go.mongodb.org/mongo-driver/v2` | Already the driver `tools/dbadmin` uses, at the same version. The documents are the ones it already writes. |
| Passwords | `golang.org/x/crypto/scrypt` | Already in use, with the server's cost parameters, and with the salt handled the way the C++ does it. See section 4. |
| Logging | `log/slog` | Standard library. `trace` and `fatal` are not slog levels and become custom ones, because `--log-level` is documented and an operator's script must keep working. |
| Configuration | Written by hand | Not viper. The precedence chain, the `--key=value` form and the refusal of a bare `--key` are all documented behaviour, and a library that almost matches is worse than 300 lines that match. |
| Tests | Standard `testing` | With `-race`, which is the reason section 7 exists. |
| CGO | Off | pion, the Mongo driver and scrypt are pure Go. `CGO_ENABLED=0` gives one static binary per platform from one machine, which is the packaging half of the whole argument. |

### 3.1 The libraries, against the criteria

A table of decisions is worth nothing without the check behind it, so this is the check.
The criteria are not general ones: they are what `server/src` does today and what `docs/protocol.md` obliges anybody who replaces it to keep doing.

Status means one of three things.
**Confirmed** is an API read in the library's own documentation that does the stated thing.
**Named** is an API that exists and is the obvious answer, with the behaviour our protocol needs not yet observed.
**Spike** is a question no documentation can settle, because the answer is what our client does when it receives the result.

| Criterion, from the current server | Library and API | Status |
| --- | --- | --- |
| The server is always the offerer and owns the mids | `PeerConnection.CreateOffer`, `AddTrack` before negotiating | Confirmed |
| Forward RTP without decoding, rewriting SSRC and payload type | `TrackLocalStaticRTP.WriteRTP`, which binds per sender and rewrites on write | Confirmed |
| `a=msid:<user_id>` on each sendonly audio line | `NewTrackLocalStaticRTP(cap, id, streamID)`, stream id set to the user id | Named. The msid pion writes is `<streamID> <trackID>`, two tokens, and what the client reads out of it is a spike question |
| Opus at 111 and H.264 at 96, named by us | `MediaEngine.RegisterCodec(RTPCodecParameters, ...)`, which carries the payload type | Confirmed |
| `a=rtcp-fb:96 nack` and `goog-remb` in the offer we write | `MediaEngine.RegisterFeedback(RTCPFeedback, ...)` | Confirmed |
| Answer a viewer's NACK from a cache of recent packets | `interceptor/pkg/nack.ResponderInterceptor` | Named |
| Ask the sharer to resend what was lost upstream | `interceptor/pkg/nack.GeneratorInterceptor` | Named. This is the half libdatachannel never had, and the reason `video_feedback.cpp` exists |
| Pass a viewer's PLI up to the sharer | `RTPSender.Read` to see it, `PeerConnection.WriteRTCP` to send it up | Confirmed |
| Send REMB to the sharer once a second | `PeerConnection.WriteRTCP([]rtcp.Packet)`, with `rtcp.ReceiverEstimatedMaximumBitrate` | Named |
| Read what a viewer says it can take | `RTPSender.Read`, parsed for REMB | Named |
| Demultiplex arriving media by the SSRC in `a=ssrc` | pion's own track resolution | Spike. Section 4.3 says media without it is dropped on arrival; what pion does in its absence has to be seen, not assumed |
| One JSON message per text frame, and a reader that can time out on the heartbeat | `websocket.Accept`, `wsjson.Read(ctx, ...)` with a deadline on the context | Confirmed |
| One reader goroutine, writes from the hub loop | Documented: all methods may be called concurrently except `Reader` and `Read` | Confirmed, and it is exactly the shape section 7 asks for |
| The same documents, indexes and short timeout | `go.mongodb.org/mongo-driver/v2`, already writing them | Confirmed by `tools/dbadmin` running against MongoDB in CI |
| scrypt, N of 2^14, r of 8, p of 1, 32 bytes, salt as hex text | `golang.org/x/crypto/scrypt` | Confirmed by `TestDerivedKeyMatchesTheServer`, which exists |
| One static binary, no compiler on the target | All of the above are pure Go | Confirmed |

**What reading the documentation already changed in this plan.**

pion does not drain RTCP for you.
Its own WHIP example is explicit about it: the receiving side has to loop on `receiver.ReadRTCP()` and every outgoing sender has to loop on `rtpSender.Read()`, and *without those loops RTCP feedback is silently dropped and never processed*.
That is the M8 freeze with a different cause and the same symptom, and it is the kind of defect that produces a working demo and a frozen screen share at 5% loss.
Two goroutines that look like they do nothing are load bearing, and G5 task 5 is not done until both exist.

`TrackLocalStaticRTP.WriteRTP` copies the packet and allocates on every call.
At five participants, one screen share and one packet becoming four forwarded ones, that is worth measuring in G5 rather than assuming, and pion offers lower level write paths if it turns out to matter.

pion's own wiki states that the standard WebRTC API cannot express an SFU at all: reading, writing and modifying RTP, selecting streams, custom NACK handling.
That is the reason pion and not a higher level library, and it is also the honest version of section 11: pion gives access, not answers.

**What is rejected, and why.**

LiveKit and similar are servers, not libraries.
Adopting one is not a migration of this server, it is a replacement of the topology, the protocol and the bitrate rules with somebody else's, and the clients would have to change, which is the one thing this plan is built to avoid.

`gorilla/websocket` would do.
The deciding factor is small and stated so it can be overruled: every read here wants the heartbeat timeout expressed as a context deadline, and one library takes a context and the other takes a deadline on the connection.

**The spike, and when.**

Three of the rows above are marked Spike or turn on our client's behaviour, and all three are answered by the same half-week of work: one Go process using pion, one unmodified Qt client, one audio track.

1. Does the client accept an offer pion writes, negotiate DTLS and produce audio?
2. What does the client read out of `a=msid`, and does the stream id reach it as the speaker's identity?
3. Does pion resolve arriving tracks the way section 4.3 requires, and what happens when `a=ssrc` is absent?

**This spike runs before G2, not at G5.**
Every milestone from G2 onward assumes the answer to question 1 is yes, and it is the only assumption in this document whose failure invalidates the rest of it.
It costs days and it is placed before the milestones that cost weeks.

---

## 4. What the migration inherits

Two assets exist already, and they are what makes this a port rather than a rewrite.

**`tools/dbadmin` is a working Go implementation of the store.**
`internal/store/account.go` is the `users` document field for field, `internal/store/password.go` derives credentials with the server's scrypt parameters, and `internal/store/store.go` is 557 lines of accounts, rooms and audit against the same collections and the same indexes.
It has tests, it runs against MongoDB in CI, and `TestDerivedKeyMatchesTheServer` already asserts the property that matters.

One detail in that file is worth reading before writing any of this:
the salt handed to scrypt is the hexadecimal *text*, all 32 characters of it, not the 16 bytes it spells, because that is what the C++ does when it passes `EVP_PBE_scrypt` the `std::string` it holds.
A Go implementation that decodes the hex first produces a hash the server rejects for every password.

**`docs/protocol.md` is normative and complete.**
Sections 4.1 through 4.7 list every message with its mandatory fields, section 5 lists eighteen stable error codes, section 6 is the state machine and section 7 fixes the message ordering on join, down to the detail that a client's own `user_joined` arrives last.
That is a specification a conformance suite can be generated from, which is what G1 does.

---

## 5. Repository layout

**Decision: the whole Go tree lives at `server/go/`, and nothing of it appears at the root.**

The root of this repository is a CMake project and stays one.
A `go.mod` beside `CMakeLists.txt`, with `cmd/` and `internal/` beside `client/` and `shared/`, would make Go the first thing the layout says about a project whose client is C++ and remains C++.
The server is one of the three pieces described in the README, so it sits one directory deep, exactly like the other two.

```text
PartyShare/
├── CMakeLists.txt             still the root of a CMake project
├── client/  shared/           untouched
├── server/
│   ├── CMakeLists.txt         the C++ server, until G7
│   ├── src/                   idem
│   └── go/
│       ├── go.mod             module github.com/edermanoel94/PartyShare/server/go
│       ├── go.sum
│       ├── cmd/
│       │   ├── partyshare-server/
│       │   ├── dbadmin/       from tools/dbadmin
│       │   └── conformance/   the driver of section 6
│       └── internal/
│           ├── store/         from tools/dbadmin/internal/store, now shared
│           ├── protocol/      transcription of shared/protocol
│           ├── models/        transcription of shared/models
│           ├── config/        transcription of shared/config
│           ├── signaling/     hub, authenticator, permissions, transport
│           ├── rooms/         room manager
│           └── sfu/           media router, video feedback, bandwidth estimator
└── tests/
    └── conformance/           the scenarios of section 6, JSON, no Go in them
```

Every Go path named in this document is relative to `server/go/`.
The binary keeps the name `partyshare-server`, so nothing outside the repository has to know which of the two it is talking to.

**`tools/dbadmin` moves into that module, as `cmd/dbadmin`.**
The reason to fold it rather than leave it is in the CI file already:
the `dbadmin` job carries a comment about the one thing that can break silently in two places at once, a cost parameter changed in the tool locking every account out of the server.
Inside one module that stops being a thing to test and becomes a thing that cannot happen, because there is then one definition of `scryptCost` and one of every `bson` tag.

The alternative is leaving the tool under `tools/` as its own module and having it depend on this one through a `replace` directive.
That keeps the directory name at the cost of a second `go.mod`, a second `go test ./...` in CI, and a relative path in a file nobody reads that is wrong the first time anything moves.
The directory name is worth less than the single definition, so the tool moves, and `tools/dbadmin/README.md` becomes a pointer to where it went.

At G7 the C++ half of `server/` is deleted and `server/go/` stays exactly where it is.
Promoting it to `server/` afterwards is a rename that changes every import path in the module and buys a shorter path.

---

## 6. The conformance suite, and why it comes first

The acceptance criterion for the whole migration is one sentence: an unmodified Qt client cannot tell which server it is connected to.
That sentence is not testable by inspection, so G1 turns it into a program.

The suite is a set of scenario files, each a list of frames to send and frames to expect, driven over a WebSocket against a server binary given by a flag.
One driver, two binaries.

```json
{
  "name": "join-ordering-section-7",
  "steps": [
    { "as": "c", "send": { "type": "authenticate", "username": "carla", "password": "..." } },
    { "as": "c", "expect": { "type": "authenticated" } },
    { "as": "c", "send": { "type": "join_room", "room_id": "$room", "user_id": "$c.id" } },
    { "as": "c", "expect_ordered": [
        { "type": "user_joined", "user.id": "$a.id" },
        { "type": "user_joined", "user.id": "$b.id" },
        { "type": "user_joined", "user.id": "$c.id" } ] }
  ]
}
```

Written against the C++ server, and green against it, before a line of the Go hub exists.
That ordering is the point.
A suite written after the Go server works tests the Go server; a suite written against the one that is already correct tests the protocol, and every scenario it contains is a sentence of `protocol.md` that stopped being prose.

Where the two servers are allowed to differ is stated in the suite rather than discovered later: identifiers are opaque and are captured into variables, the `message` beside an error code is for humans and is not compared, and the order of independent messages to different connections is not asserted.
The order of section 7, which is not independent, is.

---

## 7. Concurrency, the one decision that is not a translation

Everything else in this plan is a transcription. This is not.

The C++ `Hub` is single threaded by construction: `SignalingServer` holds one mutex, every libdatachannel callback goes through it, and the protocol logic contains no locking at all.
`RoomManager`, `Authenticator` and every store say the same thing in their headers, "not thread safe, the Hub owns the only instance".

The naive Go port gives every connection a goroutine and puts a mutex on the hub, and it will look correct and pass its tests.
It is still a different program: two administrative messages can then interleave inside `handle_delete_user`, and the rule that the last administrator may not be demoted is a check followed by a write with a gap in between.

**Decision: the hub owns its state on one goroutine, fed by a channel.**
Connection goroutines read frames and send them in; the hub loop handles one at a time and returns `[]Outgoing` exactly as it does today; a writer goroutine per connection sends them out.
Every invariant the C++ tests assert stays an invariant for the same reason it is one now, `go test -race` has something meaningful to say, and the handlers stay lock free and directly testable without a socket anywhere near them.

Two consequences follow, and both are already true of the C++ and must stay understood:

- Store calls happen on that goroutine, so a slow database stalls the server.
  This is why `DatabaseConfig::timeout_ms` is 2000 and not the driver's default of thirty seconds, and the comment explaining it must be carried across.
- scrypt at N of 2^14 is sixteen mebibytes and roughly fifty milliseconds per attempt, and today the hub mutex means one at a time.
  A Go server that derives on the connection goroutine instead makes a flood of logins a flood of 16 MiB allocations.
  Derivation moves off the hub goroutine for latency, and behind a semaphore sized in configuration, which is a bound the C++ got for free from its mutex and Go has to ask for.

The SFU is the opposite case and ports directly.
Its routing table is already an immutable structure published through an atomic pointer, for the deadlock reason in section 9, and `atomic.Pointer[RoutingTable]` is the same design with less ceremony.

---

## 8. Milestones

Sizes are relative. No week counts, because none has been measured.

### G0 - The module, and the tool inside it

**Deliverables.** The module at `server/go/`, `tools/dbadmin` moved into it as `cmd/dbadmin` and `internal/store`, CI job widened to the whole module.

**Tasks.**
1. Create the module; move the dbadmin packages; fix imports.
2. Update `tools/dbadmin/README.md`, which becomes a pointer, and the `dbadmin` CI job's `working-directory`, `go-version-file` and `cache-dependency-path`.
3. Add `cmd/partyshare-server` with a `main` that prints the usage text and exits, so the binary exists from the first commit.
4. Confirm the format job is unaffected: it walks `shared client server tests tools` for `*.cpp` and `*.hpp`, so a Go tree under `server/` matches nothing it looks for.

**Acceptance.** `gofmt -l .` empty, `go vet ./...` clean, `go build ./...`, `go test -race ./...` green against Mongo in CI, and dbadmin's behaviour unchanged from a user's seat.

**Size.** Small. It is a move.

### G1 - The conformance suite, against the C++ server

**Deliverables.** The scenarios in `tests/conformance/`, and the driver that runs them in `cmd/conformance`.

The scenarios are JSON and contain no Go, which is what lets them sit outside the module beside the C++ tests they describe.
The driver is Go because it has to be one program, and it is the only Go in this milestone.

**Tasks.**
1. Driver: multi connection, variable capture, ordered and unordered expectations, timeouts.
2. Scenarios for section 4.1 through 4.5, every one of the eighteen codes in section 5, the state machine of section 6, and the join ordering of section 7.
3. Scenarios for section 4.6, the whole of administration, including the two rules that cannot be violated: an administrator may not touch their own account, and the last administrator may not be demoted or deleted.
4. A CI job that runs the suite against the C++ binary.

**Acceptance.** Every scenario green against `partyshare-server` as it is today, with no change to the C++ to make one pass.
A scenario that needs a change to the server is a scenario that found a defect or misread the document, and both are settled here rather than during the port.

**Size.** Medium, and the highest value per line in the plan.

### G1.5 - The pion spike

Independent of G1 and runnable beside it. Whoever is not writing scenarios writes this.

**Deliverables.** A throwaway program, and three answers written into section 3.1.

**Tasks.** The three questions of section 3.1, against one unmodified Qt client: an offer pion writes and the client accepts, what the client reads out of `a=msid`, and how pion resolves an arriving track with and without `a=ssrc`.

**Acceptance.** Audio from the client, through a pion process, back to a second client, with the speaker identified correctly.
The program is then deleted; what survives is the three answers and whatever they changed in G5.

**Size.** Days. It is the cheapest milestone here and the only one whose failure stops the plan, which is why it is not at the end.

### G2 - Protocol, models, configuration

**Deliverables.** `internal/protocol`, `internal/models`, `internal/config`.

**Tasks.**
1. Transcribe the message types, with the envelope rules of section 2: unknown fields ignored, `null` read as absent, missing mandatory fields an error carrying `missing_field`.
2. Transcribe `models::User`, `Room`, `AuditEntry`, and `role_from_string` with its rule that anything unrecognised is `user`.
3. The configuration loader, with the documented precedence: defaults, `--config` file, `DV_` environment, command line. `UnknownOptions::Reject` for the server, and a bare `--key` refused rather than ignored.
4. An error type carrying a protocol code, which is what `Result<T>` and `Error{code, message}` are doing today.

**Acceptance.** Ports of `test_protocol.cpp`, `test_config.cpp` and `test_models.cpp`.
`partyshare-server --help` byte identical to the C++ one, because it is documentation an operator reads.
Every configuration key that the C++ accepts, accepted.

**Size.** Medium. 1,183 lines of C++ protocol and 639 of configuration, most of it mechanical.

### G3 - Stores

**Deliverables.** `internal/store` grown from dbadmin's into the full set: user, room and audit, in memory and on MongoDB, behind the same three interfaces.

**Tasks.**
1. Lift the interfaces from `server/src/store/*.hpp`, including `count_with_role`, which exists as its own operation because the last administrator rule depends on it.
2. Memory implementations, which is what a server without a database uses.
3. Extend the Mongo implementation to rooms and audit with the field names, the indexes and the `$setOnInsert` on `created_at` that `mongo_store.cpp` uses.
4. Keep `clamp_audit_limit`: default 100, ceiling 500.

**Acceptance.** A port of `test_user_store.cpp` and `test_mongo_store.cpp`, plus one cross implementation test that is the point of the milestone:
an account created by the Go server authenticates against the C++ server, and an account created by the C++ server authenticates against the Go one, against the same database.

**Size.** Small to medium. Most of it exists.

### G4 - Signaling: authenticator, rooms, hub

**Deliverables.** A Go server that speaks the whole protocol with media routing off, answering `media_unavailable` to anything addressed to `sfu`.

**Tasks.**
1. `Authenticator`: scrypt from `internal/store`, tokens in memory only, constant time comparison, the same message for an unknown user and a wrong password.
2. `RoomManager`: capacity, one sharer at a time, six character room identifiers, persistent rooms loaded back at startup, forced mute that a participant cannot release.
3. `Hub`: the message loop of section 7, the handlers, the permission table of `permissions.hpp` consulted once, and `current_role` read from the store on every administrative message rather than from the identity the connection logged in with.
4. WebSocket transport, heartbeat, `--create-admin`, signal handling, exit codes.

**Acceptance.** Ports of `test_hub.cpp`, `test_hub_admin.cpp`, `test_room_manager.cpp`, `test_permissions.cpp` and `test_authenticator.cpp`, which is 1,563 lines of C++ tests.
The G1 suite green against the Go binary, with the media scenarios expecting `media_unavailable`.
Two Qt clients in a room, seeing each other, with no audio.

**Size.** Large. This is the milestone.

### G5 - The SFU on pion

**Deliverables.** `internal/sfu`: sessions, negotiation, audio, video, repair, bitrate.

**Tasks.**
1. Session per participant: one `PeerConnection`, server always the offerer, mids assigned by us, one recvonly audio line plus one sendonly per other participant, and both video lines created with the session so that starting a share renegotiates nothing.
2. `a=msid:<user_id>` on each sendonly audio line. In pion that is the stream id of `NewTrackLocalStaticRTP`, and it has to be confirmed against what the client reads, because the client takes the speaker's identity from it.
3. Forwarding: `TrackLocalStaticRTP.WriteRTP`, which rewrites SSRC and payload type per binding. Opus at 111 and H.264 at 96, registered on the media engine so the offer we write names them.
4. The immutable routing table behind `atomic.Pointer`, and the counters the metrics log reports.
5. Repair: `nack.ResponderInterceptor` downstream, `nack.GeneratorInterceptor` upstream, PLI from a viewer passed up to the sharer.
   The RTCP drain loops of section 3.1 are part of this task and not an implementation detail of it: without one loop per receiver and one per sender, every interceptor above is registered and does nothing.
6. Bitrate: port `bandwidth_estimator.hpp` unchanged in its constants, send REMB once a second while a stream is arriving, cap it by the lowest a viewer reported, read viewer REMB off the sender.
7. Renegotiation while an offer is in flight has to wait, as it does today.

**Acceptance.** Ports of `test_sfu.cpp` and `test_video_feedback.cpp`, driven by pion clients.
Five clients, one sharing, audio and screen working.
`docs/benchmarks.md` re-measured on the Go server and recorded as a new measurement rather than an inherited one, on the reference machine the document names, including the impaired network cases at 5% and 20% loss.
The bar is the one the C++ set: 443 frames in 15 seconds at 5% loss, not 4, and the sender's target falling under loss instead of sitting at the ceiling.

**Size.** Large, and the one with the real technical risk.

### G6 - Operational parity

**Deliverables.** Everything an operator touches.

**Tasks.**
1. Logging: the same level names, the same `--log-file`, output an existing log pipeline still parses.
2. Panics: a deferred recover writing a stack trace where `crash_reporter` writes today, under the same `--crash-directory`, so `docs/release.md` stays true.
3. Packaging: `CGO_ENABLED=0` builds for the three platforms, install rules, service unit and MSI adjusted, release workflow building the Go binary.
4. `INSTALL.md`, `README.md`, `docs/build.md`: the server no longer needs libdatachannel, OpenSSL, vcpkg or a compiler.

**Acceptance.** The packaging CI job produces an artefact containing the Go server; a fresh machine follows `INSTALL.md` to a room with two clients in it, which is what that document promises.

**Size.** Medium.

### G7 - Cutover and removal

**Tasks.**
1. Run both servers against the G1 suite in CI for the whole transition window, which is what keeps them from drifting.
2. Switch the default, soak, keep the C++ binary buildable for one release.
3. Delete `server/src`, `server/CMakeLists.txt`, the server tests, `dv::server_core` from the build, and the `libdatachannel` and `openssl` entries from `vcpkg.json` if nothing else needs them. `server/go/` stays where it is.
4. Move the Go server's documentation into `PLAN.md` and `SPEC.md` where they describe the server as C++.

**Acceptance.** Nothing C++ left under `server/`, the client untouched across the whole migration, and the G1 suite green against the only server left.

---

## 9. Bugs already paid for

Every one of these was found by running the thing, and a rewrite is exactly the event that reintroduces them.
They are listed here because reading them is cheaper than finding them twice.

| What | Where | What a Go port has to keep |
| --- | --- | --- |
| The SFU deadlock | PLAN.md, M7 | Two locks taken in opposite orders, by a join and by an arriving RTP packet, hanging the whole server. One packet landing while somebody joins is enough. The fix is the immutable routing table, and it is why `atomic.Pointer` is not an optimisation here. |
| The screen share freeze | PLAN.md, M8 and benchmarks.md | 4 frames in 15 seconds at 5% loss, because an intra frame is over a hundred packets and the viewer's only repair was to ask for another one. Both halves of retransmission are required: the responder downstream and the generator upstream. |
| A rate that never falls | PLAN.md, M8 | Without REMB nobody tells the sender that loss exists, and its estimate climbs to the 3 Mbps ceiling and stays there under a fifth of the packets being dropped. The SFU is the only party that sees both links, so it is the one that decides. |
| The estimator's constants | `sfu/bandwidth_estimator.hpp` | Above 10% loss the target falls in proportion, below 2% it grows 8% per second, and between the two nothing happens. The dead band is not a rounding: reacting to noise is how a rate oscillates instead of settling. |
| Silent media loss | protocol.md, 4.3 | A participant that does not declare `a=ssrc` on every line it sends has its media dropped on arrival, because all tracks share one transport and the SSRC is the demultiplexer. Confirm what pion does when it is absent. |

---

## 10. Risks

| Risk | Impact | Mitigation |
| --- | --- | --- |
| pion's SDP differs from libdatachannel's in a way the client notices | High | The one risk that invalidates the premise. Retired by the spike of section 3.1, which runs before G2 and not at G5, because everything after G2 assumes it away. |
| `a=msid` reaching the client differently | Medium | pion writes `a=msid:<streamID> <trackID>`; the protocol says `a=msid:<user_id>`. Setting the stream id to the user id is the likely answer, and question 2 of the spike verifies it against the client rather than assuming it. |
| RTCP feedback silently dropped | High | pion processes none of it unless something loops on `receiver.ReadRTCP` and on every `rtpSender.Read`. The failure mode is a working demo and a frozen screen share under loss, which is M8 again by another route. Section 3.1, and an assertion in G5 rather than a comment. |
| Two protocol implementations drifting | Medium | The G1 suite runs against both for the whole window, and the window ends at G7. |
| The single goroutine hub becoming a bottleneck | Low | Five participants per room. If it ever is one, the fix is a hub per room, which the room identifier already partitions cleanly. |
| scrypt memory under a login flood | Medium | 16 MiB per attempt, bounded by a semaphore rather than by a mutex that happened to serialize it. Section 7. |
| Losing behaviour that lives only in the C++ | Medium | It is what the ported tests are for: 1,563 lines of hub and room tests, and 1,177 of SFU tests, are the specification nobody wrote down. |
| The benchmark numbers not reproducing | Medium | They are re-measured in G5 and recorded honestly. A number nobody measured is worth no more than a blank space, and an inherited one is worse, because it looks measured. |
| Windows and macOS | Low | The C++ server needed a toolchain per platform; `CGO_ENABLED=0` needs `GOOS`. This risk mostly goes away, which is part of the argument. |

---

## 11. What gets simpler, and what does not

Simpler: no vcpkg, no CMake, no OpenSSL and no libdatachannel for the server; one static binary per platform from one machine; `go test -race` over concurrency that is currently checked by reading; NACK generation and response as registered interceptors instead of 355 hand written lines; one implementation of the store instead of a C++ one and a Go one that must agree.

Not simpler: pion is a toolkit rather than a solution, and everything section 4.3 says about mids, payload types, msid and SSRC still has to be written by hand and got exactly right.
The SFU is where the difficulty of this project lives, in either language.

---

## 12. Open questions

1. Does `--create-admin` stay in the server once `dbadmin` is in the same module and does the same job better? Parity first; the answer belongs after G7.
2. Does the plain text `--users-file` survive the migration? It is documented as a stopgap the SPEC rules out for deployment, and a migration is a defensible moment to drop it. Parity first, again, and then decide in the open.
3. Whether the client should eventually read a shared JSON schema instead of two hand written implementations. Out of scope here, and worth revisiting once the conformance suite exists, since the suite is most of that schema already.
