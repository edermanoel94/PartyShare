#!/usr/bin/env bash
#
# Installs the PartyShare server on a Debian or Ubuntu machine as a systemd
# service, with MongoDB beside it, in one command:
#
#   git clone https://github.com/edermanoel94/PartyShare.git
#   cd PartyShare
#   sudo scripts/install_server.sh --admin=ana
#
# What it does, in order: installs MongoDB from its apt repository, obtains the
# server, installs it under /opt/partyshare, creates a partyshare system
# account, writes /etc/partyshare/server.ini, installs the unit from
# deploy/linux/partyshare.service, creates the first administrator, opens the
# two ports in ufw when ufw is active, and starts the service.
#
# The server comes from one of three places, tried in this order:
#
#   1. bin/partyshare-server beside this script, which is what an unpacked
#      release tarball looks like: the tarball alone is enough to install from.
#   2. The release tarball for this checkout's version, downloaded from GitHub
#      and checked against the release's SHA256SUMS.
#   3. A build from this checkout, when no release carries this version or
#      this machine's architecture. vcpkg compiles libdatachannel and the
#      MongoDB driver from source the first time, ten to thirty minutes.
#
# Usage:
#
#   sudo scripts/install_server.sh [options]
#
#   --admin=NAME              Create this administrator. The password is read
#                             from PARTYSHARE_ADMIN_PASSWORD, or asked for.
#   --port=PORT               Signaling port (default 8080).
#   --ice-port-range=A-B      UDP range the SFU binds media in
#                             (default 50000-50100, one port per participant).
#   --database-uri=URI        Use a MongoDB that already exists, here or
#                             elsewhere, and do not install one.
#   --mongo-version=X.Y       Which MongoDB to install (default 8.0).
#   --prefix=DIR              Where the server goes (default /opt/partyshare).
#   --build                   Build from this checkout even when a release
#                             carries the version.
#   --jobs=N                  Build parallelism (default: every core).
#   --uninstall               Stop the service and remove the server. The
#                             database, the configuration and the logs stay.
#
# PARTYSHARE_RELEASE_URL in the environment replaces the GitHub releases
# address, for a mirror.
#
# Running it again is how you upgrade: pull, run, and the service comes back
# on the new build with the configuration it had. server.ini is written only
# when it does not exist yet.
#
# Two things are worth knowing.
#
# First, building needs GCC 12 and CMake 3.25, which is Debian 12 or Ubuntu
# 24.04. Ubuntu 22.04 ships GCC 11 and is refused with a message rather than
# failing an hour later in the middle of the build. The downloaded server has
# no such need: it runs on anything with glibc 2.35, Ubuntu 22.04 included.
#
# Second, a build runs as whoever called sudo, not as root, so the checkout
# does not end up with root-owned build/ and .vcpkg/ directories. Only the
# install steps run as root.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
BUILD_DIR="${REPO_ROOT}/build/server"

PREFIX="/opt/partyshare"
CONFIG_DIR="/etc/partyshare"
CONFIG_FILE="${CONFIG_DIR}/server.ini"
STATE_DIR="/var/lib/partyshare"
LOG_DIR="/var/log/partyshare"
UNIT_FILE="/etc/systemd/system/partyshare.service"
SERVICE_USER="partyshare"
RELEASE_URL="${PARTYSHARE_RELEASE_URL:-https://github.com/edermanoel94/PartyShare/releases/download}"

PORT=8080
ICE_RANGE="50000-50100"
DATABASE_URI=""
DATABASE_NAME="partyshare"
MONGO_VERSION="8.0"
ADMIN=""
JOBS="$(nproc 2>/dev/null || echo 4)"
FORCE_BUILD=0
UNINSTALL=0

while [[ $# -gt 0 ]]; do
  case "$1" in
    --admin=*) ADMIN="${1#*=}"; shift ;;
    --port=*) PORT="${1#*=}"; shift ;;
    --ice-port-range=*) ICE_RANGE="${1#*=}"; shift ;;
    --database-uri=*) DATABASE_URI="${1#*=}"; shift ;;
    --mongo-version=*) MONGO_VERSION="${1#*=}"; shift ;;
    --prefix=*) PREFIX="${1#*=}"; shift ;;
    --build) FORCE_BUILD=1; shift ;;
    --jobs=*) JOBS="${1#*=}"; shift ;;
    --uninstall) UNINSTALL=1; shift ;;
    -h|--help)
      # The header above, up to the first line of code.
      sed -n '2,/^set -euo pipefail/{/^set -euo pipefail/!p}' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
      exit 0
      ;;
    *)
      echo "unknown argument: $1" >&2
      echo "try: $0 --help" >&2
      exit 2
      ;;
  esac
