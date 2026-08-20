# Signaling protocol

This document is the normative definition of the protocol.
The C++ implementation in `shared/include/dv/protocol/message.hpp` follows this document, not the other way around.
That makes it possible to reimplement the server in another language without changing the clients, as anticipated in section 14 of the SPEC.

## 1. Transport

The transport is WebSocket, with one JSON message per text frame.
Binary frames are not used in this version.

The client connects over `ws://` or `wss://`.
Outside local development, only `wss://` should be accepted, because the session token travels on the channel.

## 2. Envelope

Every message is a flat JSON object with a `type` discriminator field.

```json
{ "type": "join_room", "room_id": "8F42A1", "user_id": "user123" }
```

Rules:

- `type` is mandatory and has to be a known string.
- Unknown fields are ignored by the receiver.
  This is intentional: it allows optional fields to be added without breaking older clients.
- A field present with a `null` value is treated as absent.
- Missing mandatory fields are an error.

## 3. Identifiers

`room_id` is exactly 6 uppercase hexadecimal characters, for example `8F42A1`.
That gives 16,777,216 combinations, enough for the MVP, and it is short enough to read out loud.

`user_id` is an opaque string assigned by the server at authentication.
Clients must never infer meaning from it.

The identifier `sfu` is reserved and belongs to no person.
It represents the server's own media endpoint, described in section 4.3.
Since server assigned identifiers are 16 hexadecimal characters, there is no way for a participant to receive it by accident.

## 4. Messages

### 4.1 Client to server

| Type | Mandatory fields | Optional fields |
| --- | --- | --- |
| `authenticate` | `username`, `password` | |
| `create_room` | `user_id` | `room_name` |
| `join_room` | `room_id`, `user_id` | `display_name` |
| `leave_room` | `room_id`, `user_id` | |

`authenticate` has to be the first message on the connection.
Anything else before it is answered with an `error` carrying code `unauthorized`.

The password appears only in that message.
The server never echoes it back and never writes it to a log.

### 4.2 Server to client

| Type | Mandatory fields | Optional fields |
| --- | --- | --- |
| `authenticated` | `user`, `token`, `expires_in_seconds` | |
| `room_created` | `room_id` | `room_name` |
| `user_joined` | `room_id`, `user` | |
| `user_left` | `room_id`, `user_id` | |
| `error` | `code` | `message` |

The `user` object has this shape:

```json
{ "id": "user123", "display_name": "Ana", "avatar": "" }
```

`id` and `display_name` are mandatory, `avatar` is optional.

### 4.3 WebRTC negotiation

| Type | Mandatory fields |
| --- | --- |
| `offer` | `room_id`, `from_user_id`, `to_user_id`, `sdp` |
| `answer` | `room_id`, `from_user_id`, `to_user_id`, `sdp` |
| `ice_candidate` | `room_id`, `from_user_id`, `to_user_id`, `candidate`, `sdp_mid`, `sdp_mline_index` |

`sdp_mline_index` is an integer.

The server must validate that `from_user_id` matches the connection that sent the message.
Accepting an arbitrary `from_user_id` would let one participant impersonate another.

These three messages have two possible destinations, decided by `to_user_id`.

**To another participant.**
The server interprets neither SDP nor candidates, it only forwards the message unchanged to `to_user_id` within the same room.

**To `sfu`.**
MVP media goes through an SFU, so the far end of each participant's connection is the server rather than another participant.
A message addressed to `sfu` is consumed by the server, and what it sends back arrives with `from_user_id` equal to `sfu`.

On that path the server is always the offerer:

```text
server -> participant:  offer          as soon as the participant joins the room
participant -> server:  answer
both directions:        ice_candidate
```

The server re-offers whenever the set of participants changes, adding or removing a media line.
Each `sendonly` audio line the server offers carries `a=msid:<user_id>`, and that is how the client knows whose voice arrives on that track.

Each participant also receives two video lines, created together with the session rather than when someone asks to share:

```text
recvonly, H.264   the participant's own screen, going up
sendonly, H.264   the screen of whoever is sharing, coming down
```

They exist from the moment of joining, empty, because that way starting and stopping a share renegotiates nothing.

The `sendonly` video line has no `a=msid:<user_id>`: it carries whoever currently has the floor, not a fixed participant.
Who is sharing is stated by `screen_share_started`, and that is where the client takes the name from.

A participant sending media has to declare their SSRC in the `answer`, with `a=ssrc`, on every line they send.
All tracks share one transport, so the SSRC is what tells the server which track each arriving packet belongs to.
Without it the media is silently dropped on arrival.

Keyframe requests cross the SFU. A viewer that needs an intra frame sends a PLI on the track it receives, and the server passes the request up to the video tracks going up in that room, because nothing in the middle decodes the video to produce one.

Retransmission works in both directions, and both video lines negotiate `a=rtcp-fb:96 nack`.
A viewer that lost a packet asks the SFU for that packet, and the SFU resends it from a cache of the most recent ones.
The SFU makes the same request upstream when a packet is lost on the way to it, rather than letting the hole propagate to every viewer.
Without that second half, a lossy link turns a screen share into a sequence of keyframe requests that never arrive whole; the measurement is in [benchmarks.md](benchmarks.md).

