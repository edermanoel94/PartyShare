# 6. Signaling protocol

This chapter is the **normative** definition of the protocol.
The C++ implementation in `shared/include/dv/protocol/message.hpp` follows this document, not the other way around.
That makes it possible to reimplement the server in another language without changing the clients, as anticipated in section 14 of the SPEC.

It is deliberately the one chapter of this book that was not condensed. Every
rule here is one a second implementation would have to obey, and the reasoning
beside each rule is what stops it being read as arbitrary.
The concepts behind sections 4.5 and 4.7 — chat, roles and restrictions — are in
[chapter 5](05-administration.md).

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
| `change_password` | `current_password`, `new_password` | |
| `create_room` | `user_id` | `room_name`, `persistent` |
| `join_room` | `room_id`, `user_id` | `display_name` |
| `leave_room` | `room_id`, `user_id` | |

`persistent` is a boolean, false when absent.
A persistent room outlives its last participant, so its identifier keeps working; an ordinary room is deleted the moment it empties.
Only an administrator may ask for one, and anyone else asking is answered with `forbidden` rather than quietly given an ordinary room.

`room_name` is optional and is trimmed by the server.
A room created without one is named after the identifier it has just been given, so a room name is never empty: whatever the client sent, `room_created` and every `room_list` entry carry the name the room actually has.
A name longer than 48 bytes once trimmed is answered with `invalid_value` rather than cut down, because a name that arrives shortened is one the person who chose it did not choose.
So is a name carrying a control character: a client flattens each room into one tab separated row on its way to a table, and a tab inside a name would arrive there as extra columns in everybody's list.

A name no other room is using, too: a second room asking for one that is taken is answered with `room_name_taken`.
A name is what somebody reads in a list and then clicks, so two rooms wearing one name is a list that cannot be acted on.
The comparison is on the trimmed name with its ASCII letters folded to lower case, which makes `Daily`, `daily` and `  DAILY  ` one name, and `Reunião` and `reunião` one name as well.
`REUNIÃO` is a different name from `reunião`, because folding `Ã` needs a Unicode table the server does not carry.
A room created without a name never collides: it is named after its own identifier, and an identifier that would produce a name somebody already chose is passed over for another.
Closing a room releases its name.

Rooms already in the database when this rule arrived keep the names they have, duplicates included: the rule applies to creating a room, which is the moment somebody can be asked to pick something else.

`authenticate` has to be the first message on the connection.
Anything else before it is answered with an `error` carrying code `unauthorized`, with one exception: the heartbeat of section 4.6.
`ping` and `pong` are transport level and are answered normally on a connection that has not authenticated, because the server heartbeats every connection it holds and a pong is the socket reporting itself alive rather than the client asking for anything.
Without that exception the two rules contradict each other, and a connection that failed to authenticate is told `unauthorized` once per heartbeat interval for as long as it stays open.

The password appears only in that message and in `change_password`, section 4.8.
The server never echoes one back and never writes one to a log.

### 4.2 Server to client

| Type | Mandatory fields | Optional fields |
| --- | --- | --- |
| `authenticated` | `user`, `token`, `expires_in_seconds` | |
| `password_changed` | | |
| `room_created` | `room_id` | `room_name` |
| `user_joined` | `room_id`, `user` | |
| `user_left` | `room_id`, `user_id` | |
| `error` | `code` | `message` |

The `user` object has this shape:

```json
{
  "id": "user123",
  "display_name": "Ana",
  "avatar": "",
  "role": "user",
  "restrictions": {
    "banned": false,
    "muted": false,
    "silenced": false,
    "screen_share_blocked": false
  }
}
```

`id` and `display_name` are mandatory, `avatar`, `role` and `restrictions` are optional.

`role` is `"user"` or `"admin"`.
Anything else, including an absent field, has to be read as `"user"`.
A receiver that treats an unrecognised value as anything else is one that hands out privileges to whatever a future version of the protocol invents.

`restrictions` is what an administrator has taken away from the account until they give it back, and section 4.7 says what each flag means and how it is changed.
An absent object, and an absent flag inside one, both mean false: nothing taken away.
That is the same rule the role follows and for the same reason, read in the other direction.
A sender that does not know about restrictions must not have a ban read into its silence.

