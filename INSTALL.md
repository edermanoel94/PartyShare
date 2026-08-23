# Install

Getting PartyShare built and running, on one machine, in as few steps as possible.
This page is the short path; [docs/build.md](docs/build.md) is the long one, with every option, every
environment variable and the media layer.

The whole thing is two binaries:

| Binary | What it is | Where it lands |
| --- | --- | --- |
| `partyshare-server` | Signaling server and SFU | `build/<preset>/bin/` |
| `partyshare` | Desktop client, Qt 6 | `build/<preset>/bin/` |

Both come out of a single build. Nothing has to be installed system-wide to try it.

## 1. Prerequisites

| Tool | Minimum | Needed by |
| --- | --- | --- |
| CMake | 3.25 | everything |
| Ninja | 1.11 | every preset except `linux-make` |
| C++20 compiler | MSVC 2022, GCC 12, Clang 15 | everything |
| Qt | 6.5 | the client only |
| libdatachannel and OpenSSL | — | the server only, through vcpkg |

spdlog, nlohmann/json and GoogleTest are resolved automatically: found if installed, downloaded if not.
There is no step for them.

### Linux

```sh
# Arch
sudo pacman -S --needed cmake ninja gcc git qt6-base openssl

# Debian and Ubuntu
sudo apt install cmake ninja-build g++ git qt6-base-dev libssl-dev
```

libdatachannel is rarely packaged, so it comes from vcpkg like everywhere else:

```sh
./scripts/ci_vcpkg.sh      # checks out the pinned vcpkg into .vcpkg, prints the toolchain file
```

### macOS

```sh
brew install cmake ninja qt
./scripts/ci_vcpkg.sh
```

### Windows

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

The `windows-*` presets name `cl` as the compiler, so configure, build and test all have to run from a
Developer Command Prompt, or from a shell that has sourced `vcvars64.bat`:

```
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
```

## 2. Build

Pick the preset for the platform. Everything else is identical.

**Linux**

```sh
cmake --preset linux-release -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build --preset linux-release
```

**macOS**

```sh
cmake --preset macos-arm64-release \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
cmake --build --preset macos-arm64-release
```

**Windows**

```
cmake --preset windows-release ^
  -DCMAKE_TOOLCHAIN_FILE=%CD%\.vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64
cmake --build --preset windows-release
```

The first configure builds the vcpkg dependencies from source and takes minutes; later ones reuse them.
Binaries land in `build/<preset>/bin/`.

Other presets: `linux-debug`, `linux-asan`, `linux-make` (Unix Makefiles, for machines without Ninja),
`windows-debug`, `windows-asan`, `macos-arm64-debug`, `macos-arm64-asan`, `macos-x64-release`.

### Building only one half

| Situation | Flag |
| --- | --- |
| No Qt installed | `-DDV_BUILD_CLIENT_UI=OFF` keeps the client core, drops the interface |
| Server only | `-DDV_BUILD_CLIENT=OFF` |
| Client only | `-DDV_BUILD_SERVER=OFF` |
| Faster iteration | `-DDV_BUILD_TESTS=OFF` |

#### Only the server

Two ways, and which one is right depends on whether the machine has Qt on it at all.

In a build tree that already exists, name the target. The executable target is `dv_server`;
`partyshare-server` is only the name it is written under, so `--target partyshare-server` is not a thing:

```sh
cmake --build build/linux-release --target dv_server
```

That builds the shared library, the server library and the executable, and stops: no Qt, no client, no
tests. It is the one to use while working on the server, because it is incremental against a tree that is
already configured.

On a machine with no Qt, or a CI runner that has no reason to install it, configure a tree that never
mentions the client:

```sh
cmake -S . -B build/server-only \
  -DDV_BUILD_CLIENT=OFF -DDV_BUILD_TESTS=OFF \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build/server-only
```

Windows, from a shell that has sourced `vcvars64.bat`:

```
cmake -S . -B build\server-only -G Ninja ^
  -DCMAKE_BUILD_TYPE=RelWithDebInfo ^
  -DDV_BUILD_CLIENT=OFF -DDV_BUILD_TESTS=OFF ^
  -DCMAKE_TOOLCHAIN_FILE=%CD%\.vcpkg\scripts\buildsystems\vcpkg.cmake
cmake --build build\server-only
```

The presets are not used here on purpose: each one fixes its own `binaryDir`, so `--preset windows-release`
with the client turned off would overwrite the full tree rather than sit beside it.

Either way the binary lands in `<build tree>/bin/partyshare-server`, and section 3 runs it from there.

## 3. Run the server

The server needs accounts before anyone can log in. For a first run, a development account list is enough.
Write `dev-users.json`:

```json
[
  {"username": "ana", "password": "test-password", "display_name": "Ana", "role": "admin"},
  {"username": "bruno", "password": "test-password", "display_name": "Bruno"}
]
```

Then start it:

```sh
./build/linux-release/bin/partyshare-server \
  --port=8080 --log-level=debug --users-file=dev-users.json
```

On Windows: `build\windows-release\bin\partyshare-server.exe --port=8080 --users-file=dev-users.json`.

`role` is optional and anything other than `"admin"` reads as an ordinary user. That file keeps passwords in
plain text and exists only so the MVP has users: the server warns about it at every startup, and section 17
of [SPEC.md](SPEC.md) forbids it in production.

Every option takes the `--key=value` form, with the value attached; a bare `--key` is refused rather than
quietly ignored. `partyshare-server --help` lists all of them. The ones that matter here:

| Option | Default | Effect |
| --- | --- | --- |
| `--port=PORT` | 8080 | Port to listen on |
| `--bind-address=ADDRESS` | 0.0.0.0 | Address to listen on |
| `--max-participants=N` | 5 | Participants per room |
| `--ice-port-range=A-B` | — | UDP range the SFU binds media in, one port per participant. Unset, the system picks an ephemeral port and the firewall has to allow the whole ephemeral range. See [requirements.md](docs/requirements.md) |
| `--users-file=PATH` | — | Development account list |
| `--config=PATH` | — | Configuration file, read before anything else |
| `--log-level=LEVEL` | info | `trace`, `debug`, `info`, `warn`, `error`, `fatal`, `off` |

### With MongoDB, optional

Persistence for accounts, roles, rooms and the audit log. Off by default; without it the server keeps all
four in memory and behaves as it did before persistence existed.

It needs both a CMake option and the vcpkg feature that brings the driver, so it is a separate build tree:

```sh
cmake -S . -B build/mongo -DDV_ENABLE_MONGO=ON -DVCPKG_MANIFEST_FEATURES=mongo \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build/mongo
```

Then a database, an administrator, and the server:

```sh
docker run -d -p 27017:27017 --name partyshare-mongo mongo:7

./build/mongo/bin/partyshare-server \
  --database-uri=mongodb://127.0.0.1:27017 --create-admin=ana:choose-a-password

./build/mongo/bin/partyshare-server \
  --database-uri=mongodb://127.0.0.1:27017 --port=8080
```

`--database-uri` turns the database on by itself, so there is no second switch that has to agree with the
first. `--create-admin` creates that administrator, or promotes an existing account and resets its password,
and then exits; it is also the way back in when the only administrator password is lost. The password is
visible in `ps` while it runs, so change it from the client afterwards.

A build without `-DDV_ENABLE_MONGO` refuses to start when the database is on, rather than falling back to
memory: a server that was told to persist and quietly did not is one whose accounts vanish at the next restart.

[tools/dbadmin](tools/dbadmin/README.md) does the same job from a terminal, without a running server.

## 4. Run the client

```sh
./build/linux-release/bin/partyshare
```

It connects to `ws://127.0.0.1:8080` by default, which is where the server above is listening. To point it
somewhere else:

```sh
./build/linux-release/bin/partyshare --signaling-url=ws://192.168.1.10:8080
```

Log in with one of the accounts from `dev-users.json`, create a room, and share the code with whoever else
is connecting to that same server. `partyshare --help` lists the rest: `--input-device`, `--output-device`,
`--codec`, `--fps`, `--log-level`, `--log-file`.

