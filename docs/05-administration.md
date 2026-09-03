# 5. Administration

Who may do what, how it is enforced, and the two places it can be done from.

The normative wire definition is [section 4.7 of chapter 6](06-protocol.md). This
chapter is the concepts and the operational half.

## Roles

An account carries a role, `user` or `admin`. Anything else — including an absent
field — reads as `user`, in every reader. A receiver that treats an unrecognised
value as anything else is one that hands out privileges to whatever a future
version of the protocol invents.

A user joins and creates rooms and shares a screen. An administrator can also
remove and mute other participants, manage accounts and rooms, apply
restrictions, and read the audit log.

Four decisions hold the whole thing up.

**One gate, not one per handler.** `server/src/signaling/permissions.hpp` is a
table over the message type enum, consulted once in `Hub::on_message` before
anything is dispatched. A handler added later without a check is not possible to
write: the table's switch covers the enum, and a type nobody classified does not
compile.

**The role is read from the store, not from the session.** Every administrative
message costs one account lookup, and what it buys is that demoting somebody
takes effect on their next action rather than on their next login. Revoking an
administrator is otherwise a request that they please reconnect.

**The client's check is decoration.** The panel and the context menu are hidden
from anybody who is not an administrator, and that is presentation only. The
server refuses the messages themselves.

**Nobody can be locked out of their own system.** An administrator may not
change, restrict or delete their own account (`invalid_target`), and the last
remaining administrator may not be demoted, banned or deleted
(`last_administrator`). Both are refused with their own error code rather than
silently ignored. Without them, one click leaves a deployment that can only be
repaired by editing the database by hand.

## Kick, force mute, restrictions, and a notice

Four different tools, and none of them replaces another.

| | Reaches | Lasts |
| --- | --- | --- |
| **Kick** | One participant, one room | That visit. They can come straight back |
| **Force mute** | One participant, one room | Until an administrator releases it |
| **Restriction** | The account | Until an administrator lifts it. Survives the room, the session and the process |
| **Notice** | The account | Until they say they have read it. Waits for them if they are not connected |

The first three take something away. A notice takes nothing away, and that is
what it is for: it is the only way to tell somebody *why*, and the only one of
the four that can be aimed at a person who is not connected and still reach
them.

## Sending somebody a notice

`Message` on the accounts tab of the panel. It is one box of text, at most 500
bytes, and it arrives on their screen as a box with one button.

A notice is not chat. Chat belongs to a room, is read by everybody in it, and
needs no answer. A notice belongs to the account, is read by nobody else, and
exists to be answered — and until it is answered, the server hands it over again
at the start of every session that account opens. Somebody who was signed out
when it was written reads it the next time they sign in; somebody who closed the
box without answering reads it again.

The answer is one button and it goes nowhere near the sender. It is written into
the audit log as `acknowledge_notice`, with the person who read it as the actor,
because a receipt that only arrived while the administrator happened to be
connected would be a receipt nobody could rely on — and the two of them not
having to be online at the same time is the whole point.

Both halves are in the log, and both carry the identifier of the notice, so a
message and its answer can be read as a pair:

```text
2026-09-02 09:41  ana    send_notice         bruno  notice=68b0… please use a headset
2026-09-02 10:03  bruno  acknowledge_notice  bruno  notice=68b0… from=ana
```

The full text is in the entry. "An administrator sent a message" with no message
is a row nobody can act on afterwards, and 500 bytes is a limit small enough
that a log full of them is still a log somebody scrolls.

Deleting an account removes every notice ever sent to it. These are messages
written to a named person, and a record with a subject and no owner is not one
worth keeping.

`dbadmin` can send one too, from the users screen, with no server running. It
writes the document and nothing else; a running server hands it over on its
next heartbeat if the person is connected, and their next sign-in does if they
are not, exactly as for one sent from the panel while they were away. What
differs is the sender: there is no account behind a terminal, so the notice
arrives from "an administrator", and the audit entry is where the operator's
name is - the same two facts as for a restriction written from there.

A forced mute holds until an administrator releases it: a participant sending
`unmute` about themselves while one is in place is refused. Muting themselves
further is still allowed, because it takes nothing away from anyone. Without that
rule a forced mute is advice, undone by one click on the button that appears to
have turned itself off.

### The four restrictions

| Flag | While it is set |
| --- | --- |
| `banned` | The account cannot sign in. Its sessions are revoked and it is removed from whatever room it was in |
| `muted` | The account cannot transmit audio. It joins rooms already muted, by the administrator rather than by itself |
| `silenced` | The account cannot write in a room's chat. Reading it is untouched — a conversation somebody may not speak in is still one they are sitting in |
| `screen_share_blocked` | The account cannot start a share, and one already running is stopped. Stopping is never refused |

Each flag is optional on the wire, and **an absent one means unchanged**, not
false. That is why they are booleans that may be absent rather than booleans that
default to false: lifting a restriction and leaving it alone have to be different
requests. Without the distinction, an administrator unmuting somebody would
silently lift the ban a colleague applied a minute earlier, and the only sign of
it would be that person logging in again.

Where a room-level statement and an account restriction overlap, **the
restriction wins**. Force-unmuting an account whose `muted` restriction is set is
refused rather than handing the microphone back. Lifting the restriction releases
the mute in the room on its own.