A server writes the object whole, all four flags, even when none is set, so that a reader never has to tell "nothing taken away" apart from "written by something older".

The role and the restrictions appear wherever a `user` object does, so a client learns its own from `authenticated` and everyone else's from `user_joined`.
That is what lets a client disable a control the server would refuse, instead of offering it and reporting an error a second later.

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
Without that second half, a lossy link turns a screen share into a sequence of keyframe requests that never arrive whole; the measurement is in [chapter 11](11-benchmarks.md).

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
| `mute` | `room_id`, `user_id`, and `by_user_id` optional |
| `unmute` | `room_id`, `user_id`, and `by_user_id` optional |

These four messages travel in both directions.
From the client to the server, they are a request.
From the server to the clients, they are the confirmation, relayed to every other participant in the room.

In the client to server direction `user_id` has to be the sender's own.
A participant asking to mute somebody else is answered with `unauthorized`; the way an administrator does it is `force_mute`, in section 4.7.

`by_user_id` is empty or absent when somebody muted themselves, and carries the administrator's identifier when the server produced the message from a `force_mute`.
It is ignored on the way in: a participant does not get to claim who muted them.

A client should update its own UI only after receiving the server's confirmation, and not when sending the request.
Otherwise two people can simultaneously believe they are sharing their screen.

### 4.5 Chat

| Type | Direction | Mandatory fields | Optional fields |
| --- | --- | --- | --- |
| `chat_message` | both | `message` | |
| `list_chat` | client to server | `room_id` | `limit` |
| `chat_history` | server to client | `room_id`, `messages` | |

`message` and every element of `messages` is an object of this shape:

```json
{
  "id": "6890f2...",
  "room_id": "8F42A1",
  "user_id": "user123",
  "display_name": "Ana",
  "text": "the build is green",
  "timestamp_seconds": 1755676800
}
```

`room_id`, `user_id` and `text` are the only ones a client has to send.
`id`, `display_name` and `timestamp_seconds` belong to the server: they are ignored on the way in, and a client that fills them gets the server's values back, not its own.
That is the same rule `by_user_id` follows in section 4.4, and for the same reason.
A participant does not decide when their message was sent, what it is called, or whose name is on it.

The payload is a nested object rather than fields on the envelope so that a live `chat_message` and an element of `chat_history` are the same object, and a client needs one reader for both.

`user_id` has to be the sender's own, as everywhere else in this protocol.
The sender also has to be in `room_id`, and is answered `not_in_room` otherwise.

`text` is between one and 2000 bytes once the whitespace around it has been removed, and the server stores it trimmed.
Anything outside that is answered with `invalid_value` rather than truncated, because a message that arrives cut in half is worse than one that never arrived.

The server writes the message before it announces it, and a store that refuses it stops the message.
That is the opposite of what the audit log does in section 4.6, and the difference is the point: an audit entry records something that happened elsewhere, while here the store *is* the message.
Broadcasting one that was not written produces a conversation everybody present saw and nobody who reconnects can find.

What the server broadcasts goes to the whole room, the sender included.
A client displays that copy and never its own draft, which is what makes every participant read the same messages, in the same order, with the same identifiers.

`chat_history` is the newest `limit` messages of one room, **oldest first**, so that the last element is the most recent thing said and no client has to reverse it.
`limit` is clamped by the server, and zero or absent asks for its default.

It is sent unasked to a participant as they join, right after the sequence in section 7, so that somebody joining a persistent room arrives into the conversation rather than into an empty panel.
`list_chat` asks for it again, which is how a client reads further back than the default window.

A conversation is readable by the people it happened in front of.
`list_chat` from somebody who is not in that room is answered `not_in_room`, whatever their role: administration is section 4.7, and what people said to each other is not in it.
Without that rule any account could read any room by trying six characters at a time.

A room's conversation lives exactly as long as the room.
An ordinary room is deleted when it empties and its messages go with it; a persistent room keeps both.
This is not housekeeping: room identifiers are six characters and are handed out again, so a history that outlived its room would one day be shown to whoever is given that identifier next.

