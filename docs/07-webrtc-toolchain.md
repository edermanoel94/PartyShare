# 7. libwebrtc toolchain

Why this project builds libwebrtc from source instead of downloading it, what
that build does, and the traps encoded in `cmake/Findlibwebrtc.cmake` so that
nobody meets them twice.

Validating the result on a platform for the first time is
[chapter 8](08-webrtc-validation.md).

## 1. Where each platform stands

| Platform | Status |
| --- | --- |
| Linux x64, prebuilt | Compiles, links and runs — **but only against Chromium's libc++**, which rules it out. Section 4 |
| Linux x64, from source | Validated, with `use_custom_libcxx=false`. Real screen capture under X11 |
| Windows x64 | Validated over a source build with the dynamic CRT. All 25 media tests pass |
| macOS ARM64 | Validated over a source build. 22 of 25 media tests pass, one skips, two fail on the audio device rather than transport |
| macOS x64 | The distribution publishes no such build |

The spike lives in `tools/webrtc_spike/`, behind `-DDV_ENABLE_WEBRTC_SPIKE=ON`.
It checks threads, `PeerConnectionFactory`, SDP offer generation, monitor
enumeration, capture of one real frame, audio device enumeration, and
`std::string` crossing the library boundary:

```text
libwebrtc toolchain spike

[ OK ] threads started
[ OK ] peer connection factory
[ OK ] peer connection
[ OK ] sdp offer                    5790 bytes
[ OK ] screen capturer              monitors found: 1
        monitor id=382 title="DP-2"
[ OK ] screen capture frame         1920x1080, 8100 KiB
[ OK ] audio device module          inputs: 2, outputs: 2
[ OK ] std::string across ABI       dv::shared linked and interoperating

spike passed
```

The generated SDP carries `opus/48000/2` with `transport-cc`, plus H.264, VP8,
VP9 and AV1, which confirms the codecs section 6 of the SPEC requires are present.

## 2. Pinned version

```text
release:  m152.7977.0.0
source:   https://github.com/shiguredo-webrtc-build/webrtc-build
```

SHA-256 checksums live in `cmake/Findlibwebrtc.cmake`, taken from the digests the
GitHub API publishes, so nothing has to be downloaded to check a new version:

```sh
scripts/webrtc_checksums.sh                 # latest release
scripts/webrtc_checksums.sh m153.0000.0.0   # a specific release
```

## 3. Reproducing the spike

```sh
cmake -S . -B build/spike \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DDV_ENABLE_WEBRTC_SPIKE=ON \
  -DDV_BUILD_CLIENT=OFF -DDV_BUILD_TESTS=OFF
cmake --build build/spike
./build/spike/bin/webrtc-spike
```

The download is roughly 110 MB on Linux, 322 MB on macOS and 739 MB on Windows.
`-DDV_WEBRTC_ROOT=/path/to/webrtc` reuses an already extracted tree.

## 4. The traps

All of them were found by running the spike, and all of them are encoded in
`cmake/Findlibwebrtc.cmake`. The first two apply only to the published binaries;
the rest apply to a source build as well.

**4.1 The archive extracts into a subdirectory.** The tarball unpacks into
`webrtc/`, holding `include/`, `lib/libwebrtc.a`, `VERSIONS` and `DEPS`. The
module accepts both that layout and a hand-built one.

**4.2 Chromium's libc++, with headers removed.** The build uses Chromium's libc++,
whose symbols live in the `std::__Cr` ABI namespace:

```sh
nm -C --defined-only lib/libwebrtc.a | grep -oE 'std::__[A-Za-z0-9]+::' | sort -u
# std::__Cr::
```

The archive ships that libc++ with every extensionless header removed — no
`<string>`, no `<vector>`, no `<cstdio>` — so the tree is unusable on its own.
The module reads the pinned commit out of `VERSIONS`, downloads the complete
header set from the Chromium mirror, and writes the two headers the libc++ build
normally generates: `__config_site` with `_LIBCPP_ABI_NAMESPACE __Cr`, and an
empty `__assertion_handler`.

**4.3 The package's abseil has to come first.** libwebrtc's public headers include
`absl/...`. With the system abseil found first, compilation fails inside
`absl/strings/internal/str_format`, because the two copies disagree about the
standard library configuration.

**4.4 `WEBRTC_USE_X11` and `WEBRTC_USE_PIPEWIRE` change the layout of public
structs.** The most dangerous of them, because it fails silently.
`DesktopCaptureOptions` has members inside those two `#if`s. A consumer that does
not define them compiles without a warning and then corrupts its own stack when
the library writes into the object:

```text
*** stack smashing detected ***: terminated
```

The module defines both as INTERFACE usage requirements, so any target linking
`libwebrtc::libwebrtc` gets them.

**4.5 The portal path needs glib, gbm and libdrm at link time.** A consequence of
`WEBRTC_USE_PIPEWIRE`: the XDG portal code uses GDBus and imports frames as
DMA-BUF, leaving `g_dbus_*`, `g_variant_*`, `gbm_*` and `drm*` unresolved. The
module asks `pkg-config` for `glib-2.0`, `gio-2.0`, `gobject-2.0`, `gbm` and
`libdrm`. PipeWire itself is not on the list, because libwebrtc `dlopen`s it.

**4.6 GN's `webrtc` target is not the whole library.** `ninja webrtc` produces an
`obj/libwebrtc.a` that looks complete and is not:
`CreateBuiltinVideoEncoderFactory` and `CreateBuiltinVideoDecoderFactory` live in
targets of their own. `build_webrtc.sh` builds those alongside, expands each into
the transitive closure of its dependencies with `gn desc ... deps --all`, and
appends the objects that are not already in the archive — 88 of 448 as things
stand. The comparison is by object name, because a thin `.a` stores only names;
two different objects sharing a name would show up as an undefined reference at
link time, never as a silently wrong binary.

## 5. The decision: libc++ against libstdc++

This is the finding that shaped everything after it.

On Linux, the published binaries require any code exchanging `std::` types with
libwebrtc to be compiled against Chromium's libc++. libwebrtc's public API uses
`std::string`, `std::vector` and `std::unique_ptr` everywhere, so that exchange
happens constantly and cannot be avoided. The Qt 6 that Linux distributions ship
is built against libstdc++, and **one binary cannot use both standard libraries
for the same types**.

Three ways out were considered:

1. **Build libwebrtc from source with `use_custom_libcxx=false`**, so it uses the
   system standard library and the conflict disappears. Cost: a checkout over
   30 GB, a build of tens of minutes, and the responsibility of maintaining and
   distributing that binary.
2. **Isolate libwebrtc behind a C ABI in a separate shared library.** The process
   then holds both standard libraries and no `std::` type crosses the boundary.
   Cost: a translation layer over the whole media surface, maintained by hand
   forever.
3. **Compile the entire client, Qt included, against Chromium's libc++.** Not
   viable: it would mean recompiling Qt and every dependency.

**Option 1.** `scripts/build_webrtc.sh` automates it and packages the result in
the layout `Findlibwebrtc.cmake` already consumes. The GN arguments that matter:

```text
use_custom_libcxx=false     # the reason the script exists
use_rtti=true               # our code and Qt both use RTTI
rtc_use_h264=true           # H.264 is required by section 6 of the SPEC
proprietary_codecs=true
ffmpeg_branding="Chrome"
use_sysroot=false           # links against this machine's glibc
```

The script writes a `DV_SYSTEM_LIBCXX` marker into the output tree, and
`Findlibwebrtc.cmake` turns off all the libc++ handling of section 4.2 when it
finds one.

The result confirms it — no symbol left in `std::__Cr`:

```sh
nm -C --defined-only lib/libwebrtc.a | grep -oE 'std::__[A-Za-z0-9]+::' | sort -u
# std::__cxx11::
# std::__detail::
```

`std::__cxx11` is the libstdc++ ABI, the same one the distributions' Qt 6 uses.
The final archive is about 66 MB.

Two non-obvious details of the build, both applied by the patch in
`patches/webrtc/build/`:

- The standard library used during compilation is pinned to a **libstdc++ 14**
  downloaded separately rather than the system one. The usable range is narrow:
  older lacks the C++20 features WebRTC uses, and newer makes clang and libstdc++
  disagree about `std::is_constructible`.
- Chromium's **CREL relocations** are turned off with `dv_disable_crel=true`.
  Only lld reads them, and keeping them would force every consumer, the Qt client
  included, to link with lld.

> The patch declaring those two arguments was missing from the repository for as
> long as an unanchored `build/` ignore rule existed, and what is there now is a
> reconstruction. GN answers an undeclared argument with *"Build argument has no
> effect"* and exits 0, so a build without it does not fail — it succeeds and
> produces the wrong library. Entry 13 of [chapter 15](15-postmortems.md).
> Whoever runs a Linux build over the reconstruction first should record here
> whether it does what the original did.

### 5.1 Patches carried

They live in `patches/webrtc/<repo>/`, where `<repo>` is the gclient checkout they
apply to. `build_webrtc.sh` applies them all and fails early if any does not,
which is the signal that the pinned milestone has moved.

