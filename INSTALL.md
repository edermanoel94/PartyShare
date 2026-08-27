# Install

From an empty machine to two people in a room.

There are two machines in the normal case, and they need different things built:

| | Machine | What you build | Section |
| --- | --- | --- | --- |
| **A** | The server — Linux, headless, no Qt, no sound card | `partyshare-server` against MongoDB | [1](#1-the-server) |
| **B** | Yours — Windows, Linux or macOS, with a desktop | `partyshare`, the client | [2](#2-the-client) |

In a hurry, or testing alone, [section 3](#3-everything-on-one-machine) puts both
on one machine in about five minutes.

Nothing has to be installed system-wide to try any of it. Binaries land in
`<build tree>/bin/`.

**MongoDB is the standard for the server.** Without it the server keeps accounts,
rooms, conversations and the audit log in memory and loses all four when it
stops — fine for a five minute test, and not a deployment. A build without the
option refuses to start when the database is turned on, rather than silently
falling back to memory.

---

## 1. The server

### 1.1 What it needs

| Tool | Minimum | |
| --- | --- | --- |
| CMake | 3.25 | |
| Ninja | 1.11 | or use `-G "Unix Makefiles"` |
| C++20 compiler | GCC 12, Clang 15, MSVC 2022 | |
| libdatachannel, OpenSSL, mongo-cxx-driver | — | all three through vcpkg |
| MongoDB | 7 | may live on this machine or another |

No Qt, no graphics server, no sound card. spdlog, nlohmann/json and GoogleTest
resolve themselves — found if installed, downloaded if not.

```sh
# Debian and Ubuntu
sudo apt install build-essential cmake ninja-build git curl zip unzip tar pkg-config

# Arch
sudo pacman -S --needed base-devel cmake ninja git curl zip unzip
```

### 1.2 vcpkg, at the pinned commit

```sh
./scripts/ci_vcpkg.sh
```

That checks out vcpkg into `.vcpkg` at the commit `vcpkg.json` names, and prints
the toolchain file. Use it rather than a vcpkg already on the machine: a ports
tree newer than the pinned version database fails with
`no version database entry for <package>`.

### 1.3 MongoDB

```sh
docker run -d --restart=unless-stopped -p 27017:27017 \
  -v partyshare-mongo:/data/db --name partyshare-mongo mongo:7
```

Or a managed instance, or a package — anything the server can reach. It holds one
document per account, per persistent room, per chat message and per
administrative action, none of which carries media, so it is not what sizes the
machine.

If it is not on this machine, the server needs latency rather than throughput: it
holds its own lock while it talks to the database, which is why the timeout
defaults to a deliberately short two seconds.

### 1.4 Build

```sh
cmake -S . -B build/server \
  -DDV_ENABLE_MONGO=ON -DVCPKG_MANIFEST_FEATURES=mongo \
  -DDV_BUILD_CLIENT=OFF -DDV_BUILD_TESTS=OFF \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake

cmake --build build/server
```

Four flags, and each one earns its place:

| Flag | Why |
| --- | --- |
| `-DDV_ENABLE_MONGO=ON` | Compiles the persistence layer |
| `-DVCPKG_MANIFEST_FEATURES=mongo` | Brings `mongo-cxx-driver`. It is a vcpkg *feature*, so a build without it needs nothing installed |
| `-DDV_BUILD_CLIENT=OFF` | What lets a machine with no Qt build this at all |
| `-DDV_BUILD_TESTS=OFF` | Faster. Drop it if you want to run the suite here |

**No preset is used, deliberately.** Every preset fixes its own `binaryDir`, so
`--preset linux-release` with the client turned off would overwrite the full tree
rather than sit beside it.

The first configure builds the vcpkg dependencies from source and takes minutes;
later ones reuse them. Rebuilding after a code change is
`cmake --build build/server`, and the executable target is `dv_server` if you
want to name it.

The binary is `build/server/bin/partyshare-server`.

### 1.5 The first administrator

```sh
./build/server/bin/partyshare-server \
  --database-uri=mongodb://127.0.0.1:27017 \
  --create-admin=ana:choose-a-password
```

It creates that administrator — or promotes an existing account and resets its
password — and then exits without listening. It is also the way back in when the
only administrator password is lost.

The password is visible in `ps` while the command runs, so change it from the
client afterwards. So is a database URI carrying credentials, which is why
`DV_DATABASE_URI` in the environment is the better place for one.

`--create-admin` needs a database. Against a server with none it answers
`--create-admin needs a database` rather than pretending.

### 1.6 Start it

```sh
./build/server/bin/partyshare-server \
  --database-uri=mongodb://127.0.0.1:27017 \
  --port=8080 \
  --ice-port-range=50000-50100 \
  --log-level=info
```

`--database-uri` turns the database on by itself — there is no second switch that
has to agree with the first.

Every option takes the `--key=value` form, with the value attached. A bare
`--key` is **refused** rather than quietly ignored: a server is started by a
script nobody is watching, and a typo that is silently dropped leaves it
listening on a default nobody chose. `partyshare-server --help` lists them all,
and [chapter 3](docs/03-configuration.md) is the complete reference.

The ones that matter here:

| Option | Default | |
| --- | --- | --- |
| `--port=PORT` | 8080 | |
| `--bind-address=ADDRESS` | 0.0.0.0 | Must not be `127.0.0.1` if clients are on other machines |
| `--ice-port-range=A-B` | — | **Read the next section before skipping this** |
| `--max-participants=N` | 5 | |
| `--database-uri=URI` | — | Turns persistence on |
| `--log-level=LEVEL` | info | `debug` while you are setting this up |

### 1.7 The firewall, and the one setting that is easy to miss

Two holes, not one:

```sh
sudo ufw allow 8080/tcp          # signaling
sudo ufw allow 50000:50100/udp   # ICE and media
```

On AWS, GCP or Azure the security group needs the same two entries; the host
firewall alone does not open them.

**`--ice-port-range` is what makes the second rule possible.** Without it the SFU
asks the system for an ephemeral port on every connection — 32768 to 60999 on
most Linux systems — and a firewall in front of it has nothing narrower to allow
than that entire range.

The symptom of getting this wrong is specific and misleading: the room, the
participant list, the chat and every signaling message work perfectly, and nobody
hears or sees anybody. It cost a real afternoon; entry 8 of
[chapter 15](docs/15-postmortems.md).

Size the range from the load, because the SFU binds one port per participant:
`max-participants` times the number of rooms running at once. A hundred ports
carries twenty full rooms. Both ends have to be given — half a range is refused
at startup. And `1024-65535` is worth nothing: libdatachannel reads it as its own
default and hands out an ephemeral port anyway.

The server logs the range it ended up with on startup, or warns that the ports
are ephemeral when none was set.

### 1.8 Keeping it running

```ini
# /etc/systemd/system/partyshare.service
[Unit]
Description=PartyShare signaling server and SFU
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=partyshare
WorkingDirectory=/opt/partyshare
Environment=DV_DATABASE_ENABLED=1
Environment=DV_DATABASE_URI=mongodb://127.0.0.1:27017
Environment=DV_DATABASE_NAME=partyshare
ExecStart=/opt/partyshare/bin/partyshare-server --port=8080 --ice-port-range=50000-50100
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

```sh
sudo systemctl daemon-reload
sudo systemctl enable --now partyshare
journalctl -u partyshare -f
```

The URI lives in `Environment=` rather than on the `ExecStart` line so that it
does not appear in `ps` for every user on the machine. `DV_DATABASE_ENABLED=1` is
needed alongside it: unlike `--database-uri`, naming a URI in the environment does
not turn persistence on by itself.

### 1.9 Check it is up

```sh
journalctl -u partyshare -n 40      # or the terminal it was started in
ss -lntp | grep 8080
```

A healthy startup names the port, the ICE range and the database. If the database
is unreachable the server **fails to start** and says so, rather than carrying on
in memory.

### 1.10 Without MongoDB, for a quick test only

A development account list, plain text passwords and all:

```json
[
  {"username": "ana", "password": "test-password", "display_name": "Ana", "role": "admin"},
  {"username": "bruno", "password": "test-password", "display_name": "Bruno"}
]
```

```sh
./build/server/bin/partyshare-server --port=8080 --users-file=dev-users.json
```

`role` is optional, and anything other than `"admin"` reads as an ordinary user.
Section 17 of [SPEC.md](SPEC.md) forbids this file in production and the server
warns about it on every startup that reads one.

---

## 2. The client

### 2.1 What it needs

Everything from 1.1, plus **Qt 6.5 or newer**. MongoDB is not involved: the
client never talks to a database.

**Linux**

```sh
sudo apt install cmake ninja-build g++ git qt6-base-dev libssl-dev   # Debian, Ubuntu
sudo pacman -S --needed cmake ninja gcc git qt6-base openssl         # Arch
./scripts/ci_vcpkg.sh
```

**macOS**

```sh
brew install cmake ninja qt
./scripts/ci_vcpkg.sh
```

**Windows**

```powershell
winget install -e --id Microsoft.VisualStudio.2022.BuildTools `
  --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install -e --id Kitware.CMake
winget install -e --id Ninja-build.Ninja
winget install -e --id Python.Python.3.12
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.7.3 win64_msvc2019_64 -O C:\Qt
bash scripts/ci_vcpkg.sh
```

Qt comes from `aqtinstall` because the Qt installer wants an account.

**Every `cmake` and `ctest` command on Windows has to run from a shell that has
sourced `vcvars64.bat`**, because the `windows-*` presets name `cl` as the
compiler:

```
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
```

Configuring from a shell that has not is worse than a clean failure. It dies on
`The CXX compiler identification is unknown` and leaves behind a `CMakeCache.txt`
with an **empty** `CMAKE_CXX_FLAGS`. Reconfiguring from the right shell does not
repair it — the cache is reused, the `/EHsc` CMake normally injects never appears,
and the next build dies on `warning C4530 ... treated as an error` coming out of
`<chrono>`, in a file that has nothing wrong with it. **Delete the whole build
directory**; `--fresh` from some other shell is not enough.

### 2.2 Build

Presets do the work here, one per platform.

**Linux**

```sh
cmake --preset linux-release \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset linux-release
```

**macOS**

```sh
cmake --preset macos-arm64-release \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
cmake --build --preset macos-arm64-release
```

**Windows**, from the vcvars shell:

```
cmake --preset windows-release ^
  -DCMAKE_TOOLCHAIN_FILE=%CD%\.vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64
cmake --build --preset windows-release
```

To build the client without the server — no libdatachannel, no OpenSSL — add
`-DDV_BUILD_SERVER=OFF`. To build it without Qt at all, `-DDV_BUILD_CLIENT_UI=OFF`
keeps the client core and drops the interface.

### 2.3 Screen share and voice

**The build above has no media layer.** The interface, the login, the rooms and
the chat all work, and there is no call: `create_media_session` fails with
`media_unavailable`.

Turning it on needs a libwebrtc tree, and where that comes from depends on the
platform.

**Windows** — `cmake/Findlibwebrtc.cmake` fetches a published tree by itself, so
one flag is the whole change:

```
cmake --preset windows-release ^
  -DCMAKE_TOOLCHAIN_FILE=%CD%\.vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64 ^
  -DDV_BUILD_CLIENT_MEDIA=ON
cmake --build --preset windows-release
```

**Linux and macOS** — build libwebrtc first. Set aside the time: the checkout is
over 30 GB, the build takes tens of minutes, and linking wants 16 GB of memory.

```sh
scripts/build_webrtc.sh                      # once, into ~/.cache/partyshare/webrtc/dist

cmake -S . -B build/media \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DDV_BUILD_CLIENT_MEDIA=ON \
  -DDV_WEBRTC_ROOT=$HOME/.cache/partyshare/webrtc/dist \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build/media
```

Two things that cost time if they are learned the hard way:

- **The media layer needs a release tree.** A Debug configuration does not link
  against this libwebrtc.
- **Point `DV_WEBRTC_ROOT` at the tree carrying the `DV_EXTERNAL_SSL` marker.** A
  tree with BoringSSL bundled cannot be linked with libdatachannel, and getting it
  wrong produces a wall of `LNK2005` that reads like an unsolvable OpenSSL
  conflict. `cmake/Findlibwebrtc.cmake` documents which is which, and
  [chapter 7](docs/07-webrtc-toolchain.md) is the whole story.

Why that separate build has to exist at all is section 5 of
[chapter 7](docs/07-webrtc-toolchain.md).

### 2.4 Point it at the server

```sh
./build/linux-release/bin/partyshare --signaling-url=ws://192.168.1.10:8080
```

The default is `ws://127.0.0.1:8080`, which only serves a server on the same
machine.

A flag is fine for one run and useless for a machine somebody else uses: an
installed client starts from a shortcut, and a shortcut carries no arguments. So
the client reads a `config.ini` it is not told about, in two places, and the
second wins:

| Order | Windows | Linux | macOS |
| --- | --- | --- | --- |
| 1. The machine's | Beside `partyshare.exe` | Beside the binary | Inside the `.app` |
| 2. This user's | `%LOCALAPPDATA%\partyshare\` | `$XDG_CONFIG_HOME/partyshare/` | `~/Library/Application Support/partyshare/` |

Neither has to be created by hand — the installer writes the first and the client
writes the second on its first run, both fully commented and entirely inert. One
uncommented line is the whole edit:

```ini
[network]
signaling_url = ws://192.168.1.10:8080
```

Edit the second one where there is a choice: the first belongs to the installer
and is replaced on the next upgrade, taking the address of your server with it.

The client prints which of those files it read and which it did not find on every
startup, at `info`. That log line is the answer to "I put the address in and it
still connects to localhost", which is almost always a file written one directory
away from the one being read.

Every other setting — resolution, frame rate, bitrate, devices, shared screen
sound — is in [chapter 3](docs/03-configuration.md), and most of them are in the
Settings dialog, which writes them back into this user's file.

### 2.5 Log in

Use one of the accounts created in 1.5, create a room, and give the code to
whoever else is connecting to that same server. The client has no `--username`
flag: logging in happens on the login screen.

`partyshare --help` lists the rest: `--input-device`, `--output-device`,
`--codec`, `--fps`, `--log-level`, `--log-file`.

### 2.6 Two clients on one machine

Nothing stops it — start the binary twice and log in as different accounts. It is
the quickest way to see a room work without a second computer.

**Give each one its own profile**, or the two fight over one `config.ini`. The
Settings dialog always writes to this user's file and ignores `--config`, so the
separation has to be in the environment. On Windows the client reads
`LOCALAPPDATA`:

```powershell
$env:LOCALAPPDATA = "$env:TEMP\partyshare-ana"
Start-Process .\build\windows-release\bin\partyshare.exe

$env:LOCALAPPDATA = "$env:TEMP\partyshare-bruno"
Start-Process .\build\windows-release\bin\partyshare.exe
```

Each then gets its own `config.ini`, its own log and its own crash folder. On
Linux and macOS the same trick uses `XDG_CONFIG_HOME` and `HOME`.

Windows also needs `C:\Qt\6.7.3\msvc2019_64\bin` on `PATH` for a client started
outside the shell that built it — see 5.

---

## 3. Everything on one machine

The five minute version, with persistence, for trying it out:

```sh
./scripts/ci_vcpkg.sh
docker run -d -p 27017:27017 --name partyshare-mongo mongo:7

cmake --preset linux-release \
  -DDV_ENABLE_MONGO=ON -DVCPKG_MANIFEST_FEATURES=mongo \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset linux-release

./build/linux-release/bin/partyshare-server \
  --database-uri=mongodb://127.0.0.1:27017 --create-admin=ana:test-password
./build/linux-release/bin/partyshare-server \
  --database-uri=mongodb://127.0.0.1:27017 --port=8080 &

./build/linux-release/bin/partyshare
```

One tree, both binaries, and the preset is fine here because the client is being
built too. No `--ice-port-range` is needed: there is no firewall between a machine
and itself.

Without Docker, swap the database for `--users-file=dev-users.json`, as in 1.10.

---

## 4. Check it works

```sh
ctest --preset linux-release                            # everything
ctest --test-dir build/linux-release -L unit            # unit only
ctest --test-dir build/linux-release -L integration     # integration only
ctest --test-dir build/media -L media                   # needs the media layer
```

The integration tests start a real server on an ephemeral port and connect real
WebSocket clients, so a passing suite means the server half is genuinely working.

The `mongo` labelled tests skip themselves unless `DV_TEST_MONGO_URI` is set, so a
machine with no database still runs the whole suite:

```sh
DV_TEST_MONGO_URI=mongodb://127.0.0.1:27017 ctest --test-dir build/server -L mongo
```

They leave behind databases named `partyshare_test_*`, one per test and per run,
which are safe to drop.

---

## 5. When it does not work

| Symptom | Cause and fix |
| --- | --- |
| `unable to find a build program corresponding to "Ninja"` | Ninja is not on the path. Install it, or use `--preset linux-make` |
| `Could NOT find Qt6` | Pass `-DCMAKE_PREFIX_PATH=<qt>`, or build without it: `-DDV_BUILD_CLIENT_UI=OFF` |
| libdatachannel or OpenSSL not found | The vcpkg toolchain file was not passed to the **configure** step. Run `./scripts/ci_vcpkg.sh` and add `-DCMAKE_TOOLCHAIN_FILE=...` |
| `no version database entry for <package>` | A vcpkg other than the pinned one. `scripts/ci_vcpkg.sh` checks out the commit `vcpkg.json` names; use that tree |
| mongocxx not found, with `DV_ENABLE_MONGO=ON` | `-DVCPKG_MANIFEST_FEATURES=mongo` was left off. The driver is a vcpkg feature, not a plain dependency |
| The server exits saying it was built without MongoDB | Deliberate. A server told to persist that quietly did not is one whose accounts vanish at the next restart. Rebuild with `-DDV_ENABLE_MONGO=ON` |
| `--create-admin needs a database` | That option only works against MongoDB. Pass `--database-uri=...` as well |
| `cl is not recognized` on Windows | The shell has not sourced `vcvars64.bat` |
| `warning C4530 ... treated as an error` from `<chrono>` | A cache poisoned by a configure without vcvars. Delete the whole build directory, then reconfigure from the right shell |
| `Qt6Core.dll was not found` on Windows | The build tree is not self-contained: vcpkg copies its own libraries next to the binaries, Qt is not among them. Keep `C:\Qt\6.7.3\msvc2019_64\bin` on `PATH`, or work from an install tree, below |
| A wall of `LNK2005` when building with media | The wrong libwebrtc tree. Point `DV_WEBRTC_ROOT` at the one with the `DV_EXTERNAL_SSL` marker |
| The room works and nobody hears anybody | `--ice-port-range` unset, or the UDP range not open in the firewall. Section 1.7 |
| Nobody appears in the room | Both clients have to reach the *same* server. Check `--signaling-url` on each, and that `--bind-address` is not `127.0.0.1` |
| Login refused | The account does not exist. Create it with `--create-admin`, from `tools/dbadmin`, or from the admin panel |
| "This build was compiled without audio and video" | The client was built without `-DDV_BUILD_CLIENT_MEDIA=ON`. Section 2.3 |
| The server exits on an unknown option | Deliberate. Options are `--key=value`; a detached `--key value` does not parse |

An install tree, on Windows, carries everything the program loads:

```
cmake --install build\windows-release --prefix stage
windeployqt --release stage\bin\partyshare.exe
```

`windeployqt` brings the Qt half and the install rule brings the vcpkg half.
Neither covers what the other does.

Crash reports are written to `$XDG_STATE_HOME/partyshare/crashes` on Linux,
`~/Library/Logs` on macOS and `%LOCALAPPDATA%` on Windows, the ten most recent
kept. Each carries the build, the signal and a backtrace.

---

## Where to go next

| | |
| --- | --- |
| [The book](docs/README.md) | Everything, in reading order |
| [Configuration](docs/03-configuration.md) | Every setting, and which copy wins |
| [Server and database](docs/04-server-and-database.md) | Ports, bandwidth, what the database holds |
| [Administration](docs/05-administration.md) | Roles, restrictions and the audit log |
| [tools/dbadmin](tools/dbadmin/README.md) | Managing accounts from a terminal, with no server running |
| [Build](docs/02-build.md) | Every option and environment variable, media debugging |
| [Requirements](docs/12-requirements.md) | The hardware each side needs |