A ban is enforced **after** the password is checked and never in its place. A
wrong password on a banned account answers `unauthorized`, the same as any wrong
password; only somebody holding the password is told the account is suspended.
Answering "banned" to whoever typed a username would turn the login form into a
way to enumerate accounts.

## The two paths

### From the client

An administrator gets a panel: accounts, rooms, restrictions and the audit log,
plus a per-participant menu inside a room. Everything takes effect at once, and
every change is announced to the room and to the account it is about.

The per-participant menu sends one flag and leaves the rest alone, so silencing
somebody does not lift a ban a colleague applied a minute earlier.

### From `dbadmin`, with no server running

[tools/dbadmin](../tools/dbadmin/README.md) is a terminal front end for the same
collections. It exists for the moments the panel is not available: a server that
will not start because nobody remembers the administrator password, a database
restored from a backup, a machine with a terminal and no desktop.

```sh
cd tools/dbadmin
go run . --uri=mongodb://127.0.0.1:27017 --database=partyshare
```

It reads the same `DV_DATABASE_*` environment variables the server does, so a
shell already set up to start the server is already set up to administer it.

It writes documents the server reads back unchanged, credentials included:
scrypt with N of 2^14, r of 8, p of 1, a 32-byte key, and the salt passed as
hexadecimal text. Those parameters are not tunable there. A password stored under
different ones is not a weaker password, it is a password nobody can log in with.

Three differences from the panel, all of them consequences of there being no
server to talk to:

- **It writes the whole restriction set at once**, because its form shows all
  four and whoever has just read them is the one qualified to write them.
- **A running server picks the change up within one heartbeat**, five seconds by
  default, rather than at the moment the form is submitted. Sessions already
  open, microphones already on and shares already running are all reached, but
  not instantly. The confirmation screen says so.
- **The change arrives with no actor name.** Nobody sent a message, so there is
  no account to name; clients render it as "an administrator". The audit entry is
  where the name is, attributed to `dbadmin:<name>`.

It can also sign somebody out, from the sessions screen. That is a mark on the
account, `session_end_requested_at`, which the server reads on the same
heartbeat pass and answers with the same exit a ban takes - out of the room,
tokens revoked, everybody told - without the ban: nothing is taken from the
account and the person may sign in again at once. The server zeroes the mark
once it has acted, and a login discards one written before it, so a request
made a heartbeat too late cannot end a session nobody asked about.

It deliberately does not create collections or indexes, create rooms, close a
room on a running server, or kick one participant out of one room. The first
because the schema belongs to the server; the other three because they name a
room in a running process's memory, which a database tool has no connection to,
both of those are the server's job.

What it does have that the panel does not is **who is online, and from where**.
The server writes one row per session — the account, the address, when it
started, when it was last heard from — and that collection is written for this
reader and for no other. The panel shows an `Online` column, but only about the
server it is connected to and only while it is; the sessions screen is a history
that outlives the process, which is what makes it the place to answer "which
address was Bruno on last Tuesday".

## The audit log

Every administrative action is written: who did it, to whom, in which room, when,
and what was given as a reason. Ordinary participation is not — joining, leaving,
sharing a screen and muting yourself would fill the log with what nobody reads it
for.

`action` is one of `kick`, `force_mute`, `force_unmute`, `restrict_user`,
`create_user`, `update_user`, `delete_user`, `create_room`, `delete_room`,
`change_password`, `send_notice` or `acknowledge_notice`. A `restrict_user`
entry names the flags that **moved** and
what they became, plus the reason if one was given: `silenced=true reason=off
topic`. What moved and not the resulting set, because a log that only ever states
the result leaves the reader to diff it against an entry they have to go and
find. An action that changes nothing writes no entry at all.

`acknowledge_notice` is the one entry written by somebody who is not an
administrator: the actor is the person who read the notice. It belongs here all
the same, because it is the second half of an administrative action, and an
action whose outcome is recorded somewhere else is one nobody can follow
through.

Three properties worth being explicit about:

**Nothing in it is worth stealing.** No password, no salt, no hash, no session
token. `models::AuditEntry` has no field for any of them. The text of a notice
is the one thing an entry quotes in full, and it is there because it is what
somebody was told — a message an administrator sent and can no longer produce is
not a message anybody can be held to.

**It cannot be erased from the application.** There is no protocol message and no
interface that deletes an entry, and `store::AuditLog` has no removal operation
to call. An administrator with access to the database can of course still do it,
and no application level design changes that.

**When the audit write fails, the action still goes ahead**, and the failure is
logged at error level. That is a stated trade, not an oversight: the alternative
is refusing to remove a disruptive participant because the audit database is
unreachable, which protects the record at the expense of the thing the record is
about. An operator who needs the opposite has one place to change it.

Without MongoDB the audit log lives in memory, is capped at two thousand entries,
and is gone when the process stops. It is not evidence of anything across a
restart, and a deployment that needs it to be has to turn the database on.

## What administration does not reach

An administrator manages accounts, rooms and who is in them. **Reading what
people said to each other is not one of those powers.** A room's conversation is
readable by the participants of that room and by nobody else; `list_chat` from
anybody outside is refused whatever their role. Without the rule, a six character
identifier would be the only thing between any account and every conversation on
the server.
