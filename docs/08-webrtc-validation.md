# 8. Validating libwebrtc on a new platform

The procedure for answering, on a machine nobody has tried yet, whether libwebrtc
links, captures and interoperates. Read it before starting rather than during.

What is still unanswered is section 7 of [chapter 7](07-webrtc-toolchain.md).

## 1. What has to be answered

| Question | Where |
| --- | --- |
| ~~Does libwebrtc link and run on Windows x64?~~ | Answered: yes, section 5 |
| ~~Does libwebrtc link and run on macOS ARM64?~~ | Answered: yes, the media layer builds and runs there |
| Does capture work on Wayland, and not only on X11? | Linux with a Wayland session |
| Does `std::string` cross the libwebrtc boundary intact? | Answered on all three |

The `std::string` question is the important one. It is the conflict described in
section 5 of [chapter 7](07-webrtc-toolchain.md), confirmed on Linux with the
prebuilt package and solved there by building from source. The spike tests it
directly: with `dv::shared` linked in, it serialises and reparses a protocol
message across the boundary.

## 2. About the session

**Run this in a normal graphical session, sitting in front of the machine.**

A text console or a CI runner does not count: with no graphics server attached,
`CreateScreenCapturer` returns null and the spike reports capture as skipped,
which adds nothing to what is already known.

Over SSH it counts halfway. With an X11 session running on the machine, pointing
the spike at it works and the capture is real — that is how the Linux X11
validation was done:

```sh
DISPLAY=:1 XAUTHORITY=$HOME/.Xauthority ./build/spike/bin/webrtc-spike
```

On Wayland it does not apply: the XDG portal has to show a consent dialog in the
user's session, so there you really do have to be at the keyboard.

## 3. Linux and macOS

```sh
scripts/validate_webrtc.sh
```

On Linux, repeat it in a session of the other kind — the two capture paths are
different implementations:

```sh
echo $XDG_SESSION_TYPE   # wayland on one run, x11 on the other
```

On Wayland the system should show a permission dialog. If it does not appear and
capture fails, that is relevant information; write it down.

On macOS the first run should ask for screen recording permission. Grant it in
System Settings → Privacy and Security → Screen Recording, and run it again.
Without it, monitor enumeration returns an empty list, which is different from
failing. The prebuilt package download is 322 MB.

## 4. Windows

No shell script, because the machine has no bash by default. Open the **x64
Native Tools Command Prompt for VS 2022**:

```bat
cd C:\path\to\PartyShare

cmake -S . -B build\spike -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDV_ENABLE_WEBRTC_SPIKE=ON ^
  -DDV_WEBRTC_WINDOWS_DYNAMIC_CRT=OFF ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug> ^
  -DDV_BUILD_CLIENT=OFF ^
  -DDV_BUILD_SERVER=OFF ^
  -DDV_BUILD_TESTS=OFF

cmake --build build\spike
build\spike\bin\webrtc-spike.exe
```

The prebuilt package download is 739 MB, so the first configure takes a while.
Without Ninja, swap `-G Ninja` for `-G "Visual Studio 17 2022" -A x64`, and the
binary lands in `build\spike\bin\Release\`.

The two extra flags are what ask for the published `/MT` package and make the
whole link agree with it. `Findlibwebrtc.cmake` fetches the dynamic-CRT tree by
default now, because that is the one the client links; the spike is the only
thing left that wants the other. Section 6 of
[chapter 7](07-webrtc-toolchain.md) says what each way out costs.

## 5. What a good run looks like

```text
[ OK ] screen capturer              monitors found: 2
        monitor id=0 title="DELL U2720Q"
        monitor id=1 title="Built-in Retina Display"
[ OK ] screen capture frame         2560x1440, 14400 KiB
[ OK ] audio device module          inputs: 3, outputs: 4
[ OK ] std::string across ABI       dv::shared linked and interoperating

spike passed
```

Three lines carry the whole result:

- **`screen capturer`** has to say `monitors found: N` with N above zero.
  `skipped` means there was no graphical session and the test did not count.
- **`screen capture frame`** has to carry a resolution. That is the line proving
  capture delivers pixels and not merely that it can list monitors. On Wayland it
  appears only after the portal dialog is accepted, and the spike waits up to 15
  seconds for it.
- **`std::string across ABI`** has to say `dv::shared linked and interoperating`.

The symptom of the ABI conflict differs by platform. On Linux with the prebuilt
package the spike is built standalone on purpose and the line says `skipped`,
which is the known conflict. On Windows and macOS the conflict is not there. If
it ever comes back on a new package it **does not compile**: the link fails with
`std::__Cr::` symbols, and that failure is precisely the result worth knowing —
send the output rather than working around it.

## 6. If it fails

Send the complete output, compile and link errors included, plus:

```sh
uname -a          # Linux and macOS
cmake --version
```

## 7. Building from source on a new platform

If section 3 or 4 fails, that platform needs the same treatment Linux got. Set
aside the time: the checkout is over 30 GB and the build takes tens of minutes.

On macOS the same script works:

```sh
scripts/build_webrtc.sh
scripts/validate_webrtc.sh --root ~/.cache/partyshare/webrtc/dist
```

### 7.1 Windows: two prerequisites that fail late and expensively

**Smart App Control has to be off.** The Chromium toolchain downloads and runs
dozens of unsigned binaries through CIPD — `luci-auth`, `gn`, `ninja`, `clang`,
`rustc`. With Smart App Control enforcing, `gclient sync` dies on the first:

```text
Exception: 4551: 'depot_tools\.cipd_bin\luci-auth.exe' was blocked
by your organization's Device Guard policy.
```

That happens *after* the 14 GB checkout, not before. And turning Smart App
Control off is a one-way door: Microsoft only allows it to be enabled on a clean
install of Windows, so re-enabling it later means resetting the machine. Decide
before downloading rather than after.

**The Windows SDK needs the debugging tools**, which the Visual Studio Build
Tools workload does not bring:

```powershell
winsdksetup.exe /features OptionId.WindowsDesktopDebuggers /quiet /norestart
```

### 7.2 The Windows build

Chromium looks for Visual Studio under `%ProgramFiles%`, and the Build Tools land
in `%ProgramFiles(x86)%`, so it has to be pointed at it.

```bat
set "DEPOT_TOOLS_WIN_TOOLCHAIN=0"
set "vs2022_install=C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools"
set "GYP_MSVS_VERSION=2022"
set "PATH=%CD%\depot_tools;%PATH%"