### 4.6 Transport level

| Type | Mandatory fields | Optional fields |
| --- | --- | --- |
| `ping` | | `nonce` |
| `pong` | | `nonce` |

The server sends `ping` every `heartbeat_interval_ms`.
The client answers `pong` with the same `nonce`.
A client that does not answer within `heartbeat_timeout_ms` is considered disconnected and removed from the room.

### 4.7 Administration

Every message in this section is refused with `forbidden` unless the connection authenticated as an account whose role is `admin`.

The role is read from the account store at the moment each of these arrives, and not from what the connection was when it logged in.
Promoting or demoting somebody therefore takes effect on their next action rather than on their next login, which is what makes revoking an administrator worth doing.

**Client to server.**

| Type | Mandatory fields | Optional fields |
| --- | --- | --- |
| `kick_user` | `room_id`, `user_id` | `reason` |
| `force_mute` | `room_id`, `user_id`, `muted` | |
| `restrict_user` | `user_id` | `banned`, `muted`, `silenced`, `screen_share_blocked`, `reason` |
| `list_users` | | |
| `create_user` | `username`, `password` | `display_name`, `role` |
| `update_user` | `user_id` | `role`, `display_name`, `password` |
| `delete_user` | `user_id` | |
| `list_rooms` | | |
| `delete_room` | `room_id` | |
| `list_audit` | | `limit`, `actor_id` |

**Server to client.**

| Type | Mandatory fields | Optional fields |
| --- | --- | --- |
| `user_kicked` | `room_id`, `user_id` | `reason` |
| `user_restricted` | `user_id`, `restrictions` | `by_user_id`, `reason`, `room_id` |
| `user_list` | `users` | |
| `room_list` | `rooms` | |
| `audit_list` | `entries` | |

`kick_user` removes one participant from one room.
They stay connected and authenticated: only the room membership ends, so they can join another room, or the same one again unless the account was also removed.
The room is told with `user_kicked`, the person removed included, and then with the ordinary `user_left`, so a client that knows nothing about administration still updates its participant list.
An administrator cannot kick themselves; that is `leave_room`, and asking is answered with `invalid_target`.

`force_mute` mutes or unmutes somebody else, and `muted` says which.
It is confirmed to the room as an ordinary `mute` or `unmute` carrying `by_user_id`, and never as a `force_mute`.

A forced mute holds until an administrator releases it.
The participant sending `unmute` about themselves while one is in place is answered with `forbidden`; muting themselves further is still allowed, because it takes nothing away from anyone.
Without that rule a forced mute is advice, undone by one click on the button that appears to have turned itself off.

`restrict_user` takes something away from an account, or gives it back, for longer than the room they are in lasts.
It is the only administrative message in this section that names no room: a restriction is about the account, so it can be applied to somebody who logged off an hour ago and it survives a restart of the server.

| Flag | While it is set |
| --- | --- |
| `banned` | The account cannot log in. `authenticate` is answered with `account_banned`, its sessions are revoked and it is removed from whatever room it was in |
| `muted` | The account cannot transmit audio. It joins rooms already muted, by the administrator rather than by itself, so the mute holds exactly as a `force_mute` holds |
| `silenced` | `chat_message` is answered with `forbidden`. Reading the conversation is untouched: a conversation somebody may not speak in is still one they are sitting in |
| `screen_share_blocked` | `screen_share_started` is answered with `forbidden`, and a share already running is stopped. `screen_share_stopped` is never refused |

Each flag is optional, and an absent one means unchanged.
That is why they are booleans that may be absent rather than booleans that default to false: lifting a restriction and leaving it alone have to be different requests.
Without the distinction, an administrator unmuting somebody would silently lift the ban a colleague applied a minute earlier, and the only sign of it would be that person logging in again.

The change is announced with `user_restricted` to the account's own connection, whether or not it is in a room, and to every participant of the room it is in.
It carries the whole resulting set rather than what changed, so a client that missed one while it was reconnecting is correct again on the next one.
The account's own connection is told before a ban closes it: a ban that ended the session first would leave the person it is about the only one who never heard why.

