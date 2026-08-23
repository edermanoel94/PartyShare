# Build

The hardware needed to run the client and the server is in [requirements.md](requirements.md).
This page is about the tooling needed to compile them.

## Prerequisites

| Tool | Minimum version | Note |
| --- | --- | --- |
| CMake | 3.25 | Presets version 6 |
| Ninja | 1.11 | The default generator for every preset |
| Qt | 6.5 | Client only |
| Compiler | MSVC 2022, GCC 12, Clang 15 | C++20 |

Dependencies resolved automatically: spdlog, nlohmann/json and GoogleTest.
Each is looked up with `find_package` first, and only downloaded through `FetchContent` when it is not installed.
That allows using vcpkg, distribution packages, or nothing at all, without touching the CMake.

Two dependencies have to really exist, because they are not header only:

| Dependency | Used by | Where to get it |
| --- | --- | --- |
| libdatachannel | Server: signaling WebSocket (M2) and SFU (M4) | vcpkg or a distribution package |
| OpenSSL | Server: password hashing and tokens | vcpkg or a distribution package |

Without them, use `-DDV_BUILD_SERVER=OFF` to build only the client and the shared tests.

On Linux, whoever links libwebrtc also needs the X11, glib, gbm and libdrm headers.
They are not our dependencies: they come from libwebrtc's own screen capture, which talks to the XDG portal over GDBus and imports frames as DMA-BUF.

```sh
# Arch
sudo pacman -S --needed libx11 libxext libxfixes libxdamage libxrandr libxcomposite libxtst glib2 mesa libdrm

# Debian and Ubuntu
sudo apt install libx11-dev libxext-dev libxfixes-dev libxdamage-dev libxrandr-dev \
  libxcomposite-dev libxtst-dev libglib2.0-dev libgbm-dev libdrm-dev
```

With vcpkg:

```sh
cmake --preset linux-release -DCMAKE_TOOLCHAIN_FILE=$VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake
```

## The client media layer

The client builds without libwebrtc by default.
In that mode everything works except audio: `create_media_session` fails with `media_unavailable`, and the interface and signaling stay intact.

Building with media requires the tree that `scripts/build_webrtc.sh` produces, for the reasons in section 5 of [webrtc-toolchain.md](webrtc-toolchain.md):

```sh
cmake -S . -B build/media \
  -DDV_BUILD_CLIENT_MEDIA=ON \
  -DDV_WEBRTC_ROOT=$HOME/.cache/partyshare/webrtc/dist
cmake --build build/media
```

Two environment variables help when debugging media:

| Variable | Effect |
| --- | --- |
| `DV_WEBRTC_LOG` | `warning`, `info` or `verbose`. Turns on libwebrtc's internal log, which is the only way to see why a device did not open or a codec was refused. |
| `DV_AUDIO_NULL_DEVICE` | Uses a null audio device instead of the system one. Useful for machines without a sound card and for CI. Nothing is captured and nothing is played. |
| `DV_VIRTUAL_INPUT_DEVICE` | Name of the capture device the media tests should use. Exported by `scripts/virtual_audio.sh`, described below. |
| `DV_VIRTUAL_OUTPUT_DEVICE` | The same, for playback. |
| `DV_DUMP_SDP` | Makes the SFU log every offer it sends. It is how you answer "was this negotiated?" about codecs, extensions and feedback. |
| `DV_DISABLE_HARDWARE_ENCODER` | Forces the software encoder even on a machine with a capable card. Useful for comparing the two and for working around a problematic driver. |
| `DV_CRASH_DIRECTORY` | Where crash reports are written. The default is the platform state directory. |
| `DV_CRASH_REPORTS` | `0` turns crash reporting off entirely. |
| `DV_DATABASE_ENABLED` | `1` turns MongoDB persistence on. A build without `DV_ENABLE_MONGO` refuses to start rather than falling back to memory. |
| `DV_DATABASE_URI` | Connection string, default `mongodb://127.0.0.1:27017`. |
| `DV_DATABASE_NAME` | Database to use, default `partyshare`. |
| `DV_DATABASE_TIMEOUT_MS` | How long to wait for the database, default 2000. Deliberately short: the store is called with the server's lock held. |
| `DV_TEST_MONGO_URI` | Where the `mongo` labelled tests find a database. Unset, they skip. |

### Virtual audio device

`DV_AUDIO_NULL_DEVICE` makes the negotiation tests pass on a machine without a sound card, but a null device captures nothing.
Anything that depends on real audio, which is most of M5, still cannot be verified.

`scripts/virtual_audio.sh` solves that by creating a virtual sound card on top of PulseAudio, with a tone playing into the microphone:

```sh
eval "$(scripts/virtual_audio.sh start)"
ctest --test-dir build/media -L media --output-on-failure
scripts/virtual_audio.sh stop
```

The `eval` exports `DV_VIRTUAL_INPUT_DEVICE` and `DV_VIRTUAL_OUTPUT_DEVICE`, and the media tests then pick those devices explicitly.

On a machine that already has a sound server the script attaches to it and leaves the default devices alone, so running the tests does not steal the speakers from whoever is at the keyboard.
With no sound server at all, which is the case on a CI runner, it starts a private one and only then makes the virtual devices the defaults.