git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
fetch --nohooks webrtc
cd src
git fetch origin branch-heads/7977:branch-heads/7977
git checkout branch-heads/7977
gclient sync -D --force --reset --with_branch_heads --with_tags
git -C build apply <PartyShare>\patches\webrtc\build\0002-win-dynamic-crt.patch
```

The GN arguments are the ones `scripts/build_webrtc.sh` uses, plus three only
Windows needs:

```text
win_use_dynamic_crt=true
rtc_build_ssl=false                    libsrtp_build_boringssl=false
rtc_ssl_root="<openssl include dir>"   libsrtp_ssl_root="<the same>"
```

`win_use_dynamic_crt` is what the patch adds. The two ssl pairs have to agree with
each other or `pc/BUILD.gn` refuses the configure with *"Mismatch ssl build
settings detected"*, and then *"Mismatch in ssl root detected"*.

### 7.3 Packaging, and the trap in it

`ninja -C out/dv-release webrtc` is not the whole library, exactly as section 4.6
of [chapter 7](07-webrtc-toolchain.md) says — and Windows needs one piece more
than Linux: `rtc_software_fallback_wrappers`, without which the client link ends
on `CreateVideoEncoderSoftwareFallbackWrapper` alone.

The per-target `.lib` files are **thin archives** — they begin with `!<thin>` and
hold paths rather than objects — so `lib.exe` cannot read them, and `lib /LIST`
answers with nothing rather than with a complaint. Merge the object files instead:

```bat
lib /OUT:dist\lib\webrtc.lib obj\webrtc.lib ^
  obj\api\video_codecs\builtin_video_encoder_factory\*.obj ^
  obj\api\video_codecs\builtin_video_decoder_factory\*.obj ^
  obj\api\video_codecs\rtc_software_fallback_wrappers\*.obj ^
  obj\api\video\adapted_video_track_source\*.obj ^
  obj\media\rtc_internal_video_codecs\*.obj ^
  obj\media\rtc_simulcast_encoder_adapter\*.obj
```

Headers go into `dist/include` the way the script does it on Linux, libyuv
flattened included. Then two marker files, both read by
`cmake/Findlibwebrtc.cmake`:

- `DV_SYSTEM_LIBCXX`, because the tree was built with `use_custom_libcxx=false`.
- `DV_EXTERNAL_SSL`, because it was built with `rtc_build_ssl=false` and something
  else has to bring the OpenSSL. Without it the client link ends in 49 unresolved
  externals starting at `SSL_CTX_set_options`.

### 7.4 What it produced

A `partyshare.exe` of 30 MB holding Qt, libwebrtc and OpenSSL at once — the
combination the prebuilt package makes impossible — and 24 of the 25 media tests
passing. The one that did not is explained in section 7 of
[chapter 7](07-webrtc-toolchain.md).

That `dist` tree is published as a release asset under the
`webrtc-m152.7977.0.0-windows-x64` tag, because a release pipeline cannot be asked
to repeat any of the above on every tag. Rebuilding it — a new milestone, a
different GN argument — means repeating this section, uploading the result, and
changing `_dv_url_windows_x64_md` and `_dv_sha_windows_x64_md` together.

### 7.5 macOS, published for the same reason

macOS needs none of section 7.1 to 7.3: `scripts/build_webrtc.sh --ssl-root <dir>`
produces the tree in one command, and the merge step Windows needs is what `ar`
already does there.
What it shares is the reason to publish the result rather than rebuild it, and
the packaging is the same shape:

```sh
cd ~/.cache/partyshare/webrtc/dist
tar -czf webrtc-m152.7977.0.0-macos-arm64-system-libcxx.tar.gz \
  DV_EXTERNAL_SSL DV_SYSTEM_LIBCXX VERSIONS include lib
shasum -a 256 webrtc-m152.7977.0.0-macos-arm64-system-libcxx.tar.gz
```

131 MB compressed, from a 478 MiB `lib/libwebrtc.a`, with the archive members at
the root rather than under a directory of their own: CMake's `FetchContent` lifts
a single top level directory and `Findlibwebrtc.cmake` then looks for `include/`
and the two markers where they are.
It is published under the `webrtc-m152.7977.0.0-macos-arm64` tag, and rebuilding
it means changing `_dv_url_macos_arm64_src` and `_dv_sha_macos_arm64_src`
together.
