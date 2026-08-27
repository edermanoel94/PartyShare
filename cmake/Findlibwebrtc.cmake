# Findlibwebrtc.cmake
#
# Resolves a prebuilt libwebrtc and exposes it as libwebrtc::libwebrtc.
#
# There is no official libwebrtc release in library form, so this module pins a
# specific build from the shiguredo-webrtc-build project and verifies it against
# a SHA-256 recorded here.
#
# Everything in this file that looks surprising was found by running the M3
# spike. See docs/07-webrtc-toolchain.md for the full account. In short:
#
#   1. The archive extracts to a "webrtc/" subdirectory.
#   2. The build uses Chromium's libc++ with the ABI namespace __Cr, and the
#      archive ships that libc++ with its top level headers stripped. The full
#      header set has to be fetched separately, at the commit the build pins.
#   3. The bundled abseil must come before any system abseil on the include
#      path, or the two disagree about std:: types.
#   4. WEBRTC_USE_X11 and WEBRTC_USE_PIPEWIRE change the layout of public
#      structs such as DesktopCaptureOptions. Getting them wrong does not fail
#      to compile, it corrupts the stack at runtime.
#
# Inputs:
#   DV_WEBRTC_VERSION       release tag to fetch
#   DV_WEBRTC_ROOT          use an already extracted tree and skip downloading
#   DV_WEBRTC_URL           override the archive URL
#   DV_WEBRTC_SHA256        override the expected checksum
#   DV_WEBRTC_LINUX_FLAVOR  which Ubuntu build to use on Linux (24.04 or 22.04)
#   DV_WEBRTC_WINDOWS_DYNAMIC_CRT
#                           on Windows, fetch the tree built with
#                           win_use_dynamic_crt=true, which is the only one that
#                           links next to Qt and vcpkg. Off gives the published
#                           /MT package, which is what the spike wants.
#   DV_WEBRTC_USE_BUNDLED_LIBCXX
#                           on Linux, compile consumers against the libc++ this
#                           build was made with. Required for the published
#                           binaries, and switched off automatically for a tree
#                           produced by scripts/build_webrtc.sh, which is built
#                           against the system standard library instead.
#
# Outputs:
#   libwebrtc::libwebrtc, libwebrtc_FOUND

include(FetchContent)

set(DV_WEBRTC_VERSION "m152.7977.0.0" CACHE STRING "Pinned libwebrtc build")
set(DV_WEBRTC_ROOT "" CACHE PATH "Pre-extracted libwebrtc tree")
set(DV_WEBRTC_URL "" CACHE STRING "Override the libwebrtc archive URL")
set(DV_WEBRTC_SHA256 "" CACHE STRING "Override the expected archive checksum")
set(DV_WEBRTC_LINUX_FLAVOR "24.04" CACHE STRING "Ubuntu build flavor on Linux")
option(DV_WEBRTC_USE_BUNDLED_LIBCXX "Compile against the libc++ libwebrtc was built with" ON)
option(DV_WEBRTC_ALLOW_UNVERIFIED "Allow a download with no recorded checksum" OFF)

# Which of the two Windows trees to fetch.
#
# The published shiguredo package is built against the static C runtime, /MT,
# and Qt and everything vcpkg's x64-windows triplet produces is /MD. Linking
# the two ends in LNK2038 repeated across the standard library, so the client
# cannot use that archive at all: section 5 of docs/07-webrtc-toolchain.md.
#
# With this on, the fetch is a tree built from the same milestone with
# win_use_dynamic_crt=true and rtc_build_ssl=false, published from this
# repository because reproducing it takes a 30 GB Chromium checkout. Section 7
# of docs/08-webrtc-validation.md is the procedure that made it.
#
# Turn it off for the M3 spike, which is configured standalone with
# CMAKE_MSVC_RUNTIME_LIBRARY=MultiThreaded and wants the /MT package.
option(DV_WEBRTC_WINDOWS_DYNAMIC_CRT
       "On Windows, fetch the libwebrtc built against the dynamic CRT" ON)

set(_dv_webrtc_base
    "https://github.com/shiguredo-webrtc-build/webrtc-build/releases/download/${DV_WEBRTC_VERSION}")

