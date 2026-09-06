#!/usr/bin/env bash
#
# Builds the macOS artefact: the disk image a person mounts, drags the
# application out of, and throws away.
#
# The obvious way to do this is one line, `hdiutil create -srcfolder`, and that
# is what the release workflow ran until v0.1.58 failed on it with
#
#   hdiutil: create failed - Resource busy
#
# after eleven releases that passed. The one line is not one operation. It
# creates a read/write image, mounts it, copies the folder in, unmounts it, and
# converts the result to a compressed image, and it reports any failure among
# the five as a single sentence with no indication of which one. The failure was
# the unmount: the job's cleanup had to terminate an orphaned diskimages-helper,
# which is the process that backs a mounted image, so the volume was still
# attached when hdiutil gave up. Something on the runner, almost certainly the
# Spotlight indexer noticing a newly mounted volume, held it.
#
# So the five steps are spelled out here instead. Each one can be seen when it
# fails, the volume is mounted where nothing goes looking for it, indexing is
# turned off on it, and the unmount is allowed to be told twice.
#
# Usage:
#
#   scripts/macos_dmg.sh                        # stage/ -> ./partyshare-<v>-macos-<arch>.dmg
#   scripts/macos_dmg.sh --stage-dir build/stage
#   scripts/macos_dmg.sh --out-dir dist --arch arm64
#
# The stage directory is what `cmake --install` produced and macdeployqt then
# made self-contained: partyshare.app beside share/doc/partyshare/config.ini.

set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STAGE_DIR="${REPO_ROOT}/stage"
OUT_DIR="$REPO_ROOT"
VOLNAME="PartyShare"
ARCH=""

while [[ $# -gt 0 ]]; do
  case "$1" in
    --stage-dir) STAGE_DIR="$2"; shift 2 ;;
    --out-dir)   OUT_DIR="$2";   shift 2 ;;
    --arch)      ARCH="$2";      shift 2 ;;
    -h|--help)
      sed -n '2,28p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

log() { printf '\n=== %s\n' "$*"; }

if [[ "$(uname -s)" != "Darwin" ]]; then
  echo "this builds a macOS disk image and only runs on macOS" >&2
  exit 1
fi

APP="${STAGE_DIR}/partyshare.app"
if [[ ! -d "$APP" ]]; then
  echo "no ${APP}. Run cmake --install into ${STAGE_DIR} first." >&2
  exit 1
fi

VERSION="$(sed -n 's/^  VERSION \([0-9.]*\)$/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -1)"
if [[ -z "$VERSION" ]]; then
  echo "could not read the version out of CMakeLists.txt" >&2
  exit 1
fi

if [[ -z "$ARCH" ]]; then
  case "$(uname -m)" in
    arm64)  ARCH="arm64" ;;
    x86_64) ARCH="x64" ;;
    *)      ARCH="$(uname -m)" ;;
  esac
fi

WORK="${REPO_ROOT}/build/dmg"
SRC="${WORK}/src"
MNT="${WORK}/mnt"
RW="${WORK}/rw.dmg"
DMG="${OUT_DIR}/partyshare-${VERSION}-macos-${ARCH}.dmg"

# -----------------------------------------------------------------------------
# Unmounting is the step that failed in the field, so it is the step with a
# policy rather than a command. What it is handed is the device node hdiutil
# attach reported; what it has to end with is that device not attached, because
# hdiutil convert reads the image file underneath it and a still-mounted image
# converts to a corrupt one.
#
# The three decisions in here, and what they are answering:
#
# Backoff rather than repetition. Whoever holds the volume is a scanner
# finishing a pass, and it lets go on its own in seconds. Three immediate
# retries would all land inside the same busy window and fail together, which
# is the same as not retrying at all. Four tries at 3, 6, 9 and 12 seconds give
# it half a minute to finish, which costs nothing on the run where the first
# try works.
#
# -force last, not first. Reaching for it immediately would paper over the day
# this script leaves a file open itself, and that bug would then ship as a
# corrupt image instead of a red job. Reaching for it never is how v0.1.58
# happens again. So it comes after the backoff has been spent: nothing is being
# written by then, the copy finished before this was called, and a holder that
# survived thirty seconds is not going to release on its own.
#
# A failure here is fatal. Returning zero would let the convert below run
# against a mounted image and publish a .dmg that nobody finds out is broken
# until somebody downloads it. A red job is the better of the two outcomes.
# -----------------------------------------------------------------------------
detach_volume() {
  local dev="$1"
  local attempt

  for attempt in 1 2 3 4; do
    if hdiutil detach "$dev" >/dev/null 2>&1; then
      echo "detached ${dev} on attempt ${attempt}"
      return 0
    fi
    # Printed rather than swallowed: the next person to hit this should not
    # have to infer the holder from an orphaned process in the cleanup log,
    # which is what diagnosing v0.1.58 came down to.
    echo "detach of ${dev} refused; holders:"
    lsof +D "$MNT" 2>/dev/null | head -20 || echo "  (lsof reports none)"
    sleep $(( attempt * 3 ))
  done

  echo "forcing the detach of ${dev} after 30s of waiting" >&2
  if hdiutil detach "$dev" -force; then
    return 0
  fi

  echo "could not detach ${dev}; refusing to convert a mounted image" >&2
  return 1
}

