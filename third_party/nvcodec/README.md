# nvcodec

`nvEncodeAPI.h`, the header that declares NVIDIA's video encoder API.

| | |
| --- | --- |
| Origin | https://github.com/FFmpeg/nv-codec-headers, `include/ffnvcodec/nvEncodeAPI.h` |
| API version | 13.1 |
| License | MIT, in the header of the file itself |

## Why it is here

NVENC has no library to link against: the encoder library ships with the driver — `libnvidia-encode.so` on Linux, `nvEncodeAPI64.dll` on Windows — and is opened at runtime, so all the program needs at compile time is the declaration of the structs and enums that cross that boundary.

That declaration is not distributed as a package on every platform, and the project cannot depend on the developer having installed the Video Codec SDK.
ffmpeg solves it the same way, and this file comes from there.

Nothing is linked: `client/src/webrtc/hardware_encoder_nvenc.cpp` opens the encoder library and the CUDA driver through `client/src/webrtc/dynamic_library.hpp`, which is `dlopen` on Linux and `LoadLibrary` on Windows, so a binary compiled with NVENC runs just the same on a machine with no NVIDIA card at all.

## How to update it

Download the file again from the repository above and update the version in the table.
The structs carry a `version` field, and NVENC refuses a version newer than the driver knows about, so raising this without needing to only shrinks the set of machines where the encoder works.
