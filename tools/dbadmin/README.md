# dbadmin

A terminal interface for the MongoDB collections a PartyShare operator has to reach without a running server: the accounts, the rooms that outlive their participants, the record of who was connected, and the record of what was done to all of it.

The signaling server owns the same data, and an administrator manages it from the panel in the client.
This program exists for the moments when that panel is not available.
A server that will not start because nobody remembers the administrator password, a database restored from a backup, a machine with a terminal and no desktop.

It writes documents the server reads back unchanged, credentials included, and it records what it does in the same audit log for the same reason the server does: an administrative action nobody can point at afterwards is an administrative action that might as well not have been checked.

It is also where an operator finds out **who is on the platform right now, and
from which address**, which is the one question the panel in the client cannot
answer across a restart: the server writes a row per session into the database,
and that collection is written for this reader and for nobody else.

```text
 PartyShare · database admin                        partyshare · 127.0.0.1:27017 · as eder

   Users (12)    Rooms (3)    Sessions (1482)    Audit (348)
 ━━━━━━━━━━━━━━━──────────────────────────────────────────────────────────────────────────

  USERNAME             DISPLAY NAME          ROLE     RESTRICTED    USER ID         CREATED
 ──────────────────────────────────────────────────────────────────────────────────────────
  ana                  Ana Souza             admin    -             3f2a91c0d4e5…   2026-08-19 10:12
  bruno                Bruno Lima            user     mic chat      aa11bb22cc33…   2026-08-20 09:40

 12 accounts · 2 administrators
 ↑↓ move · enter details · n new · e edit · p password · m restrictions · d delete · / filter
```

```text
   Users (12)    Rooms (3)    Sessions (1482)    Audit (348)
 ───────────────────────────────────────────────━━━━━━━━━━━━━━━━━━━━───────────────────────

  ACCOUNT              STATE     ADDRESS           LAST SEEN             CONNECTED
 ──────────────────────────────────────────────────────────────────────────────────────────
  ana                  online    203.0.113.7       2026-09-02 10:41:58   2026-09-02 08:12:04
  bruno                stale     203.0.113.8       2026-09-02 09:07:31   2026-09-02 09:01:12
  carla                ended     198.51.100.4      2026-09-01 18:22:10   2026-09-01 16:40:55

 1 account online · reading the newest 200 of 1482 sessions
 ↑↓ move · enter details · l read more · / filter · r refresh · tab next screen · q quit
```

## Running

```sh
cd tools/dbadmin
go run . --uri=mongodb://127.0.0.1:27017 --database=partyshare
```

Or build it once:

```sh
go build -o dbadmin .
./dbadmin
```

| Option | Environment | Default |
| --- | --- | --- |
| `-uri` | `DV_DATABASE_URI` | `mongodb://127.0.0.1:27017` |
| `-database` | `DV_DATABASE_NAME` | `partyshare` |
| `-timeout` | `DV_DATABASE_TIMEOUT_MS` | 5s |
| `-operator` | | the operating system user |

The environment variables are the server's own, so a shell already set up to start the server is already set up to administer it.
The flags win over them.

`-operator` is the name that goes on the audit entries this session writes.
It is not an account: whoever runs a database tool has a shell and not a session, so the entries are attributed to `dbadmin:<name>`, which is one filter away from being told apart from what the server wrote.

The connection is opened before the interface starts.
A terminal program that clears the screen and then says it cannot reach the database has hidden the shell its operator needs to fix it.

## The screens

**Users** lists every account, oldest first, which is the order the server's own user list uses.

| Key | |
| --- | --- |
| `↑` `↓` | move |
| `enter` | the account as a card, with the identifier in full |
| `n` | create an account |
| `e` | edit the username, display name, avatar and role |
| `p` | set a password |
| `m` | restrictions: what the account may no longer do |
| `d` | delete, after a confirmation |
| `/` | filter by username, display name or identifier |
| `r` | read everything again |
| `tab` | the next screen |
| `q` | quit |

**Rooms** lists every room the database holds, oldest first, which is the order the server's own room list uses.

A room used to be out of scope here, on the grounds that the server made and destroyed one while people were inside it.
That stopped being true: a room now outlives its last participant and only an administrator ever closes one, so a room is exactly the kind of thing that is still there when every server process is gone.

| Key | |
| --- | --- |
| `↑` `↓` | move |
| `d` | delete, after a confirmation |
| `/` | filter by room, name or owner |
| `r` | read everything again |

The owner column shows a username, resolved from the account list the users screen reads.
A room whose owner has been deleted shows the identifier instead of a blank, because a blank reads like a bug in the column.

**Sessions** lists who has been connected, everyone still here first.

This is the screen for "is Bruno online", "which address was he on", and "when
was he last here at all". The server writes one document per sign-in and
refreshes it on its heartbeat; nothing in this program writes to that collection
and nothing ever will, which is the same reason there is no key here that
changes anything. A presence history an administrator can edit is not evidence
of anything.