The virtual microphone is a `module-remap-source` over the monitor of a null sink, rather than the monitor directly: libwebrtc's PulseAudio backend ignores every source that monitors a sink when it enumerates capture devices, and a device it does not list is a device it does not open.

### Impaired network

The media tests degrade the network from the inside, through the client's own sockets, and for that reason they run without privileges and without any setup.
To degrade the network for real, in the operating system's queues, there is `scripts/netem.sh`:

```sh
sudo scripts/netem.sh apply lossy      # 5% loss, the number from section 22
sudo scripts/netem.sh apply distant    # 150 ms latency with jitter
sudo scripts/netem.sh apply awful      # both, plus reordering
sudo scripts/netem.sh clear
```

It needs root and the `sch_netem` module, which does not load on a machine whose kernel was upgraded without a reboot.
The script says so, instead of letting `tc` answer that the qdisc is unknown.
`DV_NETEM_DRY_RUN=1` shows the command that would run, without running it and without root.

The difference between the two paths, and the numbers measured with each, are in [benchmarks.md](benchmarks.md).

### Hardware encoder

The screen share is encoded by the graphics card when there is one, and by the processor when there is not.
On Linux the backend is NVENC, on by default and switchable off with `-DDV_HARDWARE_ENCODER_NVENC=OFF`.

Nothing is linked: `libnvidia-encode.so.1` and `libcuda.so.1` are opened at runtime, so the same binary runs on a machine without an NVIDIA card.
The API header is in `third_party/nvcodec`, with its provenance alongside.

Which encoder is running shows up in the log at every metrics interval, read from libwebrtc's statistics:

```text
Video: 1280x720 at 30.0 fps, up 835 kbps, estimate 1621 kbps, 0 frames dropped, encoder OpenH264
```

When there is no hardware, the reason is stated once, when the engine is created, and it is worth reading before going looking for the problem in the driver:

```text
Media: no hardware encoding (the NVIDIA driver does not match its own kernel module, which is
what an upgrade without a reboot leaves behind), the screen is encoded in software
```

`DV_DISABLE_HARDWARE_ENCODER=1` forces software even with a capable card, which is how the two get compared.

### Crash reports

A crash that leaves nothing behind turns into a report that says "it closed by itself".
The client and the server install a handler for the signals a crash arrives through, and write a file with the build, the signal and the backtrace:

```text
partyshare crash report
application: partyshare
version: 0.1.0
built: Aug 19 2026 16:36:58

when: 1787168270 seconds since the epoch, readable with: date -d @1787168270
signal: SIGSEGV, a read or write through a bad pointer

backtrace:
./build/media/bin/partyshare(+0x145437) [0x5572fb6c8437]
/usr/lib/libQt6Core.so.6(_ZN10QEventLoop4execE...+0x193) [0x7f6888391983]
```

The default is `$XDG_STATE_HOME/partyshare/crashes` on Linux, `~/Library/Logs` on macOS and `%LOCALAPPDATA%` on Windows, and the ten most recent are kept.
Each line is `binary(+offset) [address]`; the offset is what `addr2line -Cfe <binary> <offset>` turns into a file and a line.
Names coming from libraries come out mangled, because demangling allocates memory and a signal handler cannot: `c++filt` sorts it out.

The process still dies the way it would have died, so a configured core dump is still produced and the exit code still says what killed the program.

## Building

```sh
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

Available presets:

```text
linux-debug     linux-release     linux-asan     linux-make
windows-debug   windows-release   windows-asan
macos-arm64-debug   macos-arm64-release   macos-arm64-asan   macos-x64-release
```

The `linux-make` preset uses Unix Makefiles, for machines without Ninja.

Binaries land in `build/<preset>/bin/`.

### macOS

Homebrew has CMake, Ninja and Qt, and does not have libdatachannel, so that one comes from vcpkg.
`scripts/ci_vcpkg.sh` checks out the commit `vcpkg.json` pins and prints the toolchain file to point CMake at.

```sh
brew install cmake ninja qt
./scripts/ci_vcpkg.sh
cmake --preset macos-arm64-release \
  -DCMAKE_TOOLCHAIN_FILE=$PWD/.vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_PREFIX_PATH=$(brew --prefix qt)
cmake --build --preset macos-arm64-release
ctest --preset macos-arm64-release
```

The client is a bundle, `build/macos-arm64-release/bin/partyshare.app`, and the server is a plain binary next to it.
The first configure builds the vcpkg dependencies from source, which takes minutes; later ones reuse them.

### Windows

winget has the compiler, CMake and Ninja. Qt comes from `aqtinstall`, because the Qt installer wants an account, and
libdatachannel from vcpkg as everywhere else.

```powershell
winget install -e --id Microsoft.VisualStudio.2022.BuildTools `
  --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install -e --id Kitware.CMake
winget install -e --id Ninja-build.Ninja
winget install -e --id Python.Python.3.12
python -m pip install aqtinstall
python -m aqt install-qt windows desktop 6.7.3 win64_msvc2019_64 -O C:\Qt
```