### Writing the address down instead of typing it

A flag is fine for one run and useless for a machine somebody else uses: an installed client is started
from a shortcut, and a shortcut carries no arguments. So the client looks for a `config.ini` on its own,
in two places, and neither has to exist:

| Order | Windows | Linux | macOS |
| --- | --- | --- | --- |
| 1. The machine's | Beside `partyshare.exe` | Beside the binary | Beside the binary |
| 2. This user's | `%LOCALAPPDATA%\partyshare\` | `$XDG_CONFIG_HOME/partyshare/` | `~/Library/Application Support/partyshare/` |

The second wins over the first. That is the split that matters: whoever installs a machine writes the first
one and it answers for every account on it, and a person overrides it in the second without being asked for
an administrator password.

One line is the whole file:

```ini
[network]
signaling_url = ws://192.168.1.10:8080
```

Neither file has to be created by hand.
The installer drops a fully commented `config.ini` beside the executable, and the client writes a second copy of the same file into this user's directory the first time it runs.
Both are inert as they ship — every line is commented out — so uncommenting one line is the whole edit.

Edit the second one where there is a choice.
The first belongs to the installer and is replaced on the next upgrade, which would take the address of the server with it; the second is never touched by an installer, and on macOS the first one lives inside the signed `.app` where editing it breaks the signature.
It is also where the client saves what you pick in **Settings**: the microphone, the output device, the screen resolution and frame rate, and the two ends of the bitrate range are written into this user's `config.ini` as you choose them, so the choice is still there next time.
The monitor is the one thing on that screen that is not saved, because it is which screen to share next rather than a setting.

**Resolution** and **Frame rate** are `video.width`, `video.height` and `video.fps`, and the dialog offers 720p and 1080p at 30 or 60 fps.
Both take effect at once, including mid-call: a share that is running restarts on the same monitor, which costs a stutter and no renegotiation.
30 fps is right for a document or an editor; 60 is for what 30 makes unwatchable, which is scrolling, a terminal redrawing, anything animated.
The resolution is a ceiling and not the size sent — a monitor is fitted inside it with its shape kept, so 1080p on a 3440x1440 ultrawide sends 1920x802 and never a stretched 1920x1080, and a monitor smaller than the box is sent untouched rather than upscaled.

Raising either asks more of the encoder, and the dialog says so when the maximum bitrate below is lower than what the choice is worth — 1080p at 60 is worth around four times what 720p at 30 is.
It says it rather than doing it: a ceiling you set to fit your link is not one the client should raise behind your back.
The configuration is free to name a size or a rate the dialog does not offer, `width = 2560` is perfectly valid, and the dialog then shows that as a row of its own instead of quietly rounding you down to 720p.

The minimum bitrate the dialog offers stops at `video.floor_bitrate_kbps`, which defaults to 300 kbps.
The floor is how far congestion control may squeeze the picture when the link cannot carry the minimum, and a configuration whose floor sits above its minimum is one the client refuses to start on — so the dialog will not let you save one.
Lower both together if you need to go under 300.

Sections and keys are the same names the JSON form uses, so nothing has to be learned twice. Comments start
with `;` or `#`. A key the client does not know is a startup error naming the line, rather than a line that
quietly does nothing:

```text
configuration error [invalid_ini]: line 2: no such setting as [network] signalling_url
```

The client prints which of these files it read and which it did not find, every startup, at `info`. That
log line is the answer to "I put the address in and it still connects to localhost", which is almost always
a file written one directory away from the one being read.

Configuration precedence is: built-in defaults, the machine's `config.ini`, this user's `config.ini`, then
`DV_`-prefixed environment variables, then the command line. `--config=PATH` takes either `.ini` or `.json`
and **replaces** both discovered files rather than joining them. The complete list of variables is in
`shared/src/config/config.cpp`.

### Two clients on one machine

Nothing stops it: start the binary twice and log in as `ana` in one and `bruno` in the other. That is the
quickest way to see a room work without a second computer.

### Screen share and voice