| Key | |
| --- | --- |
| `↑` `↓` | move |
| `enter` | the session as a card, with the identifier in full |
| `l` | read more: 200, 500, 1000, 2000 |
| `/` | filter by account, address or state |
| `r` | read everything again |

The order is what makes a page of two hundred enough: every session that has not
finished comes first, whatever time it started, and the rest follow by how
recently anybody was heard from. Somebody connected since this morning would
otherwise sit below the two hundred visits that came and went since.

The account column shows a username, resolved from the account list the users
screen reads, and falls back to the identifier for a session whose account has
been deleted.

The state column has three values and the difference between the first two is
the point:

| State | |
| --- | --- |
| `online` | Open, and heard from within the last thirty seconds |
| `stale` | Open, and not heard from. A server that was killed rather than stopped leaves these behind, and nothing but the next server start will close them |
| `ended` | The server closed it, either when the person left or on the way out |

Thirty seconds is six times the server's default heartbeat interval. One missed
beat is a slow network; six is a machine that is not there. An operator who has
raised `server.heartbeat_interval_ms` past five seconds should raise
`SessionStaleAfter` in `internal/store/session.go` with it — this program is
pointed at a database and has no way to read the server's configuration file.

The tab counts documents, as every tab here does. How many of them are somebody
who is on the platform is a question about time, and it is on the status line:
`3 accounts online · reading the newest 200 of 1482 sessions`.

The users screen carries the same answer for one account, on the card behind
`enter`: `Presence  online from 203.0.113.7`. It is the one line of that card
that does not come from the account document, and it is there because it is what
somebody looking at an account most often wants next.

**Audit** lists what administrators did, newest first.

| Key | |
| --- | --- |
| `enter` | the entry as a card, with every field |
| `a` | only this actor, which is a new query and not a filter over what was read |
| `l` | read more: 100, 200, 500, 1000, 2000 |
| `/` | filter what was read, over every field |
| `r` | read everything again |

## What it writes

An account is one document of the `users` collection, with the fields `server/src/store/mongo_store.cpp` reads: `user_id`, `username`, `display_name`, `avatar`, `role`, `salt_hex`, `password_hash_hex`, `created_at` and `restrictions`.
Nothing else, because a field the server does not read is how two writers of one collection drift apart.

A password is stored the way the server's `Authenticator` stores it: scrypt with N of 2^14, r of 8, p of 1, a key of 32 bytes, and the salt passed as the hexadecimal text rather than the sixteen bytes it spells.
Those parameters are not tunable here.
A password stored under different ones is not a weaker password, it is a password nobody can log in with, and `TestDerivedKeyMatchesTheServer` pins them against vectors produced by OpenSSL through the call the server makes.

A room is one document of the `rooms` collection: `id`, `name`, `owner_id`, `persistent` and `created_at`.
Nothing here ever writes one — the server does that when somebody creates a room — and the only change this program makes to that collection is removing a document.

A session is one document of the `sessions` collection: `user_id`, `ip`, `connected_at`, `last_seen_at` and `ended_at`.
Nothing here writes one either, and unlike the rooms this program does not remove them: the collection is what the server saw, and the only thing to be done with it is read it.
It grows one row per connection and nothing prunes it, as nothing prunes the chat or the audit log — how much history to keep is an operator's policy, and a deployment that wants a year and not five will want a `deleteMany({ended_at: {$lt: …}})` in whatever already runs its backups.

Every change writes an audit entry, in the server's own vocabulary: `create_user`, `update_user`, `delete_user`, `restrict_user` and `delete_room`, with the detail naming what actually moved.
A `delete_room` entry carries the room in both `target_id` and `room_id`, which is what the server writes for its own: an entry the two programs disagree on is one somebody has to know the origin of before they can read it.
When the change succeeds and the entry cannot be written, the change stands and the status line says so in the colour of a warning rather than a success.
Refusing an administrative change because the log is unreachable protects the log at the expense of the thing the log is about, which is the trade [docs/13-security.md](../../docs/13-security.md) already states for the server.

The last administrator cannot be deleted or demoted, here as on the server.
A system with nobody able to administer it is a system that needs the database edited by hand to be recovered, and this program is that hand.

## Restrictions

`m` opens the four things an administrator can take away from an account.
They are one subdocument, `restrictions`, and the same one the server writes:

| Field | While it is set |
| --- | --- |
| `banned` | The account cannot sign in. The lasting form of a kick: a kick ends one visit, this ends access until somebody lifts it |
| `muted` | The account cannot use a microphone. It arrives in a room already muted, by the administrator rather than by itself, and cannot unmute itself |
| `silenced` | The account cannot write in a room's chat. Reading it is untouched |
| `screen_share_blocked` | The account cannot start a screen share, and the server stops one already running when it reads this |