log "packaging ${DMG##*/}"

rm -rf "$WORK"
mkdir -p "$SRC" "$MNT" "$OUT_DIR"

# What a person sees when the image mounts: the application, and the
# Applications folder to drag it into. The symlink is the whole convention on
# macOS, and an image without it is one where the only thing to do is run the
# app from the disk image, which leaves it running from a mount that disappears.
cp -R "$APP" "$SRC/"
ln -s /Applications "$SRC/Applications"

# Beside the application, where it is read before anything is installed. The
# copy inside Contents/Resources is the one that survives the drag; this one is
# the one that gets noticed.
cp "${STAGE_DIR}/share/doc/partyshare/config.ini" "$SRC/"

cp "${REPO_ROOT}/assets/partyshare.icns" "$SRC/.VolumeIcon.icns"

# Sized from the content with room on top. HFS+ wants slack to write into, and
# slack is free: the UDZO conversion at the end compresses the empty space away,
# so the published file is the size of what is in it either way.
SIZE_MB=$(( $(du -sk "$SRC" | cut -f1) / 1024 + 100 ))

log "creating a ${SIZE_MB} MB read/write image"
# -type UDIF and no -format: a blank image is read/write by nature, and
# -format is the flag that converts an existing source, so passing it here is
# rejected outright with "-format requires -srcfolder or -srcdevice". The
# compression happens at the end, on an image that is finished.
hdiutil create -size "${SIZE_MB}m" -fs HFS+ -volname "$VOLNAME" \
  -type UDIF -ov "$RW"

# -mountpoint, so the volume lands under build/ instead of /Volumes/PartyShare.
# A fixed path in /Volumes is a name two jobs can collide on and a leftover
# mount can occupy; a path under the build directory is neither. -nobrowse keeps
# it out of the Finder sidebar, -noautoopen keeps a window from opening onto it,
# and -noverify skips a checksum of an image written seconds ago.
log "attaching"
ATTACHED="$(hdiutil attach "$RW" -mountpoint "$MNT" \
  -nobrowse -noautoopen -noverify)"
DEV="$(printf '%s\n' "$ATTACHED" | grep -Eo '^/dev/disk[0-9]+' | head -1)"
if [[ -z "$DEV" ]]; then
  echo "hdiutil attach reported no device:" >&2
  printf '%s\n' "$ATTACHED" >&2
  exit 1
fi
echo "attached as ${DEV} at ${MNT}"

# The prime suspect for the Resource busy that broke v0.1.58. mdworker starts
# indexing a volume the moment it mounts and holds it while it does; turning
# indexing off on this volume means there is nothing to wait for at the end.
# Best effort: a machine where mdutil is unavailable or refuses still produces
# a correct image, it just has to wait out the indexer in detach_volume.
mdutil -i off "$MNT" >/dev/null 2>&1 || true

log "copying the bundle in"
ditto "$SRC" "$MNT"

# The icon the mounted volume gets in the Finder sidebar. It needs the file, the
# custom icon attribute, and the directory marked as having one: any of the
# three missing and it falls back to the generic disk. Set on the volume root
# rather than on the source folder, because the attribute lives in the
# filesystem being shipped and ditto does not carry it across.
SetFile -a C "$MNT" || true

log "detaching"
detach_volume "$DEV"

log "compressing"
rm -f "$DMG"
hdiutil convert "$RW" -format UDZO -o "$DMG"
rm -f "$RW"

log "done"
ls -lh "$DMG"