# Checksums for ${DV_WEBRTC_VERSION}. Regenerate with scripts/webrtc_checksums.sh.
set(_dv_sha_macos_arm64 "f5223945d0c1d71fad15f806bf19ec9314cc86ad6ddd9fac2421d73035228871")
set(_dv_sha_ubuntu_2404 "d8cd09982a9674b47a1564c651df2229d3bbc2243d77e09e90f3bcba99daba9f")
set(_dv_sha_ubuntu_2204 "d7e8a15ac6419c8fff5fd0714ce27304375ab4de2d67d7bd0d85388b8a5b8085")
set(_dv_sha_windows_x64 "fbc4afe2f9e0a8a42ca8afb94f0ba37511c0dba7728be012edee86ee848435a4")

# The dynamic CRT tree is not one of the four above: it is built by hand and
# published from this repository, so its URL is recorded next to its checksum.
set(_dv_url_windows_x64_md
    "https://github.com/edermanoel94/PartyShare/releases/download/webrtc-${DV_WEBRTC_VERSION}-windows-x64/webrtc-${DV_WEBRTC_VERSION}-windows-x64-dynamic-crt.tar.gz")
set(_dv_sha_windows_x64_md "6c2056ec726ce947501a6a764a544543813da4773b9d0b4036c93c24d8146954")

# --- select the archive -------------------------------------------------------

if(NOT DV_WEBRTC_ROOT)
  if(WIN32)
    if(DV_WEBRTC_WINDOWS_DYNAMIC_CRT)
      set(_dv_url "${_dv_url_windows_x64_md}")
      set(_dv_sha "${_dv_sha_windows_x64_md}")
    else()
      set(_dv_url "${_dv_webrtc_base}/webrtc.windows_x86_64.zip")
      set(_dv_sha "${_dv_sha_windows_x64}")
    endif()
  elseif(APPLE)
    if(CMAKE_OSX_ARCHITECTURES AND NOT CMAKE_OSX_ARCHITECTURES STREQUAL "arm64")
      # SPEC.md lists macOS x64 as desirable, not mandatory. This distribution
      # publishes no x86_64 macOS build.
      message(FATAL_ERROR
        "No prebuilt libwebrtc exists for macOS ${CMAKE_OSX_ARCHITECTURES}. "
        "Point DV_WEBRTC_ROOT at a tree you built yourself.")
    endif()
    set(_dv_url "${_dv_webrtc_base}/webrtc.macos_arm64.tar.gz")
    set(_dv_sha "${_dv_sha_macos_arm64}")
  elseif(UNIX)
    if(DV_WEBRTC_LINUX_FLAVOR STREQUAL "22.04")
      set(_dv_url "${_dv_webrtc_base}/webrtc.ubuntu-22.04_x86_64.tar.gz")
      set(_dv_sha "${_dv_sha_ubuntu_2204}")
    else()
      set(_dv_url "${_dv_webrtc_base}/webrtc.ubuntu-24.04_x86_64.tar.gz")
      set(_dv_sha "${_dv_sha_ubuntu_2404}")
    endif()
  else()
    message(FATAL_ERROR "Unsupported platform for libwebrtc")
  endif()

  if(DV_WEBRTC_URL)
    set(_dv_url "${DV_WEBRTC_URL}")
    set(_dv_sha "${DV_WEBRTC_SHA256}")
  endif()

  if(NOT _dv_sha AND NOT DV_WEBRTC_ALLOW_UNVERIFIED)
    message(FATAL_ERROR
      "No checksum recorded for ${_dv_url}. Record one in Findlibwebrtc.cmake, "
      "pass -DDV_WEBRTC_SHA256=..., or set DV_WEBRTC_ALLOW_UNVERIFIED=ON.")
  endif()

  message(STATUS "libwebrtc: fetching ${_dv_url}")
  FetchContent_Declare(libwebrtc_binary
    URL "${_dv_url}"
    URL_HASH SHA256=${_dv_sha}
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SOURCE_SUBDIR dv-headers-only
  )
  FetchContent_MakeAvailable(libwebrtc_binary)
  set(DV_WEBRTC_ROOT "${libwebrtc_binary_SOURCE_DIR}")
endif()

# The archive extracts into a "webrtc" subdirectory, but a hand built tree may
# not, so both layouts are searched.
find_path(DV_WEBRTC_INCLUDE_DIR
  NAMES api/peer_connection_interface.h
  PATHS "${DV_WEBRTC_ROOT}/webrtc/include" "${DV_WEBRTC_ROOT}/include" "${DV_WEBRTC_ROOT}"
  NO_DEFAULT_PATH
)

