# Security review

Section 17 of the SPEC lists four things the system must avoid.
This document says, for each of them, what exists today, where the evidence is, and what is still missing.

Open findings carry a severity. A finding without a fix stays here until it is fixed, it does not quietly disappear.

## 1. Unencrypted audio or video

**Status: covered, and verified by a test.**

All media crosses DTLS-SRTP, which is what WebRTC requires and what both libraries implement: libwebrtc on the client, libdatachannel on the SFU.
There is no path in the code that negotiates media in the clear, because neither library offers one.

The evidence is not the claim above. `SfuTest.TheMediaIsEncryptedAndNothingElseIsOffered` reads the SDP that crosses a real negotiation and demands three things from each side:

- `a=fingerprint:` present, meaning the DTLS peer is authenticated by certificate;
- the `RTP/SAVPF` profile, the secure one;
- the absence of `RTP/AVP`, which is the same thing without encryption.

Signaling itself runs over WebSocket without TLS in the default configuration (`ws://`).
In production this has to be `wss://`, and the client accepts both. **Finding, medium severity:** the default should be to refuse `ws://` for any host that is not loopback, rather than accepting it silently.

## 2. Unnecessary storage of streams

**Status: covered.**

Nothing in the project writes audio or video to disk.
Captured frames live in a queue of two and are dropped; decoded ones go to the screen and die there.
The only path that writes a file is the log, and it records numbers, not media.

## 3. Plain text credentials

**Status: partially covered, with one finding.**

What is already right:

- Passwords on the server go through scrypt with a per-account salt, and are never stored in the clear (`server/src/signaling/authenticator.cpp`).
- The password is never logged. The hub logs the username and says so explicitly in the code.
- `config::to_json` omits `turn_password` on purpose, so that dumping the configuration does not leak the credential.
- The development accounts file is accepted, but the server warns at `warning` level that it contains plain text passwords and has to be replaced before any real use.

**Finding fixed in this review: the password hash was salted SHA-256.**
A digest is fast by construction, which is exactly the property a password store cannot have: a modern GPU tries billions of candidates per second against a stolen file.

It is scrypt now, with N of 2^14, r of 8 and p of 1, which comes to sixteen mebibytes and something like fifty milliseconds per attempt.
The parameters are deliberately the interactive ones rather than the maximum: the same cost that protects the stolen file is the cost a flood of logins aims at the server.

Two tests hold this in place.
`HashingAPasswordIsDeliberatelySlow` fails if someone swaps the derivation for a plain hash, because the time would drop from milliseconds to microseconds.
`TheSamePasswordStoresDifferentlyForDifferentUsers` ensures the per-account salt is actually used, and not merely generated.

**Finding, medium severity: the client keeps the password in memory for the whole session.**
It follows from the automatic reconnection added in M7: the protocol has no session resumption, so coming back after the server restarts means authenticating again.
The fix is a resume token issued in `authenticated`, and it is recorded in `docs/protocol.md`.

## 4. Tokens persisted without protection

**Status: covered, by there being no persistence.**

The `authenticated` message carries a token with an expiry, and the client keeps it in memory only.
Nothing is written to disk, so there is nothing to protect at rest.
That stops being true the moment a "remember me" feature exists, and at that point the answer is the operating system credential store, not a file.

## 5. TURN with ephemeral credentials

Requested by task 6 of M8, and not covered.

**Finding, medium severity: the TURN credentials are static and come from the configuration.**
Any user of the application holds the TURN server username and password, and can use it for whatever they like for as long as they like.

The known fix is the usual one with coturn: the signaling server derives `username = <expiry>:<user_id>` and `password = base64(HMAC-SHA1(secret, username))`, and hands that to the client alongside `authenticated`.
The secret never leaves the server, and the credential expires on its own.
This is a protocol change, so it is recorded here rather than improvised.

## 6. Privilege escalation

Not part of section 17, and new with roles: once some accounts can remove and mute other people and manage the account list, "who is allowed to do this" becomes a thing that can be got wrong.

**Status: covered, and verified by tests.**

Four decisions hold it up.

**One gate, not one per handler.**
`server/src/signaling/permissions.hpp` is a table over the message type enum, consulted once in `Hub::on_message` before anything is dispatched.
A handler added later without a check is not possible to write: the table's switch covers the enum, and a type nobody classified does not compile.
`tests/unit/test_permissions.cpp` walks every message type in the protocol and asserts which of the three buckets it falls in.

**The role is read from the store, not from the session.**
Every administrative message costs one account lookup, and what it buys is that demoting somebody takes effect on their next action rather than on their next login.
Revoking an administrator is otherwise a request that they please reconnect.
`DemotingAnAdministratorTakesEffectWithoutThemReconnecting` holds this: it demotes an administrator on a live connection and requires the very next message on that same connection to be refused.