What `restrict_user` does not do is duplicate the announcements the room already has.
Taking the microphone is confirmed to the room as an ordinary `mute` carrying `by_user_id`, and stopping a share as an ordinary `screen_share_stopped`, so a client that knows nothing about restrictions still draws both correctly.

A `user_restricted` can also arrive with no `by_user_id` at all, and that is not a client that forgot to fill it in.
`restrict_user` is not the only way an account's restrictions change: `tools/dbadmin` edits the accounts collection directly, because it exists to be usable when there is no server running to send a message to.
A server that *is* running notices within one heartbeat interval and enforces exactly what a `restrict_user` would have, announcements included, but nobody sent it anything, so there is no account to name as the actor.
Clients render the missing name as "an administrator", which is as much as the server knows; the audit log is where the name is.
Nothing else about the message differs, so a client needs no special case beyond the one it already has for an absent optional field.

`kick_user` and `restrict_user` are different tools and neither replaces the other.
A kick ends one visit to one room and the account can come straight back; a restriction stays with the account until an administrator lifts it.

Where the two overlap, the account restriction wins.
`force_mute` with `muted` false, aimed at an account whose `muted` restriction is set, is answered with `invalid_target` rather than handing the microphone back: the room level statement is the weaker of the two, and letting it win would unmute somebody who was muted everywhere.
Lifting the restriction releases the mute in the room on its own, and is announced as an ordinary `unmute`.

`update_user` changes only the fields that are present.
An absent field means unchanged, which is why they are optional rather than empty: clearing a display name and not touching it have to be different requests.

`delete_room` closes a room.
Everyone in it is removed exactly as `kick_user` removes one person, and a persistent room stops existing rather than becoming empty.

`create_user`, `update_user`, `delete_user` and `restrict_user` are each answered with the whole new `user_list`, and `delete_room` with the whole new `room_list`.
There is no separate acknowledgement: the new state is the acknowledgement, and it leaves no window in which a panel shows something the server has already moved past.

Two rules exist so that a system cannot be left with nobody able to administer it, and both are refused rather than silently ignored:

- An administrator may not change, restrict or delete their own account. Code `invalid_target`.
- The last remaining administrator may not be demoted, banned or deleted. Code `last_administrator`.

The other three restrictions may be applied to an administrator, their own account excepted: an administrator who may not use a microphone can still administer.

`users` is an array of:

```json
{
  "user": {
    "id": "user123",
    "display_name": "Ana",
    "avatar": "",
    "role": "admin",
    "restrictions": {
      "banned": false,
      "muted": false,
      "silenced": false,
      "screen_share_blocked": false
    }
  },
  "username": "ana",
  "created_at": 1755676800,
  "online": true
}
```

`created_at` is seconds since the Unix epoch, UTC, and zero when the store does not know.
`online` says whether that account has a connection right now.

No password, salt or hash appears here, and none is defined for any message in this protocol.

`rooms` is an array of:

```json
{ "id": "8F42A1", "name": "standup", "owner_id": "user123", "persistent": true, "participant_count": 3 }
```

`entries` is an array of:

```json
{
  "id": "6890f2...",
  "actor_id": "user123",
  "actor_username": "Ana",
  "action": "kick",
  "target_id": "user456",
  "room_id": "8F42A1",
  "detail": "off topic",
  "timestamp_seconds": 1755676800
}
```

`action` is one of `kick`, `force_mute`, `force_unmute`, `restrict_user`, `create_user`, `update_user`, `delete_user`, `create_room` or `delete_room`.

A `restrict_user` entry names the flags that moved and what they became, and the reason if one was given: `silenced=true reason=off topic`.
What moved and not the resulting set, because a log that only ever states the result leaves the reader to diff it against an entry they have to go and find.
An action that changes nothing writes no entry at all.
Entries come back newest first.
`limit` is clamped by the server, and zero or absent asks for its default.
An empty or absent `actor_id` means every actor.

Ordinary participation is not recorded: joining, leaving, sharing a screen and muting yourself produce no entry, because a log full of them is a log nobody reads.

### 4.8 An account's own password

```json
{"type": "change_password", "current_password": "…", "new_password": "…"}
```

Answered with `password_changed`, which carries no fields, or with an `error`.

