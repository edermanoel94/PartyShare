# Validating libwebrtc on Windows and macOS

This document is the walkthrough for closing the M3 items the development environment cannot reach.
What remains to be validated is listed in section 6 of [webrtc-toolchain.md](webrtc-toolchain.md).

## 1. What has to be answered

| Question | Where |
| --- | --- |
| ~~Does libwebrtc link and run on Windows x64?~~ | Answered: yes, section 5.1 |
| Does libwebrtc link and run on macOS ARM64? | A macOS machine |
| Does capture work on Wayland, and not only on X11? | Linux with a Wayland session |
| Does `std::string` cross the libwebrtc boundary intact? | Windows and macOS |

On Linux the last two are already answered under X11, over the tree built from source.
Capture enumerated the monitor and delivered a 1920x1080 frame, and `std::string` crossed the boundary intact.
What is missing on Linux is the same check in a Wayland session.

The `std::string` question is the most important of the four.
It is the conflict described in section 5 of [webrtc-toolchain.md](webrtc-toolchain.md), confirmed on Linux with the prebuilt package and solved there by building from source.
The spike tests it directly: when `dv::shared` is linked in, it serializes and reparses a protocol message across the boundary.

## 2. Important, about the session

Run this in a normal graphical session, sitting in front of the machine.

A text console or a CI runner does not count: with no graphics server attached, `CreateScreenCapturer` returns null and the spike reports capture as skipped, which is exactly what we already know and adds nothing.

Over SSH it counts halfway.
If there is an X11 session running on the machine, pointing the spike at it works and the capture is real:

```sh
DISPLAY=:1 XAUTHORITY=$HOME/.Xauthority ./build/spike/bin/webrtc-spike
```

That is how the X11 validation on Linux was done.
On Wayland it does not apply: the XDG portal has to show a consent dialog in the user's session, so there you really do have to be in front of the machine.

## 3. Linux, in a graphical session

```sh
scripts/validate_webrtc.sh
```

Then repeat it in a session of the other kind.
If you use Wayland, log into X11 once, and vice versa, because the two capture paths are different implementations:

```sh
echo $XDG_SESSION_TYPE   # should say wayland on one run and x11 on the other
```

On Wayland capture goes through the XDG portal and the system should show a permission dialog.
If the dialog does not appear and capture fails, that is relevant information, write it down.

## 4. macOS ARM64

```sh
scripts/validate_webrtc.sh
```

On the first run macOS should ask for screen recording permission.
Grant it in System Settings, Privacy and Security, Screen Recording, and run it again.
Without that permission monitor enumeration returns an empty list, which is different from failing.

The prebuilt package download is 322 MB.

## 5. Windows x64

There is no shell script here, because the machine has no bash by default.
Open the **x64 Native Tools Command Prompt for VS 2022** and run:

```bat
cd C:\path\to\PartyShare

cmake -S . -B build\spike -G Ninja ^
  -DCMAKE_BUILD_TYPE=Release ^
  -DDV_ENABLE_WEBRTC_SPIKE=ON ^
  -DDV_BUILD_CLIENT=OFF ^
  -DDV_BUILD_SERVER=OFF ^
  -DDV_BUILD_TESTS=OFF

cmake --build build\spike

build\spike\bin\webrtc-spike.exe
```

The prebuilt package download is 739 MB, so the first configure takes a while.