find_library(DV_WEBRTC_LIBRARY
  NAMES webrtc libwebrtc
  PATHS "${DV_WEBRTC_ROOT}/webrtc/lib" "${DV_WEBRTC_ROOT}/lib" "${DV_WEBRTC_ROOT}"
  NO_DEFAULT_PATH
)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(libwebrtc
  REQUIRED_VARS DV_WEBRTC_INCLUDE_DIR DV_WEBRTC_LIBRARY
  VERSION_VAR DV_WEBRTC_VERSION
)

if(NOT libwebrtc_FOUND)
  return()
endif()

cmake_path(GET DV_WEBRTC_INCLUDE_DIR PARENT_PATH _dv_webrtc_prefix)

# scripts/build_webrtc.sh drops this marker. A tree carrying it was built with
# use_custom_libcxx=false, so none of the libc++ handling below applies and the
# result links normally next to Qt.
if(EXISTS "${_dv_webrtc_prefix}/DV_SYSTEM_LIBCXX")
  message(STATUS "libwebrtc: built against the system standard library")
  set(DV_WEBRTC_USE_BUNDLED_LIBCXX OFF CACHE BOOL "" FORCE)
endif()

# A tree built with rtc_build_ssl=false has no BoringSSL inside it, and expects
# whoever links it to bring an OpenSSL. That is the point on Windows: the
# prebuilt archives bundle BoringSSL, and its symbols collide with the OpenSSL
# libdatachannel needs, so the SFU and the client cannot be linked into the same
# binary. Dropping the bundled copy is what makes one process able to hold both.
#
# Marked by a file rather than guessed from the platform, for the same reason
# DV_SYSTEM_LIBCXX is: it is a property of how the tree was built, and the
# alternative is 49 unresolved externals naming SSL_CTX_set_options.
if(EXISTS "${_dv_webrtc_prefix}/DV_EXTERNAL_SSL")
  set(DV_WEBRTC_EXTERNAL_SSL ON)
endif()

add_library(libwebrtc::libwebrtc STATIC IMPORTED GLOBAL)
set_target_properties(libwebrtc::libwebrtc PROPERTIES
  IMPORTED_LOCATION "${DV_WEBRTC_LIBRARY}"
)

# The bundled abseil has to win over any system copy: a mismatch between the
# two shows up as std:: template errors deep inside absl/strings.
target_include_directories(libwebrtc::libwebrtc SYSTEM INTERFACE
  "${DV_WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp"
  "${DV_WEBRTC_INCLUDE_DIR}"
)

# libyuv is included as <libyuv/convert.h>, and the two trees disagree about
# where that lives. The one scripts/build_webrtc.sh produces flattens it into
# the top include directory; the prebuilt archives keep the Chromium layout and
# leave it under third_party/libyuv/include. Added when it is there rather than
# by platform, because it is a property of the tree and not of the operating
# system: without it the screen capturer and the media session do not compile,
# and they are the two files that convert frames.
if(EXISTS "${DV_WEBRTC_INCLUDE_DIR}/third_party/libyuv/include/libyuv/convert.h")
  target_include_directories(libwebrtc::libwebrtc SYSTEM INTERFACE
    "${DV_WEBRTC_INCLUDE_DIR}/third_party/libyuv/include")
endif()

# Which shape PacketSocketFactory::CreateClientTcpSocket has.
#
# Both trees call themselves m152, and their headers disagree: the prebuilt
# archives carry a six parameter form with a proxy and a user agent in the
# middle, in a webrtc::revive namespace that upstream does not have, and the
# source build carries the four parameter form the code was written against.
# An override that guesses wrong is not a warning, it is C3668, so the answer is
# measured here instead.
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_INCLUDES
  "${DV_WEBRTC_INCLUDE_DIR}/third_party/abseil-cpp" "${DV_WEBRTC_INCLUDE_DIR}")
# The same definitions the target carries below. Without them the header does
# not parse at all and the probe answers "no proxy" for the wrong reason, which
# is the failure mode a silent check has: it is never wrong, it is only useless.
if(WIN32)
  set(CMAKE_REQUIRED_DEFINITIONS
    -DWEBRTC_WIN -DNOMINMAX -DWIN32_LEAN_AND_MEAN -D_WINSOCKAPI_)
elseif(APPLE)
  set(CMAKE_REQUIRED_DEFINITIONS -DWEBRTC_POSIX -DWEBRTC_MAC)
else()
  set(CMAKE_REQUIRED_DEFINITIONS -DWEBRTC_POSIX -DWEBRTC_LINUX)
