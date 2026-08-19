#!/usr/bin/env bash
#
# Builds the Linux artefact: a single executable file that runs on a machine
# with no Qt installed and nothing unpacked.
#
# This is task 2 of the M9. The work is not the packaging format, which is a
# squashfs with a header, but the Qt runtime: a client linked against Qt 6 needs
# the libraries, the platform plugin that talks to X11 or Wayland, and the image
# and icon plugins, and none of those are found by walking the executable's
# DT_NEEDED alone. linuxdeploy's Qt plugin knows where they live and what a Qt
# application looks for at run time, which is why this downloads a tool instead
# of copying libraries by hand.
#
# Usage:
#
#   scripts/appimage.sh                       # configure, build, package
#   scripts/appimage.sh --build-dir build/rel  # reuse an existing build
#   scripts/appimage.sh --skip-build           # only repackage what is built
#
# The result is build/appimage/partyshare-<version>-linux-<arch>.AppImage.
#
# Two things are worth knowing.
#
# First, the AppImage carries Qt and the C++ runtime but not glibc, which cannot
# be bundled. So it runs on any distribution whose glibc is at least as new as
# the one it was built against, and on none that is older. The release build
# happens on the oldest distribution the CI has for that reason.
#
# Second, running an AppImage normally needs FUSE, which a container usually
# does not have. APPIMAGE_EXTRACT_AND_RUN makes the tools unpack themselves into
# a temporary directory instead, which is slower and works everywhere.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/appimage"
APPDIR="${BUILD_DIR}/AppDir"
TOOL_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/partyshare/appimage-tools"
SKIP_BUILD=0
ARCH="$(uname -m)"

LINUXDEPLOY_URL="https://github.com/linuxdeploy/linuxdeploy/releases/download/continuous/linuxdeploy-${ARCH}.AppImage"
QT_PLUGIN_URL="https://github.com/linuxdeploy/linuxdeploy-plugin-qt/releases/download/continuous/linuxdeploy-plugin-qt-${ARCH}.AppImage"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --build-dir)
      BUILD_DIR="$2"
      APPDIR="${BUILD_DIR}/AppDir"
      shift 2
      ;;
    --skip-build)
      SKIP_BUILD=1
      shift
      ;;
    -h|--help)
      sed -n '2,30p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      exit 2
      ;;
  esac
done

log() { printf '\n=== %s\n' "$*"; }

fetch_tool() {
  local url="$1" dest="$2"
  if [[ -x "$dest" ]]; then
    return
  fi
  log "downloading $(basename "$dest")"
  mkdir -p "$(dirname "$dest")"
  # Downloaded to a temporary name and moved into place, so an interrupted
  # download cannot leave behind a truncated file that looks like a tool.
  curl --fail --location --show-error --silent --output "${dest}.part" "$url"
  chmod +x "${dest}.part"
  mv "${dest}.part" "$dest"
}

# A release build has the media layer in it. Without it the client starts, shows
# its window and cannot make a call, which is a worse artefact than none: it
# looks like the product and is not. The layer needs libwebrtc, so a tree that
# has not been built is a hard failure here rather than a warning to scroll past.
WEBRTC_ROOT="${DV_WEBRTC_ROOT:-${XDG_CACHE_HOME:-$HOME/.cache}/partyshare/webrtc/dist}"
if [[ ! -f "${WEBRTC_ROOT}/lib/libwebrtc.a" ]]; then
  cat >&2 <<MSG
no libwebrtc at ${WEBRTC_ROOT}

Build it with scripts/build_webrtc.sh, or point DV_WEBRTC_ROOT at a tree that
has one. An AppImage without the media layer would start and not make calls.
MSG
  exit 1
fi

if [[ $SKIP_BUILD -eq 0 ]]; then
  log "configuring"
  cmake -S "$REPO_ROOT" -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Release \
    -DDV_BUILD_TESTS=OFF \
    -DDV_BUILD_CLIENT_MEDIA=ON \
    -DDV_WEBRTC_ROOT="$WEBRTC_ROOT"

  log "building"
  cmake --build "$BUILD_DIR" --parallel "$(nproc)"
fi

log "staging the install tree"
rm -rf "$APPDIR"
DESTDIR="$APPDIR" cmake --install "$BUILD_DIR" --prefix /usr

VERSION="$(sed -n 's/^  VERSION \([0-9.]*\)$/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -1)"
if [[ -z "$VERSION" ]]; then
  echo "could not read the version out of CMakeLists.txt" >&2
  exit 1
