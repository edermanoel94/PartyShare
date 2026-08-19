#!/usr/bin/env bash
#
# Sets up a virtual sound card so the audio pipeline can be tested on a machine
# that has no hardware, which is what a CI runner is.
#
# It creates two PulseAudio null sinks. One stands in for the speakers. The
# other carries the microphone signal: a tone is played into it, so that what
# the pipeline captures is a real signal rather than silence.
#
# The capture device itself is a remap source on top of that second sink's
# monitor. A monitor cannot be used directly, because libwebrtc's PulseAudio
# backend skips every source that monitors a sink when it enumerates capture
# devices, and a device it will not list is a device it will not open.
#
# Usage:
#
#   eval "$(scripts/virtual_audio.sh start)"   # prints the environment to use
#   ctest --test-dir build/media -L media
#   scripts/virtual_audio.sh stop
#
# On a machine that already has a sound server the script attaches to it and
# leaves the default devices alone, so running the tests never takes over the
# speakers of whoever is at the keyboard. The device names are exported instead,
# and the tests select them explicitly. With no sound server reachable it starts
# a private one and, there being nothing to disturb, makes the virtual devices
# the defaults as well.

set -euo pipefail

readonly kSpeaker="dv_virtual_speaker"
readonly kMicrophoneSink="dv_virtual_microphone_sink"
readonly kMicrophone="dv_virtual_microphone"
readonly kStateFile="${DV_VIRTUAL_AUDIO_STATE:-${XDG_RUNTIME_DIR:-/tmp}/dv-virtual-audio.state}"
readonly kRuntimeDir="${DV_VIRTUAL_AUDIO_RUNTIME:-${TMPDIR:-/tmp}/dv-virtual-audio}"

die() {
  echo "virtual_audio: $*" >&2
  exit 1
}

require() {
  command -v "$1" >/dev/null 2>&1 || die "$1 is not installed"
}

# A second of a 440 Hz sine at 48 kHz mono, which is what section 9 of SPEC.md
# says the pipeline runs at. Written with python so the script needs no sox.
write_tone() {
  local path="$1"
  python3 - "$path" <<'PYTHON'
import math
import struct
import sys
import wave

RATE = 48000
SECONDS = 1
FREQUENCY = 440.0
# Loud enough to be unambiguous, quiet enough not to clip once the automatic
# gain control has had its say.
AMPLITUDE = 12000

with wave.open(sys.argv[1], "wb") as out:
    out.setnchannels(1)
    out.setsampwidth(2)
    out.setframerate(RATE)
    frames = bytearray()
    for index in range(RATE * SECONDS):
        value = int(AMPLITUDE * math.sin(2 * math.pi * FREQUENCY * index / RATE))
        frames += struct.pack("<h", value)
    out.writeframes(bytes(frames))
PYTHON
}

start_private_server() {
  require pulseaudio
  mkdir -p "${kRuntimeDir}"
  # A private socket, so this server is invisible to anything else on the
  # machine and takes nothing over.
  pulseaudio \
    --start \
    --exit-idle-time=-1 \
    --disallow-exit \
    --load="module-native-protocol-unix socket=${kRuntimeDir}/native" \
    >/dev/null 2>&1
  export PULSE_SERVER="unix:${kRuntimeDir}/native"
  for _ in $(seq 1 50); do
    if pactl info >/dev/null 2>&1; then
      return 0
    fi
    sleep 0.1
  done
  die "the private sound server did not come up"
}

start() {
  require pactl
  require python3
  require paplay

  [[ -e "${kStateFile}" ]] && die "already started, run stop first (${kStateFile})"

  local started_server=0
  if ! pactl info >/dev/null 2>&1; then
    start_private_server
    started_server=1
  fi

  local speaker_module microphone_sink_module microphone_module
  speaker_module=$(pactl load-module module-null-sink \
    sink_name="${kSpeaker}" \
    sink_properties="device.description=${kSpeaker}")
  microphone_sink_module=$(pactl load-module module-null-sink \
    sink_name="${kMicrophoneSink}" \
    sink_properties="device.description=${kMicrophoneSink}")
  microphone_module=$(pactl load-module module-remap-source \
    source_name="${kMicrophone}" \
    master="${kMicrophoneSink}.monitor" \
    channels=1 \
    source_properties="device.description=${kMicrophone}")

  mkdir -p "${kRuntimeDir}"
  local tone="${kRuntimeDir}/tone.wav"
  write_tone "${tone}"

  # A tone on a loop, so the monitor always has something on it. Without this
  # the capture is digital silence and no level test means anything.
  ( while true; do paplay --device="${kMicrophoneSink}" "${tone}" || sleep 1; done ) \
    >/dev/null 2>&1 &
  local tone_pid=$!

  if [[ "${started_server}" == "1" ]]; then
    pactl set-default-sink "${kSpeaker}"
    pactl set-default-source "${kMicrophone}"
  fi

  mkdir -p "$(dirname "${kStateFile}")"
  {
    echo "speaker_module=${speaker_module}"
    echo "microphone_sink_module=${microphone_sink_module}"
    echo "microphone_module=${microphone_module}"
    echo "tone_pid=${tone_pid}"
    echo "started_server=${started_server}"
    echo "pulse_server=${PULSE_SERVER:-}"
  } > "${kStateFile}"

  # The device identifiers the client reports are the PulseAudio descriptions,
  # which is why the devices were given ones that can be predicted here.
  echo "export DV_VIRTUAL_INPUT_DEVICE='${kMicrophone}'"
  echo "export DV_VIRTUAL_OUTPUT_DEVICE='${kSpeaker}'"
  if [[ -n "${PULSE_SERVER:-}" ]]; then
    echo "export PULSE_SERVER='${PULSE_SERVER}'"
  fi
}

stop() {
  [[ -e "${kStateFile}" ]] || die "not started"

  local speaker_module="" microphone_sink_module="" microphone_module="" tone_pid=""
  local started_server="" pulse_server=""
  # shellcheck disable=SC1090
  source "${kStateFile}"
  [[ -n "${pulse_server}" ]] && export PULSE_SERVER="${pulse_server}"

  if [[ -n "${tone_pid}" ]]; then
    pkill -P "${tone_pid}" >/dev/null 2>&1 || true
    kill "${tone_pid}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${microphone_module}" ]]; then
    pactl unload-module "${microphone_module}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${microphone_sink_module}" ]]; then
    pactl unload-module "${microphone_sink_module}" >/dev/null 2>&1 || true
  fi
  if [[ -n "${speaker_module}" ]]; then
    pactl unload-module "${speaker_module}" >/dev/null 2>&1 || true
  fi
  if [[ "${started_server}" == "1" ]]; then
    pulseaudio --kill >/dev/null 2>&1 || true
    rm -rf "${kRuntimeDir}"
  fi

  rm -f "${kStateFile}"
}

case "${1:-}" in
  start) start ;;
  stop) stop ;;
  *) die "usage: $0 start|stop" ;;
esac
