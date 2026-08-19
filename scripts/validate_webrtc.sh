#!/usr/bin/env bash
# Runs the M3 spike on this machine and prints a report.
#
# Purpose: confirm that libwebrtc links and works on a platform the development
# environment cannot reach. See docs/webrtc-validation.md.
#
# Usage:
#   scripts/validate_webrtc.sh                 # prebuilt binaries, quick
#   scripts/validate_webrtc.sh --root DIR      # a tree from build_webrtc.sh
#
# Run it from a normal graphical desktop session, not over SSH and not from a
# text console. Screen capture cannot be validated without a display server
# attached, and validating it is half the point.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/spike"
WEBRTC_ROOT=""
COMPILER=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --root) WEBRTC_ROOT="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    -h|--help) sed -n '2,16p' "$0"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

echo "=============================================="
echo " libwebrtc validation"
echo "=============================================="
echo "host        : $(uname -s) $(uname -m)"
echo "repository  : ${REPO_ROOT}"

if [[ "$(uname -s)" == "Linux" ]]; then
  echo "session     : ${XDG_SESSION_TYPE:-unknown}"
  if [[ -z "${DISPLAY:-}" && -z "${WAYLAND_DISPLAY:-}" ]]; then
    echo
    echo "WARNING: no display server is attached."
    echo "         Screen capture will be reported as skipped, not validated."
    echo "         Run this from a graphical session to cover it."
  fi
  # The published Linux binaries require Chromium's libc++, which only clang
  # can consume. A tree built by build_webrtc.sh has no such requirement.
  if [[ -z "${WEBRTC_ROOT}" ]] && command -v clang++ >/dev/null; then
    COMPILER="-DCMAKE_CXX_COMPILER=clang++"
    echo "compiler    : clang++ (required by the prebuilt binaries)"
  fi
fi

CMAKE_ARGS=(
  -S "${REPO_ROOT}"
  -B "${BUILD_DIR}"
  -DCMAKE_BUILD_TYPE=Release
  -DDV_ENABLE_WEBRTC_SPIKE=ON
  -DDV_BUILD_CLIENT=OFF
  -DDV_BUILD_SERVER=OFF
  -DDV_BUILD_TESTS=OFF
)
[[ -n "${WEBRTC_ROOT}" ]] && CMAKE_ARGS+=("-DDV_WEBRTC_ROOT=${WEBRTC_ROOT}")
[[ -n "${COMPILER}" ]] && CMAKE_ARGS+=("${COMPILER}")

echo
echo "==> configuring (the first run downloads libwebrtc, 110 MB to 740 MB)"
cmake "${CMAKE_ARGS[@]}"

echo
echo "==> building"
cmake --build "${BUILD_DIR}" --parallel

BINARY="${BUILD_DIR}/bin/webrtc-spike"
[[ -x "${BINARY}" ]] || BINARY="${BUILD_DIR}/bin/Release/webrtc-spike"

echo
echo "==> running"
echo
if "${BINARY}"; then
  echo
  echo "RESULT: the spike passed on $(uname -s) $(uname -m)."
else
  echo
  echo "RESULT: the spike FAILED on $(uname -s) $(uname -m)."
  echo "Send the full output above back, it is exactly what M3 needs."
  exit 1
fi
