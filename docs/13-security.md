# 13. Security review

Section 17 of the SPEC lists four things the system must avoid. This chapter says,
for each of them, what exists today, where the evidence is, and what is still
missing — plus the things the review added along the way.

Open findings carry a severity. A finding without a fix stays here until it is
fixed; it does not quietly disappear.

## Summary

| Item | Severity | Status |
| --- | --- | --- |
| Unencrypted media | — | Covered, verified by a test |
| Privilege escalation | — | Covered: one gate, role re-read per action, verified by tests |
| Audit of administrative actions | — | Covered, and not erasable from the application |
| Reading another room's chat | — | Covered: participants only, administrators included |
| An address stored per session | — | Covered: never leaves the database, and no protocol message returns one |
| Session history kept forever | Low | **Open** by design, the operator sets the retention |
| Stream storage | — | Covered, nothing is written |
| Tokens at rest | — | Covered, there is no persistence |
| Password hash without cost | High | **Fixed** in this review — scrypt |
| Padded packet took the server down | High | **Fixed** — the SFU drops the packet |
| Password in memory during the session | Medium | **Open**, needs a resume token |
| Static TURN credentials | Medium | **Open**, needs an ephemeral credential |
| Signaling without TLS by default | Medium | **Open**, should refuse remote `ws://` |

The M8 acceptance criterion is "no open high severity findings". There are none.

## 1. Unencrypted audio or video

**Covered, and verified by a test.**

All media crosses DTLS-SRTP, which is what WebRTC requires and what both
libraries implement: libwebrtc on the client, libdatachannel on the SFU. There is
no path in the code that negotiates media in the clear, because neither library
offers one.

The evidence is not that claim. `SfuTest.TheMediaIsEncryptedAndNothingElseIsOffered`
reads the SDP from a real negotiation and demands three things of each side:
`a=fingerprint:` present, meaning the DTLS peer is authenticated by certificate;
the `RTP/SAVPF` profile, the secure one; and the absence of `RTP/AVP`, which is
the same thing without encryption.

**Finding, medium severity, open.** Signaling itself runs over WebSocket without
TLS in the default configuration. The client accepts both `ws://` and `wss://`,
and in production it has to be the second, because the session token travels on
that channel. The default should be to refuse `ws://` for any host that is not
loopback, rather than accepting it silently.

## 2. Unnecessary storage of streams

**Covered.** Nothing in the project writes audio or video to disk. Captured
frames live in a queue of two and are dropped; decoded ones go to the screen and
die there. The only path that writes a file is the log, and it records numbers,
not media.

## 3. Plain text credentials

**Partially covered, with one finding.**

What is right: passwords go through scrypt with a per-account salt and are never
stored in the clear; the password is never logged; `config::to_json` omits
`turn_password` and replaces the credentials in a database URI with asterisks, so
dumping the configuration leaks neither; and the development accounts file is
accepted but warned about at `warning` level on every startup.

**Fixed in this review: the password hash was salted SHA-256.** A digest is fast
by construction, which is exactly the property a password store cannot have — a
modern GPU tries billions of candidates per second against a stolen file.

It is scrypt now, with N of 2^14, r of 8 and p of 1: sixteen mebibytes and
something like fifty milliseconds per attempt. The parameters are deliberately the
interactive ones rather than the maximum, because the same cost that protects the
stolen file is the cost a flood of logins aims at the server.

Two tests hold it in place. `HashingAPasswordIsDeliberatelySlow` fails if somebody
swaps the derivation for a plain hash, because the time would drop from
milliseconds to microseconds. `TheSamePasswordStoresDifferentlyForDifferentUsers`
ensures the per-account salt is actually used and not merely generated.

**Finding, medium severity, open: the client keeps the password in memory for the
whole session.** It follows from automatic reconnection: the protocol has no
session resumption, so coming back after the server restarts means authenticating
again. The fix is a resume token issued in `authenticated`, and it is recorded in
[chapter 6](06-protocol.md).

## 4. Tokens persisted without protection

**Covered, by there being no persistence.** The `authenticated` message carries a
token with an expiry, and the client keeps it in memory only. Nothing is written
to disk, so there is nothing to protect at rest. That stops being true the moment
a "remember me" feature exists, and at that point the answer is the operating
system credential store, not a file.

Persistence did not change it: session tokens are deliberately not written to
MongoDB either. A token is worth one process lifetime, and persisting them would
mean a stolen database hands over live sessions as well as password hashes.
Deleting an account revokes its tokens immediately rather than letting one issued
a minute earlier live out its eight hours.

## 5. TURN with ephemeral credentials

**Finding, medium severity, open: the TURN credentials are static and come from
the configuration.** Any user of the application holds the TURN server username
and password, and can use it for whatever they like for as long as they like.

The known fix is the usual one with coturn: the signaling server derives
`username = <expiry>:<user_id>` and
`password = base64(HMAC-SHA1(secret, username))`, and hands that to the client
alongside `authenticated`. The secret never leaves the server and the credential
expires on its own. It is a protocol change, so it is recorded rather than
improvised.

## 6. Privilege escalation

Not part of section 17, and new with roles: once some accounts can remove and mute
other people and manage the account list, "who is allowed to do this" becomes
something that can be got wrong.

**Covered, and verified by tests.** The four decisions that hold it up are in
[chapter 5](05-administration.md). What is worth restating here is the evidence:

- `tests/unit/test_permissions.cpp` walks every message type in the protocol and
  asserts which of the three buckets it falls in.