There is no `user_id`, and that absence is the design.
The account acted on is the one the connection's token resolves to, so the message has no field an attacker could aim at somebody else's account.
The alternative — letting an ordinary user send `update_user` with a `password` and a rule saying the `user_id` must be their own — puts the safety of the whole thing in a check that a later handler can forget to make.

`current_password` is mandatory and is checked before anything is written.
It is what stands in for the authority an administrator has when they set somebody else's password: an ordinary user has no such authority, and the only thing left to prove is that they already know the password they are replacing.
Without it, two unattended minutes at somebody's desk is enough to take their account away from them permanently.

A refusal changes nothing.
The password is checked before the new one is derived, so a wrong `current_password` leaves the account and the session exactly as they were, and the client can offer the form again without signing back in.

| Situation | Code |
| --- | --- |
| `current_password` is not the account's password | `invalid_password` |
| `new_password` is empty, or is the current password | `invalid_value` |

Succeeding revokes **every** session of the account, including the one that asked.
The connection stays open and its token stops resolving to anything, so `password_changed` is the last message it is answered with; anything after it is `unauthorized`.
A client that receives it should disconnect and sign in again.

That is deliberate, and it is the reason the change is worth making at all: a password is most often replaced because the old one is believed to be loose, and a change that leaves the tokens the old one minted alive for the rest of their lifetime has not closed the door it was opened to close.

The audit log records `change_password` with the account as both actor and target.
Neither password appears in the entry, in any form.

### 4.9 Who may send what

| Role | May send |
| --- | --- |
| `user` | `authenticate`, `change_password`, `create_room`, `join_room`, `leave_room`, `offer`, `answer`, `ice_candidate`, `screen_share_started`, `screen_share_stopped`, `mute`, `unmute`, `chat_message`, `list_chat`, `ping`, `pong` |
| `admin` | everything above, plus every message in section 4.7 |

This table says what a role may send, not who they may send it about.
Sending `chat_message` is open to everybody; sending one into a room you are not in is not, and that rule lives with the handler rather than here.

Everything a server sends is refused on the way in, whatever the role: `authenticated`, `password_changed`, `room_created`, `user_joined`, `user_left`, `user_kicked`, `user_restricted`, `chat_history`, `user_list`, `room_list`, `audit_list` and `error`.
A client sending one of those is answered with `unknown_message_type`.

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
| `account_banned` | The username and password were right, and an administrator has suspended the account |
| `media_unavailable` | The message was addressed to `sfu`, and this server does not route media |
| `forbidden` | The action requires a role the account does not hold |
| `user_exists` | The username is already taken |
| `user_not_found` | No account exists with that `user_id` |
| `invalid_target` | The action cannot be aimed at that user, such as an administrator kicking or deleting themselves |
| `last_administrator` | The action would leave the system with no administrator |
| `invalid_password` | The `current_password` sent with `change_password` is not the account's password |
| `invalid_value` | A field is present and of the right type, but its value is not usable |
| `database_error` | The persistence layer could not carry out the operation |

The first five are detected in the parsing layer and have been implemented since M1.
The next ones depend on the server and arrived in M2, and `media_unavailable` in M4.
The rest come with roles and persistence, and `account_banned` with account restrictions.

`invalid_password` and `unauthorized` are deliberately different too, and for a related reason.
`unauthorized` says the server does not know who is asking, and a client answers it by going back to the login screen.
`invalid_password` comes from a session the server knows perfectly well, about one field of a form that was filled in wrong.
A client that treated them the same would sign somebody out for a typo.

`forbidden` and `unauthorized` are deliberately different.
`unauthorized` means the server does not know who is asking; `forbidden` means it does, and the answer is still no.
A client that treats them the same will try to log in again in response to a refusal that logging in cannot fix.

`account_banned` is answered only after the password has been checked, and never in its place.
A wrong password on a banned account is `unauthorized` with the same message any wrong password gets.
Saying "banned" to whoever typed a username would turn the login form into a way to ask the server which accounts exist; the person who holds the password gets the real reason.

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
Then comes `chat_history`, so that every message it carries is about somebody C has already been told about.

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