done

log() { printf '\n=== %s\n' "$*"; }

fail() {
  echo "error: $*" >&2
  exit 1
}

# Retried, for the same reason ci_vcpkg.sh retries: one reset packet should
# not cost a whole installation.
retry() {
  local description="$1"
  shift
  local attempt
  for attempt in 1 2 3; do
    if "$@"; then
      return 0
    fi
    echo "${description} failed on attempt ${attempt}"
    sleep $((attempt * 5))
  done
  fail "${description} failed three times, giving up"
}

# Runs a command as whoever called sudo, when somebody did. The build and the
# vcpkg checkout happen through this so that they belong to the person whose
# clone this is; a root-owned build/ inside somebody's working copy is the kind
# of thing that is discovered a week later, by a permission error.
as_builder() {
  if [[ -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]]; then
    runuser -u "${SUDO_USER}" -- "$@"
  else
    "$@"
  fi
}

[[ "$(id -u)" -eq 0 ]] || fail "run this with sudo: it installs packages and a service"

if [[ "${UNINSTALL}" -eq 1 ]]; then
  log "Removing the PartyShare service"
  if [[ -f "${UNIT_FILE}" ]]; then
    systemctl disable --now partyshare >/dev/null 2>&1 || true
    rm -f "${UNIT_FILE}"
    systemctl daemon-reload
  fi
  rm -rf "${PREFIX}"
  echo
  echo "The service and ${PREFIX} are gone. Kept, because they are yours:"
  echo
  printf '  %-22s %s\n' "${CONFIG_DIR}" "the configuration" \
    "${STATE_DIR}" "crash reports" \
    "${LOG_DIR}" "logs" \
    "MongoDB and its data," "and the ${SERVICE_USER} account"
  echo
  echo "Remove those by hand if you mean to."
  exit 0
fi

# --- what this machine is ----------------------------------------------------

[[ -r /etc/os-release ]] || fail "no /etc/os-release: this installer knows Debian and Ubuntu"
# shellcheck disable=SC1091
. /etc/os-release
DISTRO_ID="${ID:-}"
DISTRO_CODENAME="${VERSION_CODENAME:-}"
case "${DISTRO_ID}" in
  ubuntu|debian) ;;
  *) fail "this installer knows Debian and Ubuntu; this is '${DISTRO_ID}'. INSTALL.md has the manual steps." ;;
esac
command -v systemctl >/dev/null || fail "no systemctl: the service needs systemd"
[[ -d /run/systemd/system ]] || fail "systemd is not running this machine, so it cannot run the service"

case "$(uname -m)" in
  x86_64) ARCH="x64" ;;
  aarch64|arm64) ARCH="arm64" ;;
  *) ARCH="$(uname -m)" ;;
esac

case "${ICE_RANGE}" in
  [0-9]*-[0-9]*) ;;
  *) fail "--ice-port-range takes A-B, as in 50000-50100" ;;
esac
ICE_BEGIN="${ICE_RANGE%-*}"
ICE_END="${ICE_RANGE#*-}"
[[ "${ICE_BEGIN}" -lt "${ICE_END}" ]] || fail "--ice-port-range: ${ICE_BEGIN} is not below ${ICE_END}"

# --- packages ------------------------------------------------------------------

log "Installing what the installer itself needs"
export DEBIAN_FRONTEND=noninteractive
retry "apt-get update" apt-get update -qq
retry "installing curl and gnupg" apt-get install -y -qq curl ca-certificates gnupg tar gzip

# --- MongoDB -------------------------------------------------------------------

if [[ -n "${DATABASE_URI}" ]]; then
  log "Using the MongoDB at ${DATABASE_URI}"
