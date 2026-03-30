#!/usr/bin/env bash
set -euo pipefail

image="${1:?usage: flash-jlink.sh <image> <device> [interface] [speed] [address]}"
device="${2:?usage: flash-jlink.sh <image> <device> [interface] [speed] [address]}"
interface="${3:-swd}"
speed="${4:-4000}"
address="${5:-0x08000000}"
jlink_bin="${JLINK_COMMANDER:-JLinkExe}"

if ! command -v "$jlink_bin" >/dev/null 2>&1; then
  echo "JLinkExe not found in PATH."
  echo "Set JLINK_COMMANDER or install SEGGER J-Link Software Pack."
  exit 1
fi

if [[ ! -f "$image" ]]; then
  echo "Image not found: $image"
  exit 1
fi

cmd_file="$(mktemp)"
trap 'rm -f "$cmd_file"' EXIT

if [[ "$image" == *.elf || "$image" == *.axf ]]; then
  load_cmd="loadfile $image"
else
  load_cmd="loadfile $image, $address"
fi

cat >"$cmd_file" <<EOF
exitonerror 1
device $device
if $interface
speed $speed
connect
r
h
$load_cmd
r
g
q
EOF

exec "$jlink_bin" -NoGui 1 -CommandFile "$cmd_file"