The `windows-*` presets name `cl` as the compiler, so configure, build and test all have to run somewhere it exists:
a Developer Command Prompt, or any shell that has sourced `vcvars64.bat`.

```
"C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars64.bat"
bash scripts/ci_vcpkg.sh
cmake --preset windows-release ^
  -DCMAKE_TOOLCHAIN_FILE=%CD%\.vcpkg\scripts\buildsystems\vcpkg.cmake ^
  -DCMAKE_PREFIX_PATH=C:/Qt/6.7.3/msvc2019_64
cmake --build --preset windows-release
ctest --preset windows-release
```

The build tree is not self-contained, and this is the one thing that surprises everybody once.
vcpkg copies the libraries it built next to the executables, so spdlog, libdatachannel and the rest are there, and Qt
is not: `build\windows-release\bin\partyshare.exe` started from Explorer dies on
*"Qt6Core.dll was not found"*, while the same file started from the shell that built it runs, because that shell has
Qt on `PATH`. Either keep `C:\Qt\6.7.3\msvc2019_64\bin` on `PATH`, or work from an install tree, which carries
everything the program loads:

```
cmake --install build\windows-release --prefix stage
windeployqt --release stage\bin\partyshare.exe
```

`windeployqt` is the Qt half and the install rule is the other: `client/CMakeLists.txt` installs a runtime dependency
set on Windows, which is what puts the vcpkg libraries in the tree. Neither one covers what the other does.

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `DV_BUILD_CLIENT` | ON | Builds the client. Requires Qt 6. |
| `DV_BUILD_CLIENT_UI` | ON | Builds the Qt interface. Off leaves the client core, which has no Qt in it. |
| `DV_BUILD_SERVER` | ON | Builds the server. |
| `DV_BUILD_TESTS` | ON | Builds the test suite. |
| `DV_ENABLE_SANITIZERS` | OFF | AddressSanitizer and UndefinedBehaviorSanitizer. |
| `DV_WARNINGS_AS_ERRORS` | OFF | On in every preset. |
| `DV_ENABLE_WEBRTC_SPIKE` | OFF | The M3 spike. See docs/webrtc-toolchain.md. |
| `DV_ENABLE_MONGO` | OFF | Persists accounts, roles, rooms and the audit log in MongoDB. Needs the driver, see below. |

`DV_ENABLE_MONGO` needs `mongo-cxx-driver`, which is a vcpkg feature rather than a plain dependency so that a build without it needs nothing installed:

```sh
cmake -S . -B build/mongo -DDV_ENABLE_MONGO=ON -DVCPKG_MANIFEST_FEATURES=mongo
```

With the option off, the server keeps accounts, rooms and the audit log in memory, which is what it did before persistence existed.
The whole test suite builds and passes either way; the tests that need a real database carry the `mongo` label and skip themselves unless `DV_TEST_MONGO_URI` is set.

Tests are labelled: `ctest -L unit` runs only the unit tests, `ctest -L integration` only the integration ones.
The integration tests start a real server on an ephemeral port and connect real WebSocket clients.

Without Qt installed, use `-DDV_BUILD_CLIENT_UI=OFF`.
The client core has no Qt header in it, so it and the tests that drive it still build.
`-DDV_BUILD_CLIENT=OFF` goes further and leaves only the server, the shared library and the tests of both.

## Runtime configuration

Precedence is: built-in defaults, then the file, then environment variables, then the command line.

```sh
./build/linux-release/bin/partyshare-server \
  --config=config.json --port=8080 --log-level=debug \
  --users-file=dev-users.json
```

`--users-file` points at a list of development accounts:

```json
[
  {"username": "ana", "password": "test-password", "display_name": "Ana"}
]
```

That file stores passwords in plain text and exists only so the MVP has users.
Section 17 of the SPEC forbids it in production, and the server logs a warning on every startup.

Environment variables use the `DV_` prefix, for example `DV_SIGNALING_URL`, `DV_LOG_LEVEL`, `DV_VIDEO_FPS`.
The complete list is in `shared/src/config/config.cpp`.

The client also reads a `config.ini` it is not told about, first beside its own executable and then under
the user's configuration directory, with the second winning.
It creates the second one from `assets/config.ini`, compiled into the binary, the first time it runs, and
writes the audio devices chosen in the settings dialog back into it. That is how an installed client is pointed at
a server, because a shortcut carries no arguments; the ports section of [requirements.md](requirements.md)
and the client chapter of [../INSTALL.md](../INSTALL.md) cover it. `--config=PATH` takes `.ini` or `.json`
by extension and replaces both.

`--ice-port-range=50000-50100` pins the UDP ports the SFU binds media in, which is what makes a firewall rule
possible: without it the system picks an ephemeral port per participant.
The ports section of [requirements.md](requirements.md) covers how to size the range.

## Formatting and static analysis

```sh
find shared client server tests tools -name '*.cpp' -o -name '*.hpp' | xargs clang-format -i
clang-tidy -p build/linux-debug $(find shared server -name '*.cpp')
```

CI runs both, plus cppcheck, plus the suite under ASan and UBSan.
