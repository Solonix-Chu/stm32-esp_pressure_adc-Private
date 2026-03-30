#!/usr/bin/env bash
set -euo pipefail

target="${1:?usage: flash-pyocd.sh <target> <image>}"
image="${2:?usage: flash-pyocd.sh <target> <image>}"
pyocd_bin="${PYOCD:-pyocd}"

if ! command -v "$pyocd_bin" >/dev/null 2>&1; then
  echo "pyocd not found in PATH."
  echo "Set PYOCD or install pyocd."
  exit 1
fi

if [[ ! -f "$image" ]]; then
  echo "Image not found: $image"
  exit 1
fi

exec "$pyocd_bin" flash -t "$target" "$image"