They are account wide and they stay written down, which is why this program can set them at all.
A kick and a forced mute are about a room a running server holds in memory, and a database tool reaches neither; a restriction is about the account, so the two programs are writing the same thing.

The whole set is written at once, because the form shows all four and whoever has just read them is the one qualified to write them.
The client's per participant menu is the other case: it sends one flag and leaves the rest alone, so that silencing somebody does not lift a ban a colleague applied a minute earlier.

The last administrator cannot be banned, for the reason they cannot be deleted or demoted.
The other three are allowed on any account, an administrator's included: an administrator who may not use a microphone can still administer.

A running server re-reads the account on its next message from it, so a restriction written here takes effect there without a restart.
It also acts on what is already happening, which it did not always do: a microphone already on, a share already running and a session already open are all reached within one heartbeat interval, five seconds by default.
The server notices by comparing the accounts of the people currently connected against what each of their sessions logged in with, once per heartbeat, in `server/src/signaling/restriction_source.hpp`.
Nothing here has to announce anything for that: this program still writes the document and only the document, which is what lets it work against a database with no server anywhere near it.

The one difference from the panel is whose name is on it.
A restriction sent from the client names the administrator who sent it, and every participant's client says so; one written here reaches them with no name, and their client says "an administrator", because that is all the server can tell from a document that changed.
The audit entry this program writes is where the name is, attributed to `dbadmin:<name>`.

The list shows what is taken away in a column short enough to fit, `ban mic chat screen`, and the detail card behind `enter` prints the field names in full.

Lifting them is the same form with the answers set back to `no`.
A form submitted with the answers it already had writes nothing and records nothing, because a form nobody changed is not an administrative action.

## What it deliberately does not do

It does not create collections or indexes.
The schema belongs to the server, and a tool pointed at a mistyped database name should leave an empty database empty rather than furnish it.
Until the server has run once against a database, the unique index on `username` is not there, and a duplicate is refused by the check before the insert rather than by the database.

It does not create rooms.
Deleting one is removing a record; creating one is deciding that six characters are free, which is the server's job and needs the live map to answer.

It does not close a room on a running server.
Deleting the document means the room does not come back at the next start; it does not evict anybody, and the identifier goes on working until that process ends.
The confirmation screen says so.
With the server up, close it from the client's admin panel instead, where all of it takes effect at once.

It does not kick anybody out of a room, and it does not force-mute one participant of one room.
Both are about a room, and a room is memory in a process this program has no connection to.
Banning and muting an *account* is the lasting half of the same thing, and that is what the restrictions above are — those a running server does pick up and act on, microphone included, within a heartbeat.

It does not send anybody a notice.
That one is not about a room, so the argument above does not cover it, and it is worth saying why separately.
A notice has to reach whoever is connected at the moment it is written, and it has to have an identifier from that moment so the person can acknowledge it — the server does both in one step, and a document written here would be a message nobody was handed and a receipt nobody could send.
Restrictions work from here precisely because they need neither: a flag on an account is true whether or not anybody was told.
With the server up, send it from the client's admin panel, where the recipient gets it at once and the audit log records both halves.

It does not close a session, and there is no key here that tries.
A row in that collection is what the server saw, and an operator who could edit it could edit away the evidence of who was on the platform.
A session that is open and stale is closed by the next server to start against the database, which is the one program entitled to say the connection is over.

It does not decide *when* a running server acts, only that it eventually does.
Removing an account and banning one both reach a live server on its next pass rather than at the moment the form is submitted, so there is up to a heartbeat interval — five seconds by default — between the write and the room emptying.
The confirmation screen says so.
There is no version of this that is instant: this program's whole contract is that it works with no server to talk to.
With the server up, remove or restrict the account from the client panel instead, where all of it takes effect at once.

## Tests

```sh
go test ./...                                            # the screens and the password derivation
DBADMIN_TEST_MONGO_URI=mongodb://127.0.0.1:27017 go test ./...   # everything
```

The tests that need a database skip themselves without `DBADMIN_TEST_MONGO_URI`, so a machine with no MongoDB still runs the rest.
Point it at one it may write to:

```sh
docker run -d --rm -p 27017:27017 --name partyshare-mongo mongo:7
```

They create a database per test, named `dbadmin_test_*` or `dbadmin_e2e_*`, and drop it afterwards.

There are three layers, and each one exists because the others cannot fail in its place.
`internal/store` writes to a real MongoDB and checks the documents field by field.
`internal/ui` drives the screens with keystrokes against a fake database, which is what makes the error paths reachable.
`internal/e2e` drives the whole program against a real one, and is the test that says an account created through the form is an account the C++ server can authenticate.

The rule that decides whether a session counts as online is in `internal/store/session_test.go`, and it needs no database at all.
It is the one piece of logic here that a wrong answer makes actively misleading rather than merely broken — reporting somebody as present when their server died is worse than reporting nothing — so its window is pinned from both sides rather than left to whatever the expression happens to do.