else
  DATABASE_URI="mongodb://127.0.0.1:27017"
  if command -v mongod >/dev/null; then
    log "MongoDB is already installed: $(mongod --version | head -1)"
  else
    log "Installing MongoDB ${MONGO_VERSION} from repo.mongodb.org"
    # From MongoDB's own repository rather than the distribution's, which has
    # not carried MongoDB since the licence changed. The key goes into its own
    # keyring and the source names it, so nothing else on the machine trusts
    # that key.
    keyring="/usr/share/keyrings/mongodb-server-${MONGO_VERSION}.gpg"
    retry "fetching the MongoDB signing key" bash -c \
      "curl -fsSL 'https://pgp.mongodb.com/server-${MONGO_VERSION}.asc' | gpg --dearmor --yes -o '${keyring}'"
    case "${DISTRO_ID}" in
      ubuntu) component="multiverse" ;;
      debian) component="main" ;;
    esac
    echo "deb [ signed-by=${keyring} ] https://repo.mongodb.org/apt/${DISTRO_ID} ${DISTRO_CODENAME}/mongodb-org/${MONGO_VERSION} ${component}" \
      > "/etc/apt/sources.list.d/mongodb-org-${MONGO_VERSION}.list"
    retry "apt-get update" apt-get update -qq
    retry "installing mongodb-org" apt-get install -y -qq mongodb-org
  fi
  systemctl enable --now mongod >/dev/null
fi

# --- the server ------------------------------------------------------------------
#
# Each of the three sources leaves SERVER_BINARY pointing at an executable and
# UNIT_SOURCE at the unit to install, and the install below does not care
# which one it was.

SERVER_BINARY=""
UNIT_SOURCE="${REPO_ROOT}/deploy/linux/partyshare.service"
DOWNLOAD_DIR=""

use_local_binary() {
  [[ "${FORCE_BUILD}" -eq 0 && -x "${SCRIPT_DIR}/bin/partyshare-server" ]] || return 1
  log "Using the server unpacked beside this script"
  SERVER_BINARY="${SCRIPT_DIR}/bin/partyshare-server"
  [[ -f "${SCRIPT_DIR}/partyshare.service" ]] && UNIT_SOURCE="${SCRIPT_DIR}/partyshare.service"
  return 0
}

download_release() {
  [[ "${FORCE_BUILD}" -eq 0 ]] || return 1
  local version name url sums
  # Tolerant of a CRLF checkout: the file is what says which release to
  # fetch, and a stray carriage return would silently turn every install
  # into a build.
  version="$(sed -n 's/^  VERSION \([0-9.]*\)\r\{0,1\}$/\1/p' "${REPO_ROOT}/CMakeLists.txt" | head -1)"
  if [[ -z "${version}" ]]; then
    echo "no version in ${REPO_ROOT}/CMakeLists.txt, so there is no release to download"
    return 1
  fi
  name="partyshare-server-${version}-linux-${ARCH}"
  url="${RELEASE_URL}/v${version}/${name}.tar.gz"
  sums="${RELEASE_URL}/v${version}/SHA256SUMS"

  log "Downloading the ${version} server for linux-${ARCH}"
  DOWNLOAD_DIR="$(mktemp -d)"
  if ! curl -fsSL --retry 3 -o "${DOWNLOAD_DIR}/${name}.tar.gz" "${url}"; then
    echo "no release carries ${name}.tar.gz (${url})"
    rm -rf "${DOWNLOAD_DIR}"
    DOWNLOAD_DIR=""
    return 1
  fi
  # Against the checksum list the release publishes beside it. A tarball that
  # downloads but does not match is refused, not built around: the file is
  # wrong, and whatever made it wrong is not something to install from.
  retry "downloading SHA256SUMS" curl -fsSL -o "${DOWNLOAD_DIR}/SHA256SUMS" "${sums}"
  local expected actual
  expected="$(awk -v f="${name}.tar.gz" '$2 == f { print $1 }' "${DOWNLOAD_DIR}/SHA256SUMS")"
  [[ -n "${expected}" ]] || fail "SHA256SUMS of release v${version} does not list ${name}.tar.gz"
  actual="$(sha256sum "${DOWNLOAD_DIR}/${name}.tar.gz" | awk '{ print $1 }')"
  [[ "${expected}" == "${actual}" ]] || fail "${name}.tar.gz does not match SHA256SUMS: expected ${expected}, got ${actual}"
  echo "checksum verified"

  tar -xzf "${DOWNLOAD_DIR}/${name}.tar.gz" -C "${DOWNLOAD_DIR}"
  SERVER_BINARY="${DOWNLOAD_DIR}/${name}/bin/partyshare-server"
  [[ -x "${SERVER_BINARY}" ]] || fail "the tarball carries no bin/partyshare-server"
  return 0
}

