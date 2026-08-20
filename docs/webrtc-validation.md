# Validating libwebrtc on Windows and macOS

This document is the walkthrough for closing the M3 items the development environment cannot reach.
What remains to be validated is listed in section 6 of [webrtc-toolchain.md](webrtc-toolchain.md).

## 1. What has to be answered

| Question | Where |
| --- | --- |
| Does libwebrtc link and run on Windows x64? | A Windows machine |
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
- On **Windows and macOS** the spike tries to link `dv::shared` directly.
  If the conflict exists there too, it **does not compile**: the link fails with `std::__Cr::` symbols.
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

On Windows the build requires Visual Studio with the Windows SDK and the depot_tools toolchain, and the procedure is the one in the official WebRTC documentation.
Worth recording here once it has been done for the first time.

In both cases, set aside the time: the checkout is over 30 GB and the build takes tens of minutes.