**The client's check is decoration.**
The panel and the context menu are hidden from anybody who is not an administrator, and that is presentation only.
The server refuses the messages themselves, and `AParticipantIsRefusedAndTheRoomIsUnchanged` sends `kick_user` from an ordinary participant over a real socket to prove it.

**Nobody can be locked out of their own system.**
An administrator may not change or delete their own account, and the last remaining administrator may not be demoted or deleted.
Both are refused with their own error code rather than silently ignored, and both are tested.
Without them, one click leaves a deployment that can only be repaired by editing the database by hand.

`forbidden` and `unauthorized` are kept apart deliberately.
The first means the server knows who is asking and the answer is still no; the second means it does not know.
A client that merges them will offer to log in again in response to a refusal that logging in cannot fix.

## 7. What the audit log does and does not promise

**Status: covered, with one stated trade.**

Every administrative action is written: who did it, to whom, in which room, when, and what was given as a reason.
Ordinary participation is not, on purpose: joining, leaving, sharing a screen and muting yourself would fill the log with what nobody reads it for.

Two properties are worth being explicit about.

**Nothing in it is worth stealing.**
No password, no salt, no hash, no session token. `models::AuditEntry` has no field for any of them.

**It cannot be erased from the application.**
There is no protocol message and no interface that deletes an entry, and `store::AuditLog` has no removal operation to call.
An administrator with access to the database can of course still do it, and no application level design changes that.

**The trade, stated rather than hidden:** when the audit write fails, the action still goes ahead and the failure is logged at error level (`Hub::record`).
The alternative is refusing to remove a disruptive participant because the audit database is unreachable, which protects the record at the expense of the thing the record is about.
An operator who needs the opposite has one place to change it.

**A note on the in-memory log.**
Without MongoDB, the audit log lives in memory, is capped at two thousand entries, and is gone when the process stops.
It is not evidence of anything across a restart, and a deployment that needs it to be has to turn the database on.

## 8. What persistence changed, and what it did not

Accounts now live in MongoDB rather than in a process's memory, which moves the password hashes from somewhere that dies with the server to somewhere that does not.

What did not change: the hashes themselves.
scrypt with a per account salt, the same parameters, derived and compared in exactly the same code.
The store holds what the `Authenticator` gives it and knows nothing about how it was produced.

What is deliberately not persisted: **session tokens**.
A token is worth one process lifetime.
Persisting them would mean a stolen database hands over live sessions as well as password hashes, and the client already reconnects by authenticating again.

Two consequences worth stating:

- The connection string may carry credentials. `config::to_json` writes it back with them replaced by asterisks, for the same reason `turn_password` is omitted there.
- Deleting an account revokes its tokens immediately (`Authenticator::revoke_tokens_of`) rather than letting a token issued a minute earlier live out its eight hours.

## 9. A packet that took the server down

Not requested by section 17, and found while task 3 of M8 was measuring bitrate adaptation.

**Finding, high severity: any authenticated participant could take down the entire server with one RTP packet.**

A packet with the RFC 3550 padding bit set, forwarded by the SFU, reaches libdatachannel's sender report constructor, which holds an `assert(!header->padding())`.
The process aborts, and with it every call in every room.

No bad faith is needed to produce one: libwebrtc sends exactly those packets when probing bandwidth, which is how this turned up.
A single client with one extra header extension is enough for it to happen on its own.

**Fixed:** the SFU drops padded packets instead of forwarding them.
A participant who sends them loses their own frames, which retransmission then repairs, rather than ending everyone else's call.
`tests/integration/test_sfu.cpp` sends padded packets and requires the server to keep forwarding video afterwards; without the fix, the test aborts rather than fails.

The `assert` is still in libdatachannel, and it is still an `assert` in a network library that processes untrusted input.
While it is there, no extension that makes libwebrtc probe bandwidth may be negotiated.

## Summary

| Item | Severity | Status |
| --- | --- | --- |
| Unencrypted media | - | Covered, verified by a test |
| Privilege escalation | - | Covered, one gate, role re-read per action, verified by tests |
| Audit of administrative actions | - | Covered, and not erasable from the application |
| Stream storage | - | Covered, nothing is written |
| Password hash without cost | High | Fixed in this review, scrypt |
| Padded packet took the server down | High | Fixed, the SFU drops the packet |
| Password in memory during the session | Medium | Open, needs a resume token |
| Static TURN credentials | Medium | Open, needs an ephemeral credential |
| Signaling without TLS by default | Medium | Open, should refuse remote `ws://` |
| Tokens at rest | - | Covered, there is no persistence |

The plain text development accounts file is still accepted, and now takes a `"role"` as well.
It is the same finding it always was: `--create-admin` against a database is what replaces it, and the server still warns on every startup that reads one.

The M8 acceptance criterion is "no open high severity findings".
There are none open. The three that remain are medium severity and are described above with the known fix for each.
