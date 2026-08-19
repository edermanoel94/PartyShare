#!/usr/bin/env bash
#
# Makes the runner's vcpkg able to resolve the baseline this project pins.
#
# The runners ship vcpkg as a shallow clone. A shallow clone has the tip and
# nothing behind it, so vcpkg's own lookup of the pinned baseline fails with a
# message that reads like the file is missing rather than the history:
#
#   fatal: path 'versions/baseline.json' exists on disk, but not in '<sha>'
#
# The file is there. The commit is not, or rather its tree is not, and git says
# the same thing either way.
#
# Fetching the one commit is enough and costs almost nothing: it brings the tree
# behind it, which is all vcpkg reads. Unshallowing the whole repository is the
# fallback for a server that refuses to serve a commit by hash.
#
# Run from the repository root, on every job that configures with vcpkg.

set -euo pipefail

root="${VCPKG_INSTALLATION_ROOT:-}"
if [[ -z "$root" ]]; then
  echo "VCPKG_INSTALLATION_ROOT is not set, so there is no vcpkg to prepare" >&2
  exit 1
fi

# Read without a JSON parser, because this runs on the Windows runner too and
# the only thing guaranteed there is the shell this script is already using.
baseline="$(sed -n 's/.*"builtin-baseline"[[:space:]]*:[[:space:]]*"\([0-9a-f]*\)".*/\1/p' vcpkg.json)"
if [[ -z "$baseline" ]]; then
  echo "no builtin-baseline in vcpkg.json, so there is nothing to fetch" >&2
  exit 0
fi

if git -C "$root" cat-file -e "${baseline}^{tree}" 2>/dev/null; then
  echo "vcpkg already has the baseline ${baseline}"
  exit 0
fi

echo "fetching the baseline ${baseline} into ${root}"
if ! git -C "$root" fetch --depth 1 origin "$baseline" 2>/dev/null; then
  echo "fetching it by hash was refused, unshallowing instead"
  git -C "$root" fetch --unshallow || git -C "$root" fetch
fi

git -C "$root" cat-file -e "${baseline}^{tree}"
echo "vcpkg can resolve the baseline"
