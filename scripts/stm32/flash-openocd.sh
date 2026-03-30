#!/usr/bin/env bash
set -euo pipefail

interface_cfg="${1:?usage: flash-openocd.sh <interface_cfg> <target_cfg> <image> [address]}"
target_cfg="${2:?usage: flash-openocd.sh <interface_cfg> <target_cfg> <image> [address]}"
image="${3:?usage: flash-openocd.sh <interface_cfg> <target_cfg> <image> [address]}"
address="${4:-0x08000000}"
openocd_bin="${OPENOCD:-openocd}"

if ! command -v "$openocd_bin" >/dev/null 2>&1; then
  echo "openocd not found in PATH."
  echo "Set OPENOCD or install openocd."
  exit 1
fi

if [[ ! -f "$image" ]]; then
  echo "Image not found: $image"
  exit 1
fi

if [[ "$image" == *.elf || "$image" == *.axf ]]; then
  program_cmd="program $image verify reset exit"
else
  program_cmd="program $image $address verify reset exit"
fi

exec "$openocd_bin" \
  -f "$interface_cfg" \
  -f "$target_cfg" \
  -c "init" \
  -c "reset init" \
  -c "$program_cmd"