| Patch | Why |
| --- | --- |
| `build/0001-libstdcxx-and-crel-opt-outs.patch` | The external libstdc++ and the CREL opt-out above |
| `build/0002-win-dynamic-crt.patch` | Adds `win_use_dynamic_crt`, so Windows gets `/MD` without a component build |
| `src/0001-qualify-nullptr-t.patch` | Compilation fix against the pinned libstdc++ |
| `src/0002-pulse-adm-reset-quit-on-init.patch` | The PulseAudio bug below |

### 5.2 The PulseAudio capture bug

A real libwebrtc bug, not a project one, and worth recording because it cost
time. The first call in a process works; every call after it takes exactly ten
seconds to negotiate and ends up with no microphone.

`AudioDeviceLinuxPulse::Terminate()` sets `quit_ = true`, and `Init()` never puts
it back. On the second initialisation the freshly created capture thread reads
`quit_` on its first pass and exits, and `StartRecording()` waits ten seconds for
an event that thread was supposed to signal.

The patch sets `quit_ = false` in `Init()`. Successive sessions in one process go
from 10.2 s to 1.2 s, and all of them capture. Worth reporting upstream.

## 6. Windows and macOS

The guess used to be that they had the same problem, because Chromium uses its
own libc++ on all three platforms by default. **On Windows that guess was wrong.**

The prebuilt Windows package carries no `std::__Cr::` symbol at all — it is built
against MSVC's own STL, so `dv::shared` links straight into it. Windows does not
need the source build for the reason Linux needed it.

What Windows has instead is the same conflict in another currency: `webrtc.lib`
is compiled against the **static** C runtime, `/MT`, while everything else
defaults to the dynamic one, and linking the two gives dozens of

```text
error LNK2038: mismatch detected for 'RuntimeLibrary': value 'MT_StaticRelease'
doesn't match value 'MD_DynamicRelease'
```

The whole link has to agree, and there are two ways to make it. Moving the vcpkg
stack to `x64-windows-static` is one, and it is the expensive one — it changes
what the MSI ships. **The decision was the other direction:** move libwebrtc to
`/MD`, which is what `win_use_dynamic_crt=true` does and what
[section 7 of chapter 8](08-webrtc-validation.md) builds. vcpkg stays on
`x64-windows`, Qt stays as downloaded, and the MSI keeps shipping the same DLLs.

That tree cannot be downloaded from anywhere, so it is published from this
repository under the `webrtc-m152.7977.0.0-windows-x64` tag.
`cmake/Findlibwebrtc.cmake` fetches it on Windows by default, and
`DV_WEBRTC_WINDOWS_DYNAMIC_CRT=OFF` goes back to the `/MT` package — which the
spike is now the only thing that wants.

macOS has neither conflict. The media layer builds and runs there over a source
build, so `std::string` crosses the boundary in anger rather than in a probe. It
needed `--ssl-root`, because libwebrtc's bundled BoringSSL and the OpenSSL
libdatachannel needs collide in 932 duplicate symbols; `ld64` refuses that
outright, which is why it surfaced there and not on Linux, where GNU `ld` takes
the first definition and links anyway.

## 7. What is still open

- [ ] **Screen capture on Wayland**, through the XDG portal. The development
      machine runs X11, so this depends on another session.
- [ ] A Linux build over the reconstructed `0001` patch, per the note in section 5.
- [x] Everything else: the pin and its checksums, `Findlibwebrtc.cmake`, source
      builds on Linux and Windows, X11 capture with a real 1920x1080 frame, and
      the media layer running on all three platforms.

Two platform-specific results are worth keeping, because both look like defects
and are not:

- On **Windows**, `TheEchoCancellerRunsOnTheCapturedAudio` failed while echo
  cancellation was working the whole time. Windows has an echo canceller of its
  own, libwebrtc enables it and switches AEC3 off — *"Disabling EC since built-in
  EC will be used instead"* — and `echo_return_loss` is an AEC3 metric. That
  canceller has since been declined, and not for the metric: it captures only
  while it is also playing, so the first participant of a room, whose microphone
  starts before there is anything to play, sent no audio at all — *"Playout must
  be started before recording when using the built-in AEC"*. The engine turns it
  off after the voice engine's own defaults turn it on, AEC3 runs on Windows as
  it does elsewhere, and `echo_return_loss` is the evidence on every platform.
- On **macOS**, switching the microphone takes 2.4 s to resume audio against a
  500 ms budget, which is what re-opening a CoreAudio input costs.

macOS x64 stays out: the distribution publishes no such build. The SPEC lists it
as desirable rather than mandatory.
