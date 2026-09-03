# 4. Server and database

The server is one process doing two jobs: signaling over WebSocket, and an SFU
forwarding RTP. It transcodes nothing, so it spends bandwidth and almost no
processor.

MongoDB is the intended shape for any server other people log into. Without it
the server keeps accounts, rooms, conversations and the audit log in memory, and
loses all four when it stops — which is fine for a five minute test and nothing
else.

## Building the server

Persistence needs both a CMake option and the vcpkg feature that brings the
driver, so it is its own build tree:

```sh
cmake -S . -B build/server \
  -DDV_ENABLE_MONGO=ON -DVCPKG_MANIFEST_FEATURES=mongo \
  -DDV_BUILD_CLIENT=OFF -DDV_BUILD_TESTS=OFF \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build/server
```

`-DDV_BUILD_CLIENT=OFF` is what lets a machine with no Qt build this at all.
[INSTALL.md](../INSTALL.md) is the full walkthrough, Windows included.

A build **without** `-DDV_ENABLE_MONGO` refuses to start when the database is
turned on, rather than falling back to memory. A server that was told to persist
and quietly did not is one whose accounts disappear at the next restart.

## Starting it

On Debian 12 or Ubuntu 24.04, `sudo scripts/install_server.sh --admin=ana` does
all of this and leaves the server running as a systemd service, with MongoDB
installed from its apt repository beside it; [INSTALL.md](../INSTALL.md) section
1.0 has the options. By hand:

```sh
docker run -d -p 27017:27017 --name partyshare-mongo mongo:7

./build/server/bin/partyshare-server \
  --database-uri=mongodb://127.0.0.1:27017 \
  --create-admin=ana:choose-a-password

./build/server/bin/partyshare-server \
  --database-uri=mongodb://127.0.0.1:27017 \
  --port=8080 --ice-port-range=50000-50100
```

`--database-uri` turns the database on by itself, so there is no second switch
that has to agree with the first.

`--create-admin` creates that administrator, or promotes an existing account and
resets its password, and then exits. It is also the way back in when the only
administrator password is lost. The password is visible in `ps` while the command
runs, so change it from the client afterwards — [section 4.8 of the
protocol](06-protocol.md) is the self-service path.

The same is true of a URI carrying a password, which is one reason to supply it
through the environment or the configuration file instead. Both forms are in
[chapter 3](03-configuration.md).

## Accounts without a database

`--users-file` points at a JSON list, and exists only so the MVP has users:

```json
[
  {"username": "ana", "password": "test-password", "display_name": "Ana", "role": "admin"},
  {"username": "bruno", "password": "test-password", "display_name": "Bruno"}
]
```

`role` is optional, and anything other than `"admin"` reads as an ordinary user.

That file stores passwords in plain text. Section 17 of [SPEC.md](../SPEC.md)
forbids it in production, and the server logs a warning on every startup that
reads one. `--create-admin` against a database is what replaces it.

## What the database holds

One document per account, one per persistent room, one per chat message, one per
administrator's notice, one per session and one per administrative action. None
of them carries media, so a deployment with a hundred accounts and a year of
history is measured in megabytes: the database is not what sizes the machine,
the outbound media is.

| Collection | Written by |
| --- | --- |
| `users` | The server, and [tools/dbadmin](../tools/dbadmin/README.md). One field, `session_end_requested_at`, goes the other way: written by `dbadmin`, read and zeroed by the server |
| `rooms` | The server only |
| Chat | The server only, and deleted with its room |
| Notices | The server, and `dbadmin`, and deleted with the account they were sent to. Only the server ever acknowledges one |
| Sessions | The server only. Read by `dbadmin`, written by nothing else; ending one is a mark on the account, above |
| Audit | The server, and `dbadmin`, in the same vocabulary |

The sessions collection is the one here that exists for a reader rather than for
the server. The server is holding the sockets and never has to ask who is
connected; `dbadmin` talks to MongoDB and to nothing else, so "who is online" is
a question only the database can answer, and only because the server writes the
answer into it. One row per session: the account, the address the connection
came from, when it started, when it was last heard from on the heartbeat, and
when it ended.

