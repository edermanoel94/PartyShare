#!/usr/bin/env bash
#
# Damages the network on this machine, so that a call can be measured on a bad
# link instead of on a perfect one.
#
# This is the wire level half of task 2 of M8. The other half is in the client
# itself, described in client/src/media/network_impairment.hpp: it needs no
# privilege and runs on every platform, and the media tests use it. This script
# is the one that impairs the operating system's own queues, both directions at
# once and for every process on the machine, which is what a real bad network
# does and what a fault injector inside one process cannot reproduce.
#
# Usage:
#
#   sudo scripts/netem.sh apply lossy       # 5% loss, the number in SPEC.md
#   sudo scripts/netem.sh apply distant     # 150 ms of latency and jitter
#   sudo scripts/netem.sh apply awful       # both, plus reordering
#   sudo scripts/netem.sh apply custom --loss 2% --delay 80ms --jitter 20ms
#   sudo scripts/netem.sh status
#   sudo scripts/netem.sh clear
#
# The interface defaults to loopback, which is where a test that runs the
# client and the server on one machine puts its traffic. Pass --interface to
# impair a real one:
#
#   sudo scripts/netem.sh apply lossy --interface wlp4s0
#
# Two things are worth knowing before reading the numbers this produces.
#
# First, netem on loopback impairs the packet once, not twice: there is no
# separate path for the two directions. On a real interface the qdisc is
# egress only, so impairing both ends of a call means running this on both
# machines.
#
# Second, on loopback it impairs everything, including the WebSocket that
# carries signaling. That is realistic - a bad network is bad for every
# connection over it - but it means a call that fails to be set up under heavy
# loss failed for a reason that has nothing to do with media.

set -euo pipefail

readonly kInterfaceDefault="lo"

die() {
  echo "netem: $*" >&2
  exit 1
}

usage() {
  sed -n '3,40p' "$0" | sed 's/^# \{0,1\}//'
  exit "${1:-0}"
}

require_root() {
  [[ "$(id -u)" -eq 0 ]] || die "this needs root: run it under sudo"
}

# netem is a kernel module, and on a machine whose kernel was upgraded without
# a reboot the modules of the running kernel are gone. That produces "Specified
# qdisc kind is unknown", which reads like a missing package and is not one.
require_netem() {
  command -v tc >/dev/null 2>&1 || die "tc is not installed (package iproute2)"
  if ! modprobe sch_netem 2>/dev/null && [[ ! -d "/lib/modules/$(uname -r)" ]]; then
    die "the running kernel $(uname -r) has no modules installed, which usually means
       the kernel was upgraded and the machine has not been rebooted yet. netem
       cannot be loaded until it is. The impairment built into the client, in
       client/src/media/network_impairment.hpp, needs neither root nor a reboot
       and is what tests/integration/test_network_impairment.cpp uses."
  fi
}

interface="${kInterfaceDefault}"
loss=""
delay=""
jitter=""
reorder=""
duplicate=""
corrupt=""

parse_options() {
  while [[ $# -gt 0 ]]; do
    case "$1" in
      --interface) interface="${2:?--interface needs a value}"; shift 2 ;;
      --loss) loss="${2:?--loss needs a value}"; shift 2 ;;
      --delay) delay="${2:?--delay needs a value}"; shift 2 ;;
      --jitter) jitter="${2:?--jitter needs a value}"; shift 2 ;;
      --reorder) reorder="${2:?--reorder needs a value}"; shift 2 ;;
      --duplicate) duplicate="${2:?--duplicate needs a value}"; shift 2 ;;
      --corrupt) corrupt="${2:?--corrupt needs a value}"; shift 2 ;;
      -h|--help) usage 0 ;;
      *) die "unknown option $1" ;;
    esac
  done
}

