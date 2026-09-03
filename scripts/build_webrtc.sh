#!/usr/bin/env bash
# Builds libwebrtc from source against the system C++ standard library.
#
# Why this exists: the published prebuilt binaries are compiled against
# Chromium's own libc++, whose symbols live in the ABI namespace std::__Cr.
# The public libwebrtc API passes std::string and std::vector across every
# call, and Qt on Linux is built against libstdc++, so the two cannot share a
# binary. Building with use_custom_libcxx=false removes the conflict.
# See section 5 of docs/07-webrtc-toolchain.md.
#
# Usage:
#   scripts/build_webrtc.sh [--jobs N] [--out DIR] [--milestone BRANCH]
#                          [--ssl-root OPENSSL_INCLUDE_DIR]
#
# --ssl-root builds without the bundled BoringSSL, against the OpenSSL headers
# in that directory, and marks the tree DV_EXTERNAL_SSL so the consumer links an
# OpenSSL of its own.
#
# Needed wherever the binary that consumes libwebrtc also links OpenSSL, which
# here is anything joining the client media layer to dv_server_core: the SFU
# links OpenSSL::Crypto, libwebrtc carries BoringSSL, and the two define the
# same names. That is every platform this project builds on, not only macOS.
#
# macOS was where it was noticed, because ld64 refuses the 932 duplicate
# symbols outright. GNU ld refuses them too - SSL_new, OBJ_nid2ln,
# X509_SIG_get0 and the rest come back as "multiple definition" while linking
# dv_media_tests. The Windows archive is already published built this way.
#
# The result is a tree that cmake/Findlibwebrtc.cmake consumes directly:
#   <out>/dist/include/...
#   <out>/dist/lib/libwebrtc.a
#   <out>/dist/VERSIONS
#   <out>/dist/DV_SYSTEM_LIBCXX     marker, see Findlibwebrtc.cmake
#   <out>/dist/DV_EXTERNAL_SSL     marker, only with --ssl-root
#
# Expect a checkout of roughly 30 GB and a build measured in tens of minutes.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

# branch-heads/7977 is milestone m152, the same one the prebuilt binaries used,
# so the M3 spike keeps compiling without API changes.
MILESTONE="branch-heads/7977"
BUILD_DIR="${WEBRTC_BUILD_DIR:-$HOME/.cache/partyshare/webrtc}"
JOBS="$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
SKIP_SYNC=0
OUT_NAME="dv-release"
SSL_ROOT=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --jobs) JOBS="$2"; shift 2 ;;
    --out) BUILD_DIR="$2"; shift 2 ;;
    --milestone) MILESTONE="$2"; shift 2 ;;
    # Reuse a checkout that is already synced. Used by build_webrtc_docker.sh,
    # which shares the host checkout with the container.
    --skip-sync) SKIP_SYNC=1; shift ;;
    --out-name) OUT_NAME="$2"; shift 2 ;;
    # Build without BoringSSL, against the OpenSSL headers in DIR.
    #
    # libwebrtc bundles BoringSSL, and libdatachannel needs an OpenSSL. Both
    # define the same symbols, so a client that links the two ends in 932
    # duplicate symbols on macOS. ld64 refuses that outright; GNU ld takes the
    # first definition and links, which is why Linux has not needed this.
    --ssl-root) SSL_ROOT="$2"; shift 2 ;;
    -h|--help) sed -n '2,27p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

case "$(uname -s)" in
  Linux)  TARGET_OS="linux" ;;
  Darwin) TARGET_OS="mac" ;;
  *) echo "Unsupported host. On Windows, follow docs/07-webrtc-toolchain.md." >&2; exit 1 ;;
esac

case "$(uname -m)" in
  x86_64|amd64) TARGET_CPU="x64" ;;
  arm64|aarch64) TARGET_CPU="arm64" ;;
  *) echo "Unsupported architecture: $(uname -m)" >&2; exit 1 ;;
esac

echo "==> host       : ${TARGET_OS}/${TARGET_CPU}"
echo "==> milestone  : ${MILESTONE}"
echo "==> build dir  : ${BUILD_DIR}"
echo "==> jobs       : ${JOBS}"