The client builds without libwebrtc by default, and in that mode the interface, the login, the rooms and the
signaling all work, but there is no call: `create_media_session` fails with `media_unavailable`.

Media needs the libwebrtc tree that `scripts/build_webrtc.sh` produces, and then a build that points at it:

```sh
cmake -S . -B build/media \
  -DDV_BUILD_CLIENT_MEDIA=ON \
  -DDV_WEBRTC_ROOT=$HOME/.cache/partyshare/webrtc/dist
cmake --build build/media
```

Why that separate build exists is section 5 of [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md).

## 5. Check it works

```sh
ctest --preset linux-release                            # everything
ctest --test-dir build/linux-release -L unit            # unit only
ctest --test-dir build/linux-release -L integration     # integration only
```

The integration tests start a real server on an ephemeral port and connect real WebSocket clients, so a
passing suite means the server half is genuinely working.

The `mongo` labelled tests skip themselves unless `DV_TEST_MONGO_URI` is set, so a machine with no database
still runs the whole suite:

```sh
DV_TEST_MONGO_URI=mongodb://127.0.0.1:27017 ctest --test-dir build/mongo -L mongo
```

They leave behind databases named `partyshare_test_*`, one per test and per run, which are safe to drop.

## 6. When it does not work

| Symptom | Cause and fix |
| --- | --- |
| `unable to find a build program corresponding to "Ninja"` | Ninja is not on the path. Install it, or use `--preset linux-make`. |
| `Could NOT find Qt6` | Qt is not where CMake looks. Pass `-DCMAKE_PREFIX_PATH=<qt>`, or build without it: `-DDV_BUILD_CLIENT_UI=OFF`. |
| libdatachannel or OpenSSL not found | The vcpkg toolchain file was not passed to the configure step. Run `./scripts/ci_vcpkg.sh` and add `-DCMAKE_TOOLCHAIN_FILE=...`. |
| `no version database entry for <package>` | A vcpkg other than the pinned one. `scripts/ci_vcpkg.sh` checks out the commit `vcpkg.json` names; use that tree. |
| `Qt6Core.dll was not found` on Windows | The build tree is not self-contained: vcpkg copies its own libraries next to the binaries, Qt is not among them. Keep `C:\Qt\6.7.3\msvc2019_64\bin` on `PATH`, or work from an install tree, below. |
| `cl is not recognized` on Windows | The shell has not sourced `vcvars64.bat`. |
| Nobody appears in the room | Both clients have to reach the *same* server. Check `--signaling-url` on each, and that `--bind-address` is not `127.0.0.1` when they are on different machines. |
| Login refused | The account is not in `--users-file`, or the server was started without one. It warns at startup when the file is missing. |
| `--create-admin needs a database` | That option only works against MongoDB, so pass `--database-uri=...` as well. |
| The server exits on an unknown option | Deliberate. Options are `--key=value`; a detached `--key value` does not parse, and a typo that is silently ignored leaves a server listening on a default nobody chose. |

An install tree, on Windows, carries everything the program loads:

```
cmake --install build\windows-release --prefix stage
windeployqt --release stage\bin\partyshare.exe
```

`windeployqt` brings the Qt half and the install rule brings the vcpkg half. Neither covers what the other does.

Crash reports are written to `$XDG_STATE_HOME/partyshare/crashes` on Linux, `~/Library/Logs` on macOS and
`%LOCALAPPDATA%` on Windows, the ten most recent kept. Each carries the build, the signal and a backtrace.

## Where to go next

| Document | Subject |
| --- | --- |
| [docs/build.md](docs/build.md) | Every option and environment variable, media debugging, virtual audio, network impairment |
| [docs/requirements.md](docs/requirements.md) | The hardware the client and the server need |
| [docs/webrtc-toolchain.md](docs/webrtc-toolchain.md) | Building libwebrtc, and why it is a separate build |
| [tools/dbadmin/README.md](tools/dbadmin/README.md) | Managing accounts and reading the audit log from a terminal |
| [docs/release.md](docs/release.md) | Cutting a release, and what each platform produces |