- `DemotingAnAdministratorTakesEffectWithoutThemReconnecting` demotes an
  administrator on a live connection and requires the very next message on that
  same connection to be refused.
- `AParticipantIsRefusedAndTheRoomIsUnchanged` sends `kick_user` from an ordinary
  participant over a real socket, to prove the client-side hiding is decoration.
- `AnAbsentFlagDoesNotLiftARestrictionSomebodyElseApplied` and
  `AnAbsentRestrictionFlagIsNotFalse` hold the absent-means-unchanged rule at the
  Hub and at the wire.

`forbidden` and `unauthorized` are kept apart deliberately. The first means the
server knows who is asking and the answer is still no; the second means it does
not know. A client that merges them will offer to log in again in response to a
refusal that logging in cannot fix.

## 7. What the audit log does and does not promise

**Covered, with one stated trade.** The properties — nothing in it is worth
stealing, it cannot be erased from the application, and a failed audit write does
not block the action — are in [chapter 5](05-administration.md).

Without MongoDB the log lives in memory, is capped at two thousand entries, and is
gone when the process stops. It is not evidence of anything across a restart, and
a deployment that needs it to be has to turn the database on.

## 8. What the room chat stores, and who may read it

**Covered.** Chat is the first thing this project keeps that a person wrote for
other people rather than for the server, so it is worth being explicit.

**Who may read it.** The conversation of a room is readable by the participants of
that room and by nobody else. `list_chat` from anybody outside is answered
`not_in_room`, and that includes administrators: an administrator manages
accounts, rooms and who is in them, and reading what people said to each other is
not one of those powers. Without the rule, a six character identifier would be the
only thing between any account and every conversation on the server.

**How long it lives.** Exactly as long as its room. This is a correctness rule and
not housekeeping: room identifiers are six characters and are handed out again, so
a history that outlived its room would eventually be shown to whoever is given
that identifier next. `RoomManager` deletes rooms, so it is also what clears the
history — one owner of the lifetime means one place that can forget to forget.

**What is not written to the log.** The text of a message never reaches a log
file. The Hub records the identifier, the sender and the size, which is what an
operator needs to explain a rate or a failure, and nothing an operator has any
business reading.

**What is refused rather than trimmed.** Text over 2000 bytes, and text that is
empty once trimmed, are answered with `invalid_value`. The limit exists because
the server broadcasts each message to the whole room and writes it to a database,
and an unbounded field is a way to make it do both with a megabyte.

**What is not solved here.** Messages are stored as they were typed, without
encryption at rest beyond whatever the database is configured with, and an
operator with the database has them. That is the same position the audit log is
in: this project encrypts what crosses the network, not what a server was
deliberately asked to remember.

## 9. Storing an address per session

**Covered, and it is a deliberate trade.** The server writes one row per
sign-in: the account, the address the connection came from, when it started,
when it was last heard from, and when it ended. It is the first thing this
project keeps that describes a person rather than what they did, and it exists
because an operator with no running server had no way to answer "who is on the
platform" or "which address was this account on".

**What it is for.** The two questions above, and a third one that only appears
after something goes wrong: an account behaving badly, and an operator holding a
line from a firewall log with nothing to match it against. A per-session row is
the smallest thing that answers all three.

**Who may read it.** Nobody over the protocol. There is no message that returns
an address, and `user_list` carries only a boolean saying whether the account
has a connection right now. It is read by `tools/dbadmin`, which means it is
read by somebody who already has the database — and somebody who has the
database has the password hashes too, so this is not a new door.

**What it is not.** Not a location, and the field is named for the connection
rather than for the person on purpose: a server behind a proxy records the
proxy, which is a true statement about that hop and a useless one about
anybody's whereabouts.

**What is not solved here.** Nothing prunes it. The collection grows one row per
connection and keeps every address indefinitely, which for a deployment with a
retention policy is the wrong default and for one without is the only honest
one — the server cannot know how long its operator is allowed to remember.
[docs/04-server-and-database.md](04-server-and-database.md) says what to run and
where; a deployment under a regime that sets a limit has to run it.

## 10. A packet that took the server down

Not requested by section 17, and found while measuring bitrate adaptation.

**Finding, high severity: any authenticated participant could take down the entire
server with one RTP packet.** A packet with the RFC 3550 padding bit set,
forwarded by the SFU, reaches libdatachannel's sender report constructor, which
holds an `assert(!header->padding())`. The process aborts, and with it every call
in every room.

No bad faith is needed to produce one: libwebrtc sends exactly those packets when
probing bandwidth, which is how this turned up. A single client with one extra
header extension is enough for it to happen on its own.

**Fixed:** the SFU drops padded packets instead of forwarding them. A participant
who sends them loses their own frames, which retransmission then repairs, rather
than ending everyone else's call. `tests/integration/test_sfu.cpp` sends padded
packets and requires the server to keep forwarding video afterwards; without the
fix the test aborts rather than fails.

The `assert` is still in libdatachannel, and it is still an `assert` in a network
library that processes untrusted input. While it is there, no extension that makes
libwebrtc probe bandwidth may be negotiated.

## Still accepted, knowingly

The plain text development accounts file, which now takes a `"role"` as well. It
is the same finding it always was: `--create-admin` against a database is what
replaces it, and the server warns on every startup that reads one. Before any real
deployment the hash has to become Argon2id and the accounts have to come from a
real user store.