If Ninja is not installed, swap `-G Ninja` for `-G "Visual Studio 17 2022" -A x64`, and the binary lands in `build\spike\bin\Release\`.

### 5.1 What the first Windows run found

Done, on Windows 11 with MSVC 19.44 and the pinned `m152.7977.0.0` package. The spike passes. Two things had to
be fixed to get there, and both are in the tree now, so the commands above work as written.

**`dwmapi` was missing** from the Windows library list in `cmake/Findlibwebrtc.cmake`. Everything else resolved
and the link died on one symbol:

```text
webrtc.lib(window_capture_utils.obj) : error LNK2019: unresolved external symbol
__imp_DwmGetWindowAttribute referenced in function GetCroppedWindowRect
```

It follows from `RTC_ENABLE_WIN_WGC`, defined two lines above it: Windows Graphics Capture asks the desktop
window manager whether a window is cloaked before capturing it.

**The static C runtime is not optional.** The `webrtc.lib` in the published package is built with `/MT` and
CMake defaults to `/MD`, which ends in `LNK2038: mismatch detected for 'RuntimeLibrary'` repeated across the
standard library and then `LNK1169`. The configure above therefore also needs:

```bat
  -DDV_WEBRTC_WINDOWS_DYNAMIC_CRT=OFF ^
  -DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded$<$<CONFIG:Debug>:Debug>
```

The first of the two is what asks for the published `/MT` package at all: `cmake/Findlibwebrtc.cmake` fetches
the dynamic CRT tree of section 8 by default now, because that is the one the client links, and the spike is
the only thing left that wants the other.

The `/MT` pair works for the spike because it builds spdlog and nlohmann_json from source in the same pass with
the same flag. It is not enough for the client, and section 5 of
[webrtc-toolchain.md](webrtc-toolchain.md) says what each way out costs.

## 6. What a good run looks like

```text
libwebrtc toolchain spike

[ OK ] threads started
[ OK ] peer connection factory
[ OK ] peer connection
[ OK ] sdp offer                    5790 bytes
[ OK ] screen capturer              monitors found: 2
        monitor id=0 title="DELL U2720Q"
        monitor id=1 title="Built-in Retina Display"
[ OK ] screen capture frame         2560x1440, 14400 KiB
[ OK ] audio device module          inputs: 3, outputs: 4
[ OK ] std::string across ABI       dv::shared linked and interoperating