build_from_source() {
  log "Building the server from this checkout (the long part the first time)"
  retry "installing build tools" apt-get install -y -qq \
    build-essential cmake ninja-build git zip unzip pkg-config autoconf automake libtool

  # Refused up front, not an hour in. vcpkg would build every dependency and
  # then the project itself would fail on a compiler that does not know C++20
  # well enough, or on a CMake that does not know the presets file.
  local gcc_major cmake_version
  gcc_major="$(gcc -dumpfullversion -dumpversion | cut -d. -f1)"
  [[ "${gcc_major}" -ge 12 ]] || fail "GCC ${gcc_major} is too old; building needs GCC 12, which is Debian 12 or Ubuntu 24.04"
  cmake_version="$(cmake --version | sed -n 's/^cmake version \([0-9.]*\).*/\1/p')"
  if [[ "$(printf '%s\n3.25\n' "${cmake_version}" | sort -V | head -1)" != "3.25" ]]; then
    fail "CMake ${cmake_version} is too old; building needs 3.25 (Debian 12 or Ubuntu 24.04, or Kitware's apt repository)"
  fi

  [[ -f "${REPO_ROOT}/vcpkg.json" ]] || fail "${REPO_ROOT} is not a PartyShare checkout, so there is nothing to build from"
  # The checkout has to be writable by the builder, which it is when it is
  # their clone. When root cloned it and somebody else is the builder, say so
  # rather than let vcpkg fail on a permission error deep in its output.
  if [[ -n "${SUDO_USER:-}" && "${SUDO_USER}" != "root" ]]; then
    as_builder test -w "${REPO_ROOT}" \
      || fail "${REPO_ROOT} is not writable by ${SUDO_USER}; clone the repository as that user or run this as root directly"
  fi

  as_builder bash -c "cd '${REPO_ROOT}' && VCPKG_DIR='${REPO_ROOT}/.vcpkg' scripts/ci_vcpkg.sh"
  local toolchain="${REPO_ROOT}/.vcpkg/scripts/buildsystems/vcpkg.cmake"

  # No preset, deliberately: every preset fixes its own binaryDir, and a
  # server with the client turned off has to sit beside a full tree, not
  # replace it. See INSTALL.md section 1.4.
  as_builder cmake -S "${REPO_ROOT}" -B "${BUILD_DIR}" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_TOOLCHAIN_FILE="${toolchain}" \
    -DDV_ENABLE_MONGO=ON -DVCPKG_MANIFEST_FEATURES=mongo \
    -DDV_BUILD_CLIENT=OFF -DDV_BUILD_TESTS=OFF
  as_builder cmake --build "${BUILD_DIR}" --target dv_server --parallel "${JOBS}"
  SERVER_BINARY="${BUILD_DIR}/bin/partyshare-server"
}

use_local_binary || download_release || build_from_source
[[ -f "${UNIT_SOURCE}" ]] || fail "no unit file at ${UNIT_SOURCE}"

# --- install -------------------------------------------------------------------

log "Installing under ${PREFIX}"
if ! id "${SERVICE_USER}" >/dev/null 2>&1; then
  useradd --system --home-dir "${STATE_DIR}" --shell /usr/sbin/nologin \
    --user-group "${SERVICE_USER}"
fi

# Stopped before the binary is replaced. Overwriting a running executable is
# refused with "text file busy", and the install would fail halfway.
if systemctl is-active --quiet partyshare; then
  systemctl stop partyshare
fi
install -D -m 0755 "${SERVER_BINARY}" "${PREFIX}/bin/partyshare-server"
if [[ -n "${DOWNLOAD_DIR}" ]]; then
  rm -rf "${DOWNLOAD_DIR}"
fi

install -d -m 0750 -o root -g "${SERVICE_USER}" "${CONFIG_DIR}"
install -d -m 0750 -o "${SERVICE_USER}" -g "${SERVICE_USER}" "${STATE_DIR}" "${STATE_DIR}/crashes" "${LOG_DIR}"

if [[ -f "${CONFIG_FILE}" ]]; then
  log "Keeping the existing ${CONFIG_FILE}"
else
  log "Writing ${CONFIG_FILE}"
  # 0640 and root:partyshare: the service can read it, and nobody else on the
  # machine can read a database URI that may carry a password.
  install -m 0640 -o root -g "${SERVICE_USER}" /dev/null "${CONFIG_FILE}"
  cat > "${CONFIG_FILE}" <<EOF