A server that is stopped closes its rows on the way out. One that is **killed**
does not, and nothing else ever will, so the next server to start against that
database closes every row it finds open — stamped with when each was last heard
from rather than with the moment of recovery, because a server killed on Friday
and started on Monday did not have anybody connected over the weekend. Until
that happens, a reader tells the difference by the heartbeat: a row that is open
and has not been touched for six heartbeats is a server that is gone, not a
person who is there.

It is also the one collection that grows one row per connection rather than one
per decision. Nothing here prunes it, as nothing prunes the chat or the audit
log, and for the same reason: what to keep is an operator's policy and not the
server's. A busy deployment that wants a year and not five will want a periodic
`db.sessions.deleteMany({ended_at: {$lt: …}})` in whatever already runs its
backups.

Session tokens are deliberately **not** persisted. A token is worth one process
lifetime, and persisting them would mean a stolen database hands over live
sessions as well as password hashes. The client reconnects by authenticating
again.

The database may live on the same machine or another one. The server holds its
lock while it talks to it, so what matters is latency and not throughput: the
two second default timeout is what stops an unreachable database from holding up
every call on the server.

## Ports

| Port | Protocol | Use |
| --- | --- | --- |
| 8080 | TCP | WebSocket signaling, inbound. `--port` or `DV_SERVER_PORT` |
| a range, or ephemeral | UDP | ICE and media, inbound |
| 19302 | UDP | STUN, outbound, whatever `network.stun_servers` names |
| 3478 | UDP | TURN, outbound, only when `network.turn_url` is set |
| 27017 | TCP | MongoDB, outbound, only when persistence is on |

The server has to reach the clients over UDP. Behind NAT it uses the configured
STUN servers, plus an optional TURN for the cases STUN does not solve.

### Narrowing the media range

Left alone, the SFU asks the system for an ephemeral port on every connection,
and a firewall in front of it has nothing narrower to allow than the whole
ephemeral range — 32768 to 60999 on most Linux systems. `--ice-port-range`
replaces that with a range you choose:

```sh
partyshare-server --ice-port-range=50000-50100
```

Both ends have to be given: half a range is refused at startup rather than
silently becoming no range at all. One range is worth nothing — 1024 to 65535 is
libdatachannel's own default, which it reads as "no range" and answers with an
ephemeral port anyway.

Size it from the load, because the SFU binds one port per participant:
`max_participants_per_room` - the largest room anybody may create, 20 by default -
times the number of rooms running at once. A hundred ports carries five rooms at
that ceiling, or twenty rooms of the default five. The server logs the range it ended up with on
startup, or warns that the ports are ephemeral when none was set.

That warning is worth reading. A server started without it, behind a firewall
that opened the documented range and nothing else, gives you a room where the
participant list, the chat and every signaling message work perfectly and nobody
hears anybody. It is entry 8 of [chapter 15](15-postmortems.md).

```sh
# Linux, with the range above
sudo ufw allow 8080/tcp
sudo ufw allow 50000:50100/udp
```

On AWS, GCP or Azure the security group needs the same two entries; the host
firewall alone does not open them.

## Bandwidth per room

Five participants, one sharing, video at the 3000 kbps ceiling:

| Direction | Arithmetic | Total |
| --- | --- | --- |
| In | 1 video at 3 Mbps plus 5 audio at 48 kbps | ~3.3 Mbps |
| Out | 4 copies of the video plus 20 copies of audio | ~13 Mbps |

The outbound side grows with the number of viewers, and it is what sizes the
machine. For planning: add 3.3 Mbps per screen viewer, and 200 kbps per
participant for audio.

A share carrying sound adds no stream and no work to the SFU — it rides inside
the sharer's own track. What it changes is the size of a stream already there:
worst case, 96 kbps more inbound for the sharer and 96 kbps outbound per other
participant. In a room of five that is ~0.1 Mbps in and ~0.4 Mbps out, which next
to 3.3 Mbps of picture does not change how the machine is sized. The measurements
are in [chapter 12](12-requirements.md).

## Signaling is not encrypted by default

The client connects over `ws://` or `wss://`, and the default configuration is
`ws://`. The session token travels on that channel, so anything beyond local
development should be `wss://`.

This is an open finding, medium severity, recorded in
[chapter 13](13-security.md): the default should be to refuse `ws://` for any
host that is not loopback, rather than accepting it silently. Media itself is
unaffected — it crosses DTLS-SRTP whatever the signaling does.
