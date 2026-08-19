#!/usr/bin/env bash
#
# Puts a vcpkg at the commit this project pins, and exports the toolchain file
# that points at it.
#
# The runners ship their own vcpkg, and using it does not work. Its ports tree
# is at whatever commit the runner image was built from, while `builtin-baseline`
# in vcpkg.json pins the version database to an older one, and vcpkg needs the
# two to agree:
#
#   error: no version database entry for gtest at 1.18.0
#
# The ports offered 1.18.0 and the pinned database had never heard of it. There
# is no fixing that from the outside: either the pin follows whatever the runner
# happens to have, which is the opposite of pinning, or the checkout follows the
# pin. This does the second.
#
# The cost is a clone and a bootstrap, and the clone is a single commit rather
# than the history behind it. What it buys is a build that resolves the same
# dependency versions on a runner today and on a runner in a year, which is the
# only reason to write a baseline down.
#
# Exports DV_VCPKG_TOOLCHAIN through GITHUB_ENV, and prints it when run outside
# Actions.

set -euo pipefail

VCPKG_DIR="${VCPKG_DIR:-$PWD/.vcpkg}"

# Read without a JSON parser: this runs on the Windows runner too, and the only
# thing guaranteed there is the shell it is already using.
baseline="$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]*\)".*/\1/p' vcpkg.json)"
if [[ -z "$baseline" ]]; then
  echo "no builtin-baseline in vcpkg.json, so there is nothing to pin to" >&2
  exit 1
fi

if [[ ! -d "$VCPKG_DIR/.git" ]]; then
  echo "cloning vcpkg at ${baseline}"
  mkdir -p "$VCPKG_DIR"
  git -C "$VCPKG_DIR" init --quiet
  git -C "$VCPKG_DIR" remote add origin https://github.com/microsoft/vcpkg.git
fi

current="$(git -C "$VCPKG_DIR" rev-parse HEAD 2>/dev/null || echo none)"
if [[ "$current" != "$baseline" ]]; then
  # One commit, without the history behind it: the ports and the version
  # database are both in the tree, and nothing here ever looks further back.
  git -C "$VCPKG_DIR" fetch --depth 1 origin "$baseline"
  git -C "$VCPKG_DIR" checkout --quiet FETCH_HEAD
fi

case "${RUNNER_OS:-$(uname -s)}" in
  Windows|MINGW*|MSYS*|CYGWIN*)
    bootstrap="$VCPKG_DIR/bootstrap-vcpkg.bat"
    executable="$VCPKG_DIR/vcpkg.exe"
    ;;
  *)
    bootstrap="$VCPKG_DIR/bootstrap-vcpkg.sh"
    executable="$VCPKG_DIR/vcpkg"
    ;;
esac

if [[ ! -x "$executable" ]]; then
  echo "bootstrapping vcpkg"
  # -disableMetrics because a build machine phoning home is a dependency on a
  # service being up, in a step whose only job is to produce a compiler.
  "$bootstrap" -disableMetrics
fi

toolchain="$VCPKG_DIR/scripts/buildsystems/vcpkg.cmake"
if [[ ! -f "$toolchain" ]]; then
  echo "vcpkg was checked out but has no toolchain file at ${toolchain}" >&2
  exit 1
fi

echo "vcpkg ${baseline} ready"
if [[ -n "${GITHUB_ENV:-}" ]]; then
  echo "DV_VCPKG_TOOLCHAIN=${toolchain}" >> "$GITHUB_ENV"
else
  echo "DV_VCPKG_TOOLCHAIN=${toolchain}"
fi