spike passed
```

The three lines that matter most:

- `screen capturer` has to say `monitors found: N` with N greater than zero.
  `skipped` means there was no graphical session and the test did not count.
- `screen capture frame` has to carry a resolution.
  That is the line proving capture delivers pixels, and not merely that it can list monitors.
  On Wayland it only appears after you accept the portal dialog, and the spike waits up to 15 seconds for it.
- `std::string across ABI` has to say `dv::shared linked and interoperating`.

About that last line, the symptom of the conflict differs by platform:

- On **Linux with the prebuilt package** the spike is built standalone on purpose, and the line says `skipped`.
  That is expected and is already the known conflict.
  The real validation on Linux is over the tree `scripts/build_webrtc.sh` produces, and it has been done: the spike passes over it with `dv::shared` linked.
- On **Windows** the spike links `dv::shared` directly, and the answer is in: the conflict is **not** there.
  The package carries no `std::__Cr::` symbol, and the line reads `dv::shared linked and interoperating`.
- On **macOS** the spike tries the same thing and nobody has run it yet.
  If the conflict exists there, it **does not compile**: the link fails with `std::__Cr::` symbols.
  That failure is precisely the result we need to know, so send the output rather than trying to work around it.

## 7. If it fails

Send the complete output, including compile or link errors.
A link error mentioning symbols with `std::__Cr::` is the exact signature of the standard library conflict.

Worth sending as well:

```sh
uname -a                    # Linux and macOS
cmake --version
```

## 8. Building from source on those platforms

If item 5 fails on Windows or macOS, that platform needs the same treatment Linux got.

On macOS the same script works:

```sh
scripts/build_webrtc.sh
scripts/validate_webrtc.sh --root ~/.cache/partyshare/webrtc/dist
```

On Windows there is no script yet, and the procedure below is the one that worked the first time it was done.
Read section 8.1 before starting: two of its steps are prerequisites that fail late and expensively.

### 8.1 Two prerequisites, and why they come first

**Smart App Control has to be off.** The Chromium toolchain downloads and runs dozens of unsigned binaries through CIPD - `luci-auth`, `gn`, `ninja`, `clang`, `rustc`. With Smart App Control enforcing, `gclient sync` dies on the first of them:

```text
Exception: 4551: 'depot_tools\.cipd_bin\luci-auth.exe' was blocked
by your organization's Device Guard policy.
```

That happens *after* the 14 GB checkout, not before. Turning Smart App Control off is a one way door: Microsoft only allows it to be enabled on a clean install of Windows, so re-enabling it later means resetting the machine. Decide before downloading rather than after.

**The Windows SDK needs the debugging tools**, which the Visual Studio Build Tools workload does not bring:

```powershell
winsdksetup.exe /features OptionId.WindowsDesktopDebuggers /quiet /norestart
```

### 8.2 The build

Chromium looks for Visual Studio under `%ProgramFiles%`, and the Build Tools install lands in `%ProgramFiles(x86)%`, so it has to be pointed at it.

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

The GN arguments are the ones `scripts/build_webrtc.sh` uses, plus three that only Windows needs:

```text
win_use_dynamic_crt=true
rtc_build_ssl=false                    libsrtp_build_boringssl=false
rtc_ssl_root="<openssl include dir>"   libsrtp_ssl_root="<the same>"
```

`win_use_dynamic_crt` is what the patch adds. The two ssl pairs have to agree with each other or `pc/BUILD.gn` refuses the configure with *"Mismatch ssl build settings detected"*, and then with *"Mismatch in ssl root detected"*.

### 8.3 Packaging, and the trap in it

`ninja -C out/dv-release webrtc` is not the whole library, exactly as section 4.6 says, and Windows needs one piece more than Linux does: `rtc_software_fallback_wrappers`, without which the client link ends on `CreateVideoEncoderSoftwareFallbackWrapper` alone.

The per target `.lib` files are **thin archives** - they begin with `!<thin>` and hold paths rather than objects - so `lib.exe` cannot read them, and `lib /LIST` answers with nothing rather than with a complaint. Merge the object files instead, which sit under `obj/<target path>/<target name>/*.obj`:

```bat
lib /OUT:dist\lib\webrtc.lib obj\webrtc.lib ^
  obj\api\video_codecs\builtin_video_encoder_factory\*.obj ^
  obj\api\video_codecs\builtin_video_decoder_factory\*.obj ^
  obj\api\video_codecs\rtc_software_fallback_wrappers\*.obj ^
  obj\api\video\adapted_video_track_source\*.obj ^
  obj\media\rtc_internal_video_codecs\*.obj ^
  obj\media\rtc_simulcast_encoder_adapter\*.obj
```

Headers go into `dist/include` the way the script does it on Linux, libyuv flattened included. Then two marker files, both read by `cmake/Findlibwebrtc.cmake`:

- `DV_SYSTEM_LIBCXX`, because the tree was built with `use_custom_libcxx=false`.
- `DV_EXTERNAL_SSL`, because it was built with `rtc_build_ssl=false` and something else has to bring the OpenSSL. Without it the client link ends in 49 unresolved externals starting at `SSL_CTX_set_options`.

### 8.4 What it produced

A `partyshare.exe` of 30 MB holding Qt, libwebrtc and OpenSSL at once, which is the combination the prebuilt package makes impossible, and 24 of the 25 media tests passing. The one that did not is in section 6 of [webrtc-toolchain.md](webrtc-toolchain.md).

The `dist` tree this produces is published as a release asset, under the `webrtc-m152.7977.0.0-windows-x64` tag, because a release pipeline cannot be asked to repeat any of the above on every tag. `cmake/Findlibwebrtc.cmake` holds its URL and its SHA-256 and fetches it by default on Windows. Rebuilding it - a new milestone, a different GN argument - means repeating section 8, uploading the result, and changing `_dv_url_windows_x64_md` and `_dv_sha_windows_x64_md` together.

In both cases, set aside the time: the checkout is over 30 GB and the build takes tens of minutes.
