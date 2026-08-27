# 2. Build

The tooling needed to compile the client and the server. The hardware needed to
*run* them is [chapter 12](12-requirements.md); the short path from nothing to a
running room is [INSTALL.md](../INSTALL.md).

## Prerequisites

| Tool | Minimum | Needed by |
| --- | --- | --- |
| CMake | 3.25 | everything, presets version 6 |
| Ninja | 1.11 | every preset except `linux-make` |
| C++20 compiler | MSVC 2022, GCC 12, Clang 15 | everything |
| Qt | 6.5 | the client only |
| libdatachannel, OpenSSL | — | the server only, through vcpkg |
| mongo-cxx-driver | — | the server with persistence, a vcpkg feature |

spdlog, nlohmann/json and GoogleTest need no step: each is looked up with
`find_package` first and only downloaded through `FetchContent` when it is not
installed, so vcpkg, distribution packages and nothing at all all work.

libdatachannel and OpenSSL have to really exist, because they are not header
only. Without them, `-DDV_BUILD_SERVER=OFF` builds the client and the shared
tests alone.

`scripts/ci_vcpkg.sh` checks out vcpkg at the commit `vcpkg.json` pins and prints
the toolchain file to point CMake at. Use it rather than a vcpkg already on the
machine: a ports tree newer than the pinned version database fails with
`no version database entry for <package>`.

On Linux, whoever links libwebrtc also needs X11, glib, gbm and libdrm headers.
They are not our dependencies — they come from libwebrtc's own screen capture,
which talks to the XDG portal over GDBus and imports frames as DMA-BUF:

```sh
# Arch
sudo pacman -S --needed libx11 libxext libxfixes libxdamage libxrandr \
  libxcomposite libxtst glib2 mesa libdrm

# Debian and Ubuntu
sudo apt install libx11-dev libxext-dev libxfixes-dev libxdamage-dev \
  libxrandr-dev libxcomposite-dev libxtst-dev libglib2.0-dev libgbm-dev libdrm-dev
```

## Presets

```sh
cmake --preset linux-release
cmake --build --preset linux-release
ctest --preset linux-release
```

```text
linux-debug     linux-release       linux-asan          linux-make
windows-debug   windows-release     windows-asan
macos-arm64-debug   macos-arm64-release   macos-arm64-asan   macos-x64-release
```

Every preset generates for Ninja except `linux-make`, which uses Unix Makefiles
and exists for machines without it. Without Ninja on the path the others fail at
configure time with `unable to find a build program corresponding to "Ninja"`.

Binaries land in `build/<preset>/bin/`.

The `windows-*` presets name `cl` as the compiler, so configure, build and test
all have to run from a Developer Command Prompt or a shell that has sourced
`vcvars64.bat`.

## Options

| Option | Default | Effect |
| --- | --- | --- |
| `DV_BUILD_CLIENT` | ON | Builds the client. Requires Qt 6 |
| `DV_BUILD_CLIENT_UI` | ON | Builds the Qt interface. Off leaves the client core, which has no Qt in it |
| `DV_BUILD_CLIENT_MEDIA` | OFF | Builds the media layer. Requires a libwebrtc tree, below |
| `DV_BUILD_SERVER` | ON | Builds the server |
| `DV_BUILD_TESTS` | ON | Builds the test suite |
| `DV_ENABLE_MONGO` | OFF | Persists accounts, rooms, chat and the audit log in MongoDB |
| `DV_ENABLE_SANITIZERS` | OFF | AddressSanitizer and UndefinedBehaviorSanitizer |
| `DV_WARNINGS_AS_ERRORS` | OFF | On in every preset |
| `DV_HARDWARE_ENCODER_NVENC` | ON | NVENC backend. Only read when the media layer is on |
| `DV_HARDWARE_ENCODER_MEDIA_FOUNDATION` | ON | Media Foundation backend, Windows only |
| `DV_ENABLE_WEBRTC_SPIKE` | OFF | The libwebrtc spike, [chapter 7](07-webrtc-toolchain.md) |
| `DV_ENABLE_SCREEN_AUDIO_SPIKE` | OFF | The shared screen audio spike, [chapter 9](09-screen-audio.md) |