fi

case "$ARCH" in
  x86_64) TAG="linux-x64" ;;
  aarch64) TAG="linux-arm64" ;;
  *) TAG="linux-${ARCH}" ;;
esac

fetch_tool "$LINUXDEPLOY_URL" "${TOOL_DIR}/linuxdeploy"
fetch_tool "$QT_PLUGIN_URL" "${TOOL_DIR}/linuxdeploy-plugin-qt"

# The Qt plugin finds Qt through qmake. Without this it picks whichever qmake is
# first on PATH, which on a machine with both Qt 5 and Qt 6 is a coin toss that
# ends in an AppImage bundling the wrong major version.
if [[ -z "${QMAKE:-}" ]]; then
  for candidate in qmake6 qmake-qt6 qmake; do
    if command -v "$candidate" >/dev/null 2>&1; then
      QMAKE="$(command -v "$candidate")"
      break
    fi
  done
fi
if [[ -z "${QMAKE:-}" ]]; then
  echo "no qmake found. Install Qt 6 or set QMAKE to its qmake." >&2
  exit 1
fi
export QMAKE

log "bundling Qt and building the AppImage"
export APPIMAGE_EXTRACT_AND_RUN=1
# linuxdeploy carries its own binutils, and it is older than the libraries on a
# rolling distribution: it rejects the .relr.dyn section that a current linker
# emits, and the failure is fatal rather than skipped. The system strip below
# understands the format, so the stripping is done here and turned off there.
export NO_STRIP=1
export PATH="${TOOL_DIR}:${PATH}"
export OUTPUT="partyshare-${VERSION}-${TAG}.AppImage"
export LDAI_UPDATE_INFORMATION="gh-releases-zsync|partyshare|partyshare|latest|partyshare-*-${TAG}.AppImage.zsync"

cd "$BUILD_DIR"

# Two passes: the first fills the AppDir, the second wraps it. Splitting them is
# what leaves a point in between where the tree can be stripped with a strip
# that understands it, which is the whole reason NO_STRIP is set above.
"${TOOL_DIR}/linuxdeploy" \
  --appdir "$APPDIR" \
  --desktop-file "${APPDIR}/usr/share/applications/partyshare.desktop" \
  --icon-file "${APPDIR}/usr/share/icons/hicolor/256x256/apps/partyshare.png" \
  --plugin qt

log "stripping"
# Debug symbols in a shipped artefact are tens of megabytes nobody downloads on
# purpose. Failures are ignored one file at a time: a library that cannot be
# stripped is a library that ships unstripped, not a release that does not ship.
find "$APPDIR" -type f \( -name '*.so' -o -name '*.so.*' \) -print0 \
  | xargs -0 --no-run-if-empty strip --strip-unneeded 2>/dev/null || true
strip --strip-unneeded "${APPDIR}/usr/bin/partyshare" 2>/dev/null || true
strip --strip-unneeded "${APPDIR}/usr/bin/partyshare-server" 2>/dev/null || true

log "wrapping the AppDir"
"${TOOL_DIR}/linuxdeploy" --appdir "$APPDIR" --output appimage

# The floor is a property of the artefact that is invisible until somebody on an
# older distribution cannot start it, so it is printed rather than left to be
# discovered. A build on a rolling distribution produces a file that runs almost
# nowhere else, which is fine for development and is not a release: the release
# job builds on the oldest runner the CI has, and that is where the number that
# matters comes from.
glibc_floor() {
  find "$APPDIR" -type f \( -name '*.so*' -o -perm -u+x \) -print0 2>/dev/null \
    | xargs -0 --no-run-if-empty objdump -T 2>/dev/null \
    | grep -oE 'GLIBC_[0-9]+\.[0-9]+' \
    | sort -u -t_ -k2 -V \
    | tail -1
}

FLOOR="$(glibc_floor || true)"

log "done"
ls -lh "${BUILD_DIR}/${OUTPUT}"

if [[ -n "$FLOOR" ]]; then
  printf '\nglibc required: %s (this machine has %s)\n' \
    "${FLOOR#GLIBC_}" "$(ldd --version | head -1 | grep -oE '[0-9]+\.[0-9]+$')"
  printf 'The AppImage runs on distributions with that glibc or newer, and on no\n'
  printf 'older one. Ubuntu 24.04 has 2.39, Debian 12 has 2.36, Ubuntu 22.04 has 2.35.\n'
fi