# The profiles. Named rather than numeric, because "lossy" is what a report
# says and 5% is what it means.
select_profile() {
  case "$1" in
    lossy)
      # Section 22 of SPEC.md: the call survives 5% packet loss.
      loss="${loss:-5%}"
      ;;
    distant)
      # A call across a continent: 150 ms each way is the round trip SPEC.md
      # sets as the target, and no link delivers it without variation.
      delay="${delay:-150ms}"
      jitter="${jitter:-30ms}"
      ;;
    awful)
      # A congested hotel network. Everything at once, which is the case where
      # a call either degrades or falls over.
      loss="${loss:-10%}"
      delay="${delay:-200ms}"
      jitter="${jitter:-60ms}"
      reorder="${reorder:-5%}"
      ;;
    custom)
      [[ -n "${loss}${delay}${jitter}${reorder}${duplicate}${corrupt}" ]] ||
        die "custom needs at least one of --loss, --delay, --jitter, --reorder, --duplicate or --corrupt"
      ;;
    *)
      die "unknown profile $1. Try lossy, distant, awful or custom"
      ;;
  esac
}

build_arguments() {
  local -a arguments=()
  if [[ -n "${delay}" ]]; then
    arguments+=(delay "${delay}")
    if [[ -n "${jitter}" ]]; then
      # Jitter with a normal distribution rather than the uniform default,
      # because on a real link most packets arrive near the mean and a few
      # arrive far from it.
      arguments+=("${jitter}" distribution normal)
    fi
  fi
  if [[ -n "${reorder}" ]]; then
    arguments+=(reorder "${reorder}")
  fi
  if [[ -n "${loss}" ]]; then
    arguments+=(loss "${loss}")
  fi
  if [[ -n "${duplicate}" ]]; then
    arguments+=(duplicate "${duplicate}")
  fi
  if [[ -n "${corrupt}" ]]; then
    arguments+=(corrupt "${corrupt}")
  fi
  if [[ ${#arguments[@]} -eq 0 ]]; then
    return 0
  fi
  printf '%s\n' "${arguments[@]}"
}

apply() {
  # Checked here rather than while building the command line, because that
  # runs in a subshell where dying would only be half heard.
  if [[ -n "${jitter}" && -z "${delay}" ]]; then
    die "--jitter is a spread around --delay, so it needs one"
  fi
  # Reordering only means anything when something is being held back, and
  # netem refuses it otherwise.
  if [[ -n "${reorder}" && -z "${delay}" ]]; then
    die "--reorder needs a --delay to reorder against"
  fi

  local -a arguments=()
  mapfile -t arguments < <(build_arguments)
  [[ ${#arguments[@]} -gt 0 ]] || die "nothing to apply"

  # Dry run prints the command instead of running it, which is how the profiles
  # can be checked on a machine that cannot load netem at all.
  if [[ -n "${DV_NETEM_DRY_RUN:-}" ]]; then
    echo "tc qdisc replace dev ${interface} root netem ${arguments[*]}"
    return 0
  fi

  require_root
  require_netem

  # Replaced rather than added, so that applying twice is not two impairments
  # stacked on top of each other.
  tc qdisc replace dev "${interface}" root netem "${arguments[@]}"
  echo "netem: ${interface} is now ${arguments[*]}"
  echo "netem: clear it with 'sudo $0 clear --interface ${interface}'"
}

clear_impairment() {
  require_root
  command -v tc >/dev/null 2>&1 || die "tc is not installed (package iproute2)"
  # Nothing to remove is a success: this is what a test harness runs in its
  # cleanup, and it must not fail a run that never applied anything.
  tc qdisc del dev "${interface}" root 2>/dev/null || true
  echo "netem: ${interface} is back to normal"
}

status() {
  command -v tc >/dev/null 2>&1 || die "tc is not installed (package iproute2)"
  tc -s qdisc show dev "${interface}"
}

main() {
  [[ $# -gt 0 ]] || usage 1
  local command="$1"
  shift

  case "${command}" in
    apply)
      [[ $# -gt 0 ]] || die "apply needs a profile: lossy, distant, awful or custom"
      local profile="$1"
      shift
      parse_options "$@"
      select_profile "${profile}"
      apply
      ;;
    clear)
      parse_options "$@"
      clear_impairment
      ;;
    status)
      parse_options "$@"
      status
      ;;
    -h|--help|help) usage 0 ;;
    *) die "unknown command ${command}. Try apply, clear or status" ;;
  esac
}

main "$@"