`DV_ENABLE_MONGO` needs `mongo-cxx-driver`, which is a vcpkg *feature* rather
than a plain dependency so that a build without it needs nothing installed:

```sh
cmake -S . -B build/mongo -DDV_ENABLE_MONGO=ON -DVCPKG_MANIFEST_FEATURES=mongo
```

The whole suite builds and passes either way. The tests that need a real database
carry the `mongo` label and skip themselves unless `DV_TEST_MONGO_URI` is set.

## Building one half

| Situation | Flag |
| --- | --- |
| No Qt installed | `-DDV_BUILD_CLIENT_UI=OFF` keeps the client core, drops the interface |
| Server only | `-DDV_BUILD_CLIENT=OFF` |
| Client only | `-DDV_BUILD_SERVER=OFF` |
| Faster iteration | `-DDV_BUILD_TESTS=OFF` |

In a build tree that already exists, name the target instead. The executable
target is `dv_server`; `partyshare-server` is only the name it is written under,
so `--target partyshare-server` is not a thing:

```sh
cmake --build build/linux-release --target dv_server
```

On a machine with no Qt at all, configure a tree that never mentions the client.
Do not use a preset for that: each one fixes its own `binaryDir`, so
`--preset windows-release` with the client off would overwrite the full tree
rather than sit beside it. [INSTALL.md](../INSTALL.md) has the commands.

## The media layer

The client builds without libwebrtc by default. In that mode the interface, the
login, the rooms and the signaling all work, and there is no call:
`create_media_session` fails with `media_unavailable`.

Media needs the tree that `scripts/build_webrtc.sh` produces, for the reasons in
[chapter 7](07-webrtc-toolchain.md):

```sh
cmake -S . -B build/media \
  -DDV_BUILD_CLIENT_MEDIA=ON \
  -DDV_WEBRTC_ROOT=$HOME/.cache/partyshare/webrtc/dist
cmake --build build/media
```

On Windows `cmake/Findlibwebrtc.cmake` fetches a published tree by default, so
`-DDV_WEBRTC_ROOT` is only needed to point at one built by hand.

### Debugging environment variables

| Variable | Effect |
| --- | --- |
| `DV_WEBRTC_LOG` | `warning`, `info` or `verbose`. libwebrtc's internal log, the only way to see why a device did not open or a codec was refused |
| `DV_DUMP_SDP` | Makes the SFU log every offer it sends. How you answer "was this negotiated?" |
| `DV_AUDIO_NULL_DEVICE` | A null audio device instead of the system one, for machines without a sound card and for CI. Nothing is captured and nothing is played |
| `DV_VIRTUAL_INPUT_DEVICE`, `DV_VIRTUAL_OUTPUT_DEVICE` | Devices the media tests should use. Exported by `scripts/virtual_audio.sh` |
| `DV_DISABLE_HARDWARE_ENCODER` | Forces software encoding even on a capable card, which is how the two get compared |
| `DV_HARDWARE_ENCODER` | `nvenc`, `mediafoundation` or `none`, to pick one by name |
| `DV_CRASH_DIRECTORY`, `DV_CRASH_REPORTS` | Where crash reports go, and `0` to turn them off |

The variables that configure the product rather than debug it are in
[chapter 3](03-configuration.md).

### Virtual audio device

A null device lets negotiation through and captures silence, and captured audio
is precisely what the audio suite has to verify. `scripts/virtual_audio.sh`
builds a real virtual sound card on PulseAudio with a tone playing into the
microphone:

```sh
eval "$(scripts/virtual_audio.sh start)"
ctest --test-dir build/media -L media --output-on-failure
scripts/virtual_audio.sh stop
```

On a machine that already has a sound server it attaches to it and leaves the
default devices alone, so the tests do not steal the speakers from whoever is at
the keyboard. With no sound server, which is the CI case, it starts a private one
and only then makes the virtual devices the defaults.

The virtual microphone is a `module-remap-source` over the monitor of a null
sink, rather than the monitor directly: libwebrtc's PulseAudio backend ignores
every source that monitors a sink when it enumerates capture devices, and a
device it does not list is a device it does not open.

### Impaired network

The media tests degrade the network from the inside, through the client's own
sockets, which is why they run with no privileges and no setup. To degrade it for
real, in the operating system's queues, there is `scripts/netem.sh`:

```sh
sudo scripts/netem.sh apply lossy      # 5% loss
sudo scripts/netem.sh apply distant    # 150 ms latency with jitter
sudo scripts/netem.sh apply awful      # both, plus reordering
sudo scripts/netem.sh clear
```

It needs root and the `sch_netem` module, which does not load on a machine whose
kernel was upgraded without a reboot; the script says so rather than letting `tc`
answer that the qdisc is unknown. `DV_NETEM_DRY_RUN=1` prints the command without
running it. The numbers measured with each path are in
[chapter 11](11-benchmarks.md).

### Hardware encoder

The screen share is encoded by the graphics card when there is one, and by the
processor when there is not.

| Backend | Platform | Reaches |
| --- | --- | --- |
| NVENC | Linux, Windows | NVIDIA |
| Media Foundation | Windows | NVIDIA, Intel QuickSync, AMD VCN |

Neither on macOS: NVENC needs an NVIDIA driver and macOS has had none since
Mojave, and VideoToolbox is not written yet.

A Windows build carries both, and that is the point of them being a list rather
than a compile-time choice — the machine running the binary is the only one that
knows which card is in it. The list is probed in order and the first one that
finds usable hardware wins, NVENC first: on an NVIDIA machine the Media
Foundation transform wraps NVENC anyway, so going through it adds a layer and
takes away control. When none of them finds anything, every backend's reason is
reported, not just the last one.

Nothing is linked. Every library is opened at runtime through
`client/src/webrtc/dynamic_library.hpp` — `libcuda.so.1` / `nvcuda.dll`,
`libnvidia-encode.so.1` / `nvEncodeAPI64.dll`, `mfplat.dll`. That is not caution
for its own sake: `mfplat.dll` is absent on Windows N and KN, which ship without
the Media Feature Pack, and an executable that imported it would fail to start
there rather than fall back to software.

Which encoder is running shows up in the log at every metrics interval:

```text
Video: 1280x720 at 30.0 fps, up 835 kbps, estimate 1621 kbps, 0 frames dropped, encoder OpenH264
```

When there is no hardware, the reason is stated once, when the engine is created,
and it is worth reading before going looking for the problem in the driver:

```text
Media: no hardware encoding (the NVIDIA driver does not match its own kernel
module, which is what an upgrade without a reboot leaves behind), the screen is
encoded in software
```

## Crash reports

A crash that leaves nothing behind turns into a report that says "it closed by
itself". Both binaries install a signal handler that writes the build, the signal
and a backtrace:

```text
partyshare crash report
application: partyshare
version: 0.1.0
built: Aug 19 2026 16:36:58

when: 1787168270 seconds since the epoch, readable with: date -d @1787168270
signal: SIGSEGV, a read or write through a bad pointer

backtrace:
./build/media/bin/partyshare(+0x145437) [0x5572fb6c8437]
```

They go to `$XDG_STATE_HOME/partyshare/crashes` on Linux, `~/Library/Logs` on
macOS and `%LOCALAPPDATA%` on Windows, the ten most recent kept. Each line is
`binary(+offset) [address]`, and `addr2line -Cfe <binary> <offset>` turns the
offset into a file and a line. Names from libraries come out mangled, because
demangling allocates and a signal handler cannot; `c++filt` sorts it out.

The process still dies the way it would have died, so a configured core dump is
still produced and the exit code still says what killed the program.

## Tests

```sh
ctest --preset linux-release                          # everything
ctest --test-dir build/linux-release -L unit
ctest --test-dir build/linux-release -L integration
ctest --test-dir build/media -L media
DV_TEST_MONGO_URI=mongodb://127.0.0.1:27017 ctest --test-dir build/mongo -L mongo
```

The integration tests start a real server on an ephemeral port and connect real
WebSocket clients, so a passing suite means the server half genuinely works. The
`mongo` tests leave behind databases named `partyshare_test_*`, one per test and
per run, which are safe to drop.

## Formatting and static analysis

```sh
find shared client server tests tools -name '*.cpp' -o -name '*.hpp' \
  | xargs clang-format -i
clang-tidy -p build/linux-debug $(find shared server -name '*.cpp')
```

CI runs both, plus cppcheck, workflow linting, and the whole suite under
AddressSanitizer and UndefinedBehaviorSanitizer.