endif()
set(CMAKE_REQUIRED_QUIET ON)
check_cxx_source_compiles("
#include <api/packet_socket_factory.h>
using P = webrtc::revive::ProxyInfo;
int main() { return 0; }
" DV_WEBRTC_TCP_SOCKET_TAKES_PROXY)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_DEFINITIONS)
unset(CMAKE_REQUIRED_QUIET)
if(DV_WEBRTC_TCP_SOCKET_TAKES_PROXY)
  target_compile_definitions(libwebrtc::libwebrtc INTERFACE
    DV_WEBRTC_TCP_SOCKET_TAKES_PROXY=1)
  message(STATUS "libwebrtc: CreateClientTcpSocket takes a proxy and a user agent")
endif()

if(DV_WEBRTC_EXTERNAL_SSL)
  message(STATUS "libwebrtc: linking the external OpenSSL rather than a bundled BoringSSL")
  find_package(OpenSSL REQUIRED)
  target_link_libraries(libwebrtc::libwebrtc INTERFACE OpenSSL::SSL OpenSSL::Crypto)
endif()

# --- platform usage requirements ---------------------------------------------

if(WIN32)
  target_compile_definitions(libwebrtc::libwebrtc INTERFACE
    WEBRTC_WIN NOMINMAX WIN32_LEAN_AND_MEAN _WINSOCKAPI_ RTC_ENABLE_WIN_WGC)
  # dwmapi comes with RTC_ENABLE_WIN_WGC above rather than on its own: Windows
  # Graphics Capture asks the desktop window manager whether a window is cloaked
  # before it captures it, and window_capture_utils.obj and wgc_capture_source.obj
  # both call DwmGetWindowAttribute. Without it the link fails on that one symbol
  # after everything else has resolved, which is how the first Windows build of
  # the spike ended.
  target_link_libraries(libwebrtc::libwebrtc INTERFACE
    winmm ws2_32 secur32 dmoguids wmcodecdspuuid msdmo strmiids iphlpapi
    dxgi d3d11 shcore dwmapi)

elseif(APPLE)
  target_compile_definitions(libwebrtc::libwebrtc INTERFACE WEBRTC_POSIX WEBRTC_MAC)
  foreach(framework
      Foundation AVFoundation CoreAudio AudioToolbox CoreMedia CoreVideo
      CoreGraphics IOSurface Metal MetalKit VideoToolbox AppKit ScreenCaptureKit)
    target_link_libraries(libwebrtc::libwebrtc INTERFACE "-framework ${framework}")
  endforeach()

else()
  # These two are not optional. They change the layout of public structs such as
  # DesktopCaptureOptions, and a mismatch corrupts the caller's stack at runtime
  # with no compile time warning at all.
  target_compile_definitions(libwebrtc::libwebrtc INTERFACE
    WEBRTC_POSIX WEBRTC_LINUX WEBRTC_USE_X11 WEBRTC_USE_PIPEWIRE)

  find_package(Threads REQUIRED)
  target_link_libraries(libwebrtc::libwebrtc INTERFACE Threads::Threads ${CMAKE_DL_LIBS})

  find_package(X11 REQUIRED)
  target_link_libraries(libwebrtc::libwebrtc INTERFACE
    X11::X11 X11::Xext X11::Xfixes X11::Xdamage X11::Xrandr X11::Xcomposite X11::Xtst)

  # WEBRTC_USE_PIPEWIRE brings in the XDG desktop portal path of
  # modules/desktop_capture, which is what makes screen capture work on
  # Wayland. That code talks to the portal over GDBus and imports the captured
  # frames as DMA-BUFs, so it needs glib, gio, gobject, gbm and libdrm at link
  # time. PipeWire itself is loaded with dlopen, so it is not listed here.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(DV_WEBRTC_PORTAL REQUIRED IMPORTED_TARGET
    glib-2.0 gio-2.0 gobject-2.0 gbm libdrm)
  target_link_libraries(libwebrtc::libwebrtc INTERFACE PkgConfig::DV_WEBRTC_PORTAL)

  if(DV_WEBRTC_USE_BUNDLED_LIBCXX)
    # This build is compiled against Chromium's libc++, whose symbols live in
    # the ABI namespace std::__Cr. Anything that shares std:: types with it has
    # to use the same headers. The archive ships that libc++ with every
    # extensionless header removed, so the full set is fetched at the exact
    # commit the build pins in its VERSIONS file.
    set(_dv_versions_file "${_dv_webrtc_prefix}/VERSIONS")
    if(NOT EXISTS "${_dv_versions_file}")
      message(FATAL_ERROR "libwebrtc: ${_dv_versions_file} not found, cannot pin libc++")
    endif()
    file(STRINGS "${_dv_versions_file}" _dv_libcxx_line
         REGEX "^WEBRTC_SRC_THIRD_PARTY_LIBCXX_SRC_COMMIT=")
    string(REGEX REPLACE "^.*=" "" _dv_libcxx_commit "${_dv_libcxx_line}")
    if(NOT _dv_libcxx_commit)
      message(FATAL_ERROR "libwebrtc: could not read the pinned libc++ commit")
    endif()
    message(STATUS "libwebrtc: libc++ pinned at ${_dv_libcxx_commit}")

    FetchContent_Declare(libwebrtc_libcxx
      URL "https://chromium.googlesource.com/external/github.com/llvm/llvm-project/libcxx.git/+archive/${_dv_libcxx_commit}/include.tar.gz"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      # These are headers, not a project to build. The archive carries a
      # CMakeLists.txt that must not be configured.
      SOURCE_SUBDIR dv-headers-only
    )
    FetchContent_MakeAvailable(libwebrtc_libcxx)
    set(_dv_libcxx_include "${libwebrtc_libcxx_SOURCE_DIR}")

    # Both headers are normally generated by libc++'s own CMake build. The
    # values below mirror the Chromium configuration this build uses; they were
    # confirmed by running the M3 spike.
    file(WRITE "${_dv_libcxx_include}/__config_site"
"#ifndef _LIBCPP___CONFIG_SITE
#define _LIBCPP___CONFIG_SITE
#define _LIBCPP_ABI_VERSION 2
#define _LIBCPP_ABI_NAMESPACE __Cr
#define _LIBCPP_ABI_FORCE_ITANIUM 0
#define _LIBCPP_ABI_FORCE_MICROSOFT 0
#define _LIBCPP_HAS_THREADS 1
#define _LIBCPP_HAS_MONOTONIC_CLOCK 1
#define _LIBCPP_HAS_MUSL_LIBC 0
#define _LIBCPP_HAS_THREAD_API_PTHREAD 1
#define _LIBCPP_HAS_THREAD_API_EXTERNAL 0
#define _LIBCPP_HAS_THREAD_API_WIN32 0
#define _LIBCPP_HAS_THREAD_API_C11 0
#define _LIBCPP_HAS_VENDOR_AVAILABILITY_ANNOTATIONS 0
#define _LIBCPP_HAS_FILESYSTEM 1
#define _LIBCPP_HAS_RANDOM_DEVICE 1
#define _LIBCPP_HAS_LOCALIZATION 1
#define _LIBCPP_HAS_UNICODE 1
#define _LIBCPP_HAS_WIDE_CHARACTERS 1
#define _LIBCPP_HAS_TIME_ZONE_DATABASE 0
#define _LIBCPP_INSTRUMENTED_WITH_ASAN 0
#define _LIBCPP_PSTL_BACKEND_SERIAL
#define _LIBCPP_HARDENING_MODE_DEFAULT _LIBCPP_HARDENING_MODE_NONE
#define _LIBCPP_ASSERTION_SEMANTIC_DEFAULT _LIBCPP_ASSERTION_SEMANTIC_IGNORE
#define _LIBCPP_LIBC_PICOLIBC 0
#define _LIBCPP_LIBC_NEWLIB 0
#define _LIBCPP_LIBC_LLVM_LIBC 0
#endif
")
    file(WRITE "${_dv_libcxx_include}/__assertion_handler"
"#ifndef _LIBCPP___ASSERTION_HANDLER
#define _LIBCPP___ASSERTION_HANDLER
#include <__config>
#define _LIBCPP_ASSERTION_HANDLER(message) ((void)0)
#endif
")

    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
      message(FATAL_ERROR
        "Consuming a prebuilt libwebrtc needs clang, because it requires "
        "Chromium's libc++. The current compiler is ${CMAKE_CXX_COMPILER_ID}.\n"
        "Build libwebrtc against the system standard library instead:\n"
        "  scripts/build_webrtc.sh\n"
        "then configure with -DDV_WEBRTC_ROOT=<the dist directory it prints>.")
    endif()

    target_compile_options(libwebrtc::libwebrtc INTERFACE
      -nostdinc++ "-isystem${_dv_libcxx_include}")
    # The libc++ runtime symbols are already inside libwebrtc.a, so no standard
    # C++ library is linked in.
    target_link_options(libwebrtc::libwebrtc INTERFACE -nostdlib++)
  endif()
endif()

mark_as_advanced(DV_WEBRTC_INCLUDE_DIR DV_WEBRTC_LIBRARY)