The SFU also tells the sharer how much to send, once per second, as REMB (`a=rtcp-fb:96 goog-remb`).
The number comes from the loss the server observes upstream, capped by the lowest a viewer has reported, and libwebrtc treats it as a ceiling for its own congestion controller.
Without it the sender has no way of learning about a loss nobody tells it about.

A server without media routing answers `media_unavailable` to any message addressed to `sfu`.

### 4.3.1 Reconnection

The client reconnects on its own when the connection drops, with the interval doubling on each attempt up to a ceiling.
Only an explicit disconnect request ends that.

Reconnecting means starting from scratch as far as the protocol is concerned: a new connection, a new `authenticate` and a new `join_room`, in the same room.
There is no session resumption, and the user identity is reissued by the server as in any other login.
Everyone already in the room sees the participant leave and join again.

### 4.4 State changes

| Type | Mandatory fields |
| --- | --- |
| `screen_share_started` | `room_id`, `user_id` |
| `screen_share_stopped` | `room_id`, `user_id` |
| `mute` | `room_id`, `user_id` |
| `unmute` | `room_id`, `user_id` |

These four messages travel in both directions.
From the client to the server, they are a request.
From the server to the clients, they are the confirmation, relayed to every other participant in the room.

A client should update its own UI only after receiving the server's confirmation, and not when sending the request.
Otherwise two people can simultaneously believe they are sharing their screen.

### 4.5 Transport level

| Type | Mandatory fields | Optional fields |
| --- | --- | --- |
| `ping` | | `nonce` |
| `pong` | | `nonce` |

The server sends `ping` every `heartbeat_interval_ms`.
The client answers `pong` with the same `nonce`.
A client that does not answer within `heartbeat_timeout_ms` is considered disconnected and removed from the room.

## 5. Error codes

Codes are stable and can be compared by equality.
The message alongside them is for humans only and may change.

| Code | Meaning |
| --- | --- |
| `invalid_json` | The payload is not valid JSON, or is not an object |
| `missing_field` | A mandatory field is absent |
| `invalid_type` | A field exists with the wrong JSON type |
| `unknown_message_type` | The `type` does not belong to this protocol |
| `room_not_found` | No room exists with that `room_id` |
| `room_full` | The room already reached its participant limit |
| `already_in_room` | The user is already in the room |
| `not_in_room` | The operation requires the user to be in the room |
| `screen_share_busy` | Another participant is already sharing their screen |
| `unauthorized` | Session token absent, invalid or expired |
| `media_unavailable` | The message was addressed to `sfu`, and this server does not route media |

The first five are detected in the parsing layer and have been implemented since M1.
The next ones depend on the server and arrived in M2, and `media_unavailable` in M4.

## 6. Session state machine

```text
        ┌──────────────┐
        │ Disconnected │
        └──────┬───────┘
               │ WebSocket connection established
               ▼
        ┌──────────────┐
        │  Connected   │
        └──────┬───────┘
               │ authenticate accepted
               ▼
        ┌──────────────┐
        │Authenticated │
        └──────┬───────┘
               │ create_room / join_room
               ▼
        ┌──────────────┐   error (room_full, room_not_found)
        │   Joining    │ ─────────────────────────────────────┐
        └──────┬───────┘                                      │
               │ user_joined referring to itself              │
               ▼                                              │
        ┌──────────────┐                                      │
        │   InRoom     │                                      │
        └──────┬───────┘                                      │
               │ leave_room, connection drop                  │
               ▼                                              ▼
        ┌──────────────┐                              ┌──────────────┐
        │  Connected   │                              │  Connected   │
        └──────────────┘                              └──────────────┘
```

Relevant transitions:

- In `Connected`, only `authenticate` is accepted.
  Every other message receives an `error` with code `unauthorized`.
- In `Authenticated`, only `create_room` and `join_room` are accepted.
- In `Joining`, the client waits for `user_joined` or `error`.
- In `InRoom`, all negotiation and state messages are accepted.
- A connection drop in any state leads to `Disconnected`.
  The client reconnects with exponential backoff and has to redo `authenticate` and then `join_room`.
  The server does not preserve session state across connections in the MVP.

## 7. Message ordering when joining a room

When user C joins a room that already contains A and B, the server sends:

```text
to C:        user_joined (A)
to C:        user_joined (B)
to C:        user_joined (C)      <- confirms their own arrival, always last
to A and B:  user_joined (C)
```

The `user_joined` referring to the user themselves arrives last and is the signal that the initial state is complete.
That saves the client from needing a separate snapshot message.

If some participant is sharing their screen, the server sends `screen_share_started` to C right after the sequence above.

After that, with media routing enabled, comes the negotiation with the SFU described in section 4.3:

```text
to C:        offer (from_user_id sfu)
to A and B:  offer (from_user_id sfu)     re-offer, now with C's track
```

The order matters: the `offer` arrives after the `user_joined` messages, so the client already knows every participant when it has to associate a track with someone.

## 8. Compatibility

Adding a new optional field is backwards compatible.
Adding a new message type is compatible: older receivers answer `unknown_message_type` and keep working.

Not compatible, and requiring an explicit protocol version bump:

- Removing or renaming a mandatory field.
- Changing the JSON type of a field.
- Changing the meaning of an existing error code.
