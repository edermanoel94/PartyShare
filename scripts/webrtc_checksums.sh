#!/usr/bin/env bash
# Prints the SHA-256 of every libwebrtc asset of a release, in the format used
# by cmake/Findlibwebrtc.cmake.
#
# GitHub publishes the digests through its API, so nothing has to be downloaded.
#
# Usage: scripts/webrtc_checksums.sh [tag]
set -euo pipefail

repo="shiguredo-webrtc-build/webrtc-build"
tag="${1:-}"

if [[ -n "${tag}" ]]; then
  url="https://api.github.com/repos/${repo}/releases/tags/${tag}"
else
  url="https://api.github.com/repos/${repo}/releases/latest"
fi

curl -sfL "${url}" | python3 -c '
import json
import sys

release = json.load(sys.stdin)
wanted = ("macos_arm64", "ubuntu-24.04_x86_64", "ubuntu-22.04_x86_64", "windows_x86_64")

print("# release: " + release["tag_name"])
for asset in release["assets"]:
    name = asset["name"]
    if not any(key in name for key in wanted):
        continue
    digest = (asset.get("digest") or "").removeprefix("sha256:")
    if not digest:
        print("# " + name + ": no digest published, download and hash it manually")
        continue
    print(name)
    print("    " + digest)
'
