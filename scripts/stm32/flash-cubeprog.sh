#!/usr/bin/env bash
set -euo pipefail

image="${1:?usage: flash-cubeprog.sh <image> [address] [port]}"
address="${2:-0x08000000}"
port="${3:-SWD}"
cubeprog="${STM32_CUBEPROG_CLI:-STM32_Programmer_CLI}"

if ! command -v "$cubeprog" >/dev/null 2>&1; then
  echo "STM32_Programmer_CLI not found in PATH."
  echo "Set STM32_CUBEPROG_CLI or install STM32CubeProgrammer."
  exit 1
fi

if [[ ! -f "$image" ]]; then
  echo "Image not found: $image"
  exit 1
fi

case "$image" in
  *.bin)
    exec "$cubeprog" -c "port=$port" -d "$image" "$address" -v -rst
    ;;
  *)
    exec "$cubeprog" -c "port=$port" -d "$image" -v -rst
    ;;
esac