mkdir -p "${BUILD_DIR}"
cd "${BUILD_DIR}"

# --- depot_tools -------------------------------------------------------------

if [[ ! -d depot_tools ]]; then
  echo "==> cloning depot_tools"
  git clone --depth 1 https://chromium.googlesource.com/chromium/tools/depot_tools.git
fi
export PATH="${BUILD_DIR}/depot_tools:${PATH}"
# depot_tools would otherwise try to use Google's internal binary store.
export DEPOT_TOOLS_UPDATE=1
export GCLIENT_PY3=1

# --- checkout ----------------------------------------------------------------

if [[ ${SKIP_SYNC} -eq 0 ]]; then
  if [[ ! -d src ]]; then
    echo "==> fetching webrtc (this is the slow part)"
    fetch --nohooks webrtc
  fi

  cd src
  echo "==> checking out ${MILESTONE}"
  git fetch origin "${MILESTONE}:${MILESTONE}" 2>/dev/null || git fetch origin
  git checkout "${MILESTONE}"
  gclient sync -D --force --reset --with_branch_heads --with_tags
else
  echo "==> reusing the existing checkout, skipping sync"
  cd src
fi

# --- patches -----------------------------------------------------------------

# Building against libstdc++ needs a few changes that upstream does not carry,
# because upstream only ever builds against its own libc++. The patches live in
# patches/webrtc/<repo>/, where <repo> is the gclient checkout they apply to:
# "src" is WebRTC itself, "build" is the shared Chromium build system, which is
# a separate git repository.
apply_patches() {
  local repo_dir="$1"
  local patch_dir="${REPO_ROOT}/patches/webrtc/$(basename "${repo_dir}")"
  [[ -d "${patch_dir}" ]] || return 0

  for patch in "${patch_dir}"/*.patch; do
    [[ -e "${patch}" ]] || continue
    local name
    name="$(basename "${patch}")"
    if git -C "${repo_dir}" apply --reverse --check "${patch}" 2>/dev/null; then
      echo "==> patch already applied: $(basename "${repo_dir}")/${name}"
    elif git -C "${repo_dir}" apply --check "${patch}" 2>/dev/null; then
      echo "==> applying patch: $(basename "${repo_dir}")/${name}"
      git -C "${repo_dir}" apply "${patch}"
    else
      echo "ERROR: ${name} does not apply to this checkout." >&2
      echo "       The pinned milestone probably moved. Regenerate the patch." >&2
      exit 1
    fi
  done
}

apply_patches "$(pwd)"
apply_patches "$(pwd)/build"

# --- C++ standard library ----------------------------------------------------

# Only a narrow range of libstdc++ versions can build WebRTC with clang.
# Too old and it lacks C++20 library pieces WebRTC uses, such as
# std::make_unique_for_overwrite, which arrived in GCC 11. Too new and clang
# and libstdc++ disagree with each other: on GCC 16, std::is_constructible_v<T>
# reports false for a plain aggregate that clang's own __is_constructible(T)
# reports as true, which breaks most of <optional> and <variant>.
#
# So the headers are pinned rather than taken from the host. Only headers are
# needed: the final link happens in the consuming application, against
# whatever libstdc++ that machine has, and libstdc++ keeps ABI compatibility.
LIBSTDCXX_DIR="${BUILD_DIR}/libstdcxx"
LIBSTDCXX_DEB_URL="${DV_LIBSTDCXX_DEB_URL:-http://deb.debian.org/debian/pool/main/g/gcc-14/libstdc++-14-dev_14.2.0-19_amd64.deb}"
LIBSTDCXX_DEB_SHA256="4b962fac5f1af0bd8b1b3f97e0f47b9d8e9a79e88802143f00ebfbd78ad87a7b"

ensure_libstdcxx() {
  if [[ -d "${LIBSTDCXX_DIR}/usr/include/c++/14" ]]; then
    echo "==> libstdc++ headers already unpacked"
    return 0
  fi

  echo "==> fetching pinned libstdc++ headers"
  mkdir -p "${LIBSTDCXX_DIR}"
  local deb="${LIBSTDCXX_DIR}/libstdcxx.deb"
  curl -sfL -o "${deb}" "${LIBSTDCXX_DEB_URL}"

  local actual
  actual="$(sha256sum "${deb}" | cut -d' ' -f1)"
  if [[ "${actual}" != "${LIBSTDCXX_DEB_SHA256}" ]]; then
    echo "ERROR: checksum mismatch for ${LIBSTDCXX_DEB_URL}" >&2
    echo "       expected ${LIBSTDCXX_DEB_SHA256}" >&2
    echo "       got      ${actual}" >&2
    echo "       Debian drops superseded versions from the pool. Point" >&2
    echo "       DV_LIBSTDCXX_DEB_URL at snapshot.debian.org and update the sum." >&2
    exit 1
  fi

  (cd "${LIBSTDCXX_DIR}" && ar x libstdcxx.deb && tar xf data.tar.*)
  echo "==> libstdc++ headers unpacked into ${LIBSTDCXX_DIR}"
}

# --- configure ---------------------------------------------------------------

# use_custom_libcxx=false is the entire point of this script: it drops
#   Chromium's own libc++, whose std::__Cr symbols cannot share a binary with
#   Qt. See section 5 of docs/07-webrtc-toolchain.md.
# use_rtti=true matters because our code and Qt use RTTI, and polymorphic
#   libwebrtc types cross the boundary.
# rtc_use_h264 plus proprietary_codecs and ffmpeg_branding give the H.264
#   support section 6 of SPEC.md requires.
# use_sysroot=false uses this machine's glibc. The C++ standard library is
#   pinned separately, see ensure_libstdcxx above: Chromium's own sysroot only
#   carries libstdc++ 10, which is too old for the C++20 library features
#   WebRTC uses.
GN_ARGS=(
  "target_os=\"${TARGET_OS}\""
  "target_cpu=\"${TARGET_CPU}\""
  "is_debug=false"
  "is_component_build=false"
  "use_custom_libcxx=false"
  "use_rtti=true"
  "rtc_include_tests=false"
  "rtc_build_examples=false"
  "rtc_build_tools=false"
  "rtc_use_h264=true"
  "proprietary_codecs=true"
  "ffmpeg_branding=\"Chrome\""
  "treat_warnings_as_errors=false"
)

# The two pairs have to agree with each other, or pc/BUILD.gn refuses the
# configure with "Mismatch ssl build settings detected" and then with
# "Mismatch in ssl root detected". Section 7.2 of docs/08-webrtc-validation.md.
if [[ -n "${SSL_ROOT}" ]]; then
  GN_ARGS+=(
    "rtc_build_ssl=false"
    "rtc_ssl_root=\"${SSL_ROOT}\""
    "libsrtp_build_boringssl=false"
    "libsrtp_ssl_root=\"${SSL_ROOT}\""
  )
fi

if [[ "${TARGET_OS}" == "linux" ]]; then
  ensure_libstdcxx
  _inc="${LIBSTDCXX_DIR}/usr/include"
  GN_ARGS+=(
    "use_sysroot=false"
    "rtc_use_pipewire=true"
    "rtc_use_x11=true"
    # Chromium turns on ELF CREL relocations whenever it links with lld, and
    # only lld can read them. That would make the resulting archive unusable
    # with the system linker, so every consumer, the Qt client included, would
    # have to switch to lld. Turning lld off entirely does not work either:
    # the system linker crashes while linking the host tools this build needs.
    # So lld stays, and only CREL is skipped.
    "dv_disable_crel=true"
    "dv_libstdcxx_include_dirs=[\"${_inc}/c++/14\",\"${_inc}/x86_64-linux-gnu/c++/14\",\"${_inc}/c++/14/backward\"]"
  )
fi

OUT="out/${OUT_NAME}"
echo "==> gn gen ${OUT}"
gn gen "${OUT}" --args="${GN_ARGS[*]}"

echo "==> effective arguments"
gn args "${OUT}" --list --short --overrides-only

# --- build -------------------------------------------------------------------

# The umbrella "webrtc" target does not carry everything an application needs.
# CreateBuiltinVideoEncoderFactory and CreateBuiltinVideoDecoderFactory live in
# GN targets of their own, and a consumer that calls them fails to link against
# obj/libwebrtc.a alone. AdaptedVideoTrackSource, which is the base class for
# feeding captured frames into a peer connection, is another. They are built
# here and merged in during packaging, together with whatever they pull in
# transitively.
EXTRA_TARGETS=(
  "//api/video_codecs:builtin_video_encoder_factory"
  "//api/video_codecs:builtin_video_decoder_factory"
  "//api/video:adapted_video_track_source"
  # The Windows Core Audio device module, CreateWindowsCoreAudioAudioDeviceModule:
  # the header ships, the symbol did not, because this target is not part of
  # the webrtc library target. Step 12 of docs/16-audio-plan.md is what wants
  # it. Harmless on the other platforms, where the target compiles to nothing.
  "//modules/audio_device:audio_device_module_from_input_and_output"
)

echo "==> building"
# The labels above are GN's, and ninja does not speak GN. It collapses the
# leading "//" to a single "/" and then fails with "unknown target
# '/api/video_codecs:builtin_video_encoder_factory'", suggesting the same name
# without the slash. That name is the one gn writes into build.ninja, so the
# prefix is stripped here and kept on EXTRA_TARGETS, which gn desc needs below.
ninja -C "${OUT}" -j "${JOBS}" webrtc "${EXTRA_TARGETS[@]#//}"

# --- package -----------------------------------------------------------------

# The archives GN emits are thin: they hold paths to object files rather than
# the objects themselves. GNU ar reads that format and Apple's does not, so on
# macOS every one of them comes back as "Inappropriate file type or format" and
# the merge below silently produces an archive missing the extra targets.
#
# The llvm-ar in the checkout is the one the build itself used, so it reads what
# the build wrote on every platform. It has no llvm-ranlib beside it; "ar s" is
# the same operation and llvm-ar implements it.
AR_BIN="ar"
RANLIB_CMD=(ranlib)
# Absolute: the merge below runs from inside ${OUT}.
_dv_llvm_ar="${BUILD_DIR}/src/third_party/llvm-build/Release+Asserts/bin/llvm-ar"
if [[ -x "${_dv_llvm_ar}" ]]; then
  AR_BIN="${_dv_llvm_ar}"
  RANLIB_CMD=("${_dv_llvm_ar}" s)
fi

# install -D is a GNU extension. BSD install, which is what macOS ships, does
# not create the leading directories and fails on the first header of every
# subdirectory. mkdir -p and cp are the same thing everywhere.
install_header() {
  mkdir -p "$(dirname "$2")"
  cp "$1" "$2"
  chmod 644 "$2"
}

DIST="${BUILD_DIR}/dist"
echo "==> packaging into ${DIST}"
rm -rf "${DIST}"
mkdir -p "${DIST}/lib" "${DIST}/include"

cp "${OUT}/obj/libwebrtc.a" "${DIST}/lib/libwebrtc.a"

# Merge the extra targets into the archive.
#
# Each one is expanded to its full transitive dependency set, because the
# dependencies are not in obj/libwebrtc.a either. The archives GN emits are
# thin, so what gets appended are the object files they point at.
#
# Objects whose name is already in obj/libwebrtc.a are skipped, which is what
# keeps the vast majority of those dependencies from being added twice. The
# comparison is by name because a fat archive records nothing else, so two
# unrelated objects sharing a name would cost one of them. That would surface
# as an undefined reference at link time, never as a silently wrong binary.
(
  cd "${OUT}"

  archives=()
  for target in "${EXTRA_TARGETS[@]}"; do
    for dep in "${target}" $(gn desc . "${target}" deps --all); do
      # //api/video_codecs:builtin_video_encoder_factory becomes
      # obj/api/video_codecs/libbuiltin_video_encoder_factory.a
      path="${dep#//}"
      archive="obj/${path%:*}/lib${dep##*:}.a"

      # Only WebRTC's own tree. The dependency closure reaches into
      # third_party, and taking it wholesale drags in things like protobuf's
      # runtime and even protoc itself, in pieces that do not link. Whatever
      # WebRTC actually uses from third_party is already in obj/libwebrtc.a.
      case "${archive}" in
        obj/third_party/*) continue ;;
      esac

      # Header-only and group targets produce no archive at all.
      if [[ -f "${archive}" ]]; then
        archives+=("${archive}")
      fi
    done
  done

  # Every object name already in the archive, one per line, so that a member is
  # only appended when it is genuinely new. Appending to this list as objects
  # are picked also keeps two extra targets from contributing the same one.
  seen="$("${AR_BIN}" t "${DIST}/lib/libwebrtc.a")"
  missing=()
  for archive in "${archives[@]}"; do
    while IFS= read -r member; do
      name="$(basename "${member}")"
      if ! grep -qxF "${name}" <<< "${seen}"; then
        missing+=("${member}")
        seen+=$'\n'"${name}"
      fi
    done < <("${AR_BIN}" t "${archive}")
  done

  if (( ${#missing[@]} > 0 )); then
    echo "    merging ${#missing[@]} objects from ${#EXTRA_TARGETS[@]} extra targets"
    "${AR_BIN}" q "${DIST}/lib/libwebrtc.a" "${missing[@]}"
    "${RANLIB_CMD[@]}" "${DIST}/lib/libwebrtc.a"
  fi
)

# Headers, mirroring the layout the prebuilt archives use so that
# Findlibwebrtc.cmake needs no special case.
for dir in api audio call common_audio common_video logging media modules net p2p pc \
           rtc_base rtc_tools sdk stats system_wrappers video; do
  [[ -d "${dir}" ]] || continue
  find "${dir}" \( -name '*.h' -o -name '*.hpp' \) -print0 |
    while IFS= read -r -d '' header; do
      install_header "${header}" "${DIST}/include/${header}"
    done
done

# The bundled abseil has to travel with the headers: libwebrtc's public headers
# include absl/... and a system abseil is not interchangeable.
find third_party/abseil-cpp/absl \( -name '*.h' -o -name '*.inc' \) -print0 |
  while IFS= read -r -d '' header; do
    install_header "${header}" "${DIST}/include/third_party/abseil-cpp/${header#third_party/abseil-cpp/}"
  done

# libyuv, for the same reason. Its objects are already in the archive, because
# libwebrtc itself scales and converts with it, but the headers are not part of
# libwebrtc's public surface and would otherwise be missing.
#
# The screen share needs them directly: a captured frame is BGRA at the size of
# the monitor and has to become I420 at 1280x720, and hand rolling that scaler
# would be slower and worse than the tuned one already sitting in the archive.
find third_party/libyuv/include \( -name '*.h' \) -print0 |
  while IFS= read -r -d '' header; do
    install_header "${header}" "${DIST}/include/${header#third_party/libyuv/include/}"
  done

{
  echo "WEBRTC_BUILD_MILESTONE=${MILESTONE}"
  echo "WEBRTC_SRC_COMMIT=$(git rev-parse HEAD)"
  echo "WEBRTC_BUILT_AT=$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "WEBRTC_TARGET=${TARGET_OS}/${TARGET_CPU}"
} > "${DIST}/VERSIONS"

# Findlibwebrtc.cmake looks for this marker to know it must not apply the
# bundled libc++ flags.
touch "${DIST}/DV_SYSTEM_LIBCXX"

# And this one to know the tree carries no SSL of its own, so the consumer has
# to link OpenSSL. Without it the client link ends in unresolved externals
# starting at SSL_CTX_set_options.
if [[ -n "${SSL_ROOT}" ]]; then
  touch "${DIST}/DV_EXTERNAL_SSL"
fi

echo
echo "==> done"
echo "    library : ${DIST}/lib/libwebrtc.a"
echo "    headers : ${DIST}/include"
echo
echo "Use it with:"
echo "    cmake -S . -B build/spike -DDV_ENABLE_WEBRTC_SPIKE=ON -DDV_WEBRTC_ROOT=${DIST}"