; PartyShare server. Written by scripts/install_server.sh; edit and then
; \`systemctl restart partyshare\`. Every key is described in
; docs/03-configuration.md.

[server]
bind_address = 0.0.0.0
port = ${PORT}
; The largest room anybody may create, 2 to 50. The SFU binds one UDP port and
; relays one audio stream per participant, so size it with the range below.
max_participants_per_room = 20

[database]
enabled = true
uri = ${DATABASE_URI}
name = ${DATABASE_NAME}

[network]
; The UDP range the SFU binds media in, one port per participant. The firewall
; has to allow it, as well as the TCP port above.
ice_port_range_begin = ${ICE_BEGIN}
ice_port_range_end = ${ICE_END}

[logging]
level = info
; The journal is the log: journalctl -u partyshare. Name a file here to have
; one as well; ${LOG_DIR} is where the service may write.
log_to_console = true
crash_directory = ${STATE_DIR}/crashes
EOF
fi

log "Installing the service"
# The unit names /opt/partyshare; a different prefix is written into the copy.
sed "s|/opt/partyshare|${PREFIX}|g" "${UNIT_SOURCE}" > "${UNIT_FILE}"
chmod 0644 "${UNIT_FILE}"
systemctl daemon-reload
systemctl enable partyshare >/dev/null

# --- the first administrator ------------------------------------------------------

if [[ -n "${ADMIN}" ]]; then
  password="${PARTYSHARE_ADMIN_PASSWORD:-}"
  if [[ -z "${password}" ]]; then
    [[ -t 0 ]] || fail "--admin needs a terminal to ask for the password, or PARTYSHARE_ADMIN_PASSWORD in the environment"
    read -rsp "Password for administrator '${ADMIN}': " password
    echo
    [[ -n "${password}" ]] || fail "the password cannot be empty"
  fi
  log "Creating administrator '${ADMIN}'"
  # As the service user, against the same configuration the service reads,
  # so the account lands in the database the service will open. The password
  # is visible in \`ps\` for the moment this runs; change it from the client
  # afterwards if that matters on this machine.
  runuser -u "${SERVICE_USER}" -- "${PREFIX}/bin/partyshare-server" \
    --config="${CONFIG_FILE}" --create-admin="${ADMIN}:${password}"
fi

# --- firewall -------------------------------------------------------------------

if command -v ufw >/dev/null && ufw status 2>/dev/null | grep -q "^Status: active"; then
  log "Opening the ports in ufw"
  ufw allow "${PORT}/tcp" >/dev/null
  ufw allow "${ICE_BEGIN}:${ICE_END}/udp" >/dev/null
fi

# --- start ------------------------------------------------------------------------

log "Starting the service"
systemctl restart partyshare
for _ in $(seq 1 30); do
  if ss -lnt 2>/dev/null | grep -q ":${PORT} "; then
    break
  fi
  sleep 1
done
if ! ss -lnt 2>/dev/null | grep -q ":${PORT} "; then
  echo
  journalctl -u partyshare -n 30 --no-pager || true
  fail "the service did not start listening on port ${PORT}; the log above says why"
fi

address="$(hostname -I 2>/dev/null | awk '{print $1}')"
cat <<EOF

PartyShare server is running.

  Signaling      ws://${address:-<this machine>}:${PORT}   (what clients put in Settings)
  Media          UDP ${ICE_BEGIN}-${ICE_END}
  Database       ${DATABASE_URI}, database "${DATABASE_NAME}"
  Configuration  ${CONFIG_FILE}
  Binary         ${PREFIX}/bin/partyshare-server

  Logs           journalctl -u partyshare -f
  Restart        sudo systemctl restart partyshare
  Upgrade        git pull && sudo scripts/install_server.sh

On a cloud machine, open TCP ${PORT} and UDP ${ICE_BEGIN}-${ICE_END} in the
security group as well; the host firewall alone does not. Signaling is plain
ws://, which is fine on a LAN and not on the internet: put it behind a TLS
proxy for wss://, see docs/04-server-and-database.md.
EOF
if [[ -z "${ADMIN}" ]]; then
  cat <<EOF

No administrator was created. To make one:

  sudo runuser -u ${SERVICE_USER} -- ${PREFIX}/bin/partyshare-server \\
    --config=${CONFIG_FILE} --create-admin=NAME:PASSWORD
EOF
fi
