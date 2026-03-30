#!/usr/bin/env bash
set -euo pipefail

workspace_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build_dir="${1:-$workspace_dir/build}"

if [[ -f "$workspace_dir/CMakeLists.txt" ]]; then
  cmake -S "$workspace_dir" -B "$build_dir" -G Ninja
  cmake --build "$build_dir"
  exit 0
fi

if [[ -f "$workspace_dir/Makefile" ]]; then
  make -C "$workspace_dir" -j"$(nproc)"
  exit 0
fi

cat <<'EOF'
No CMakeLists.txt or Makefile found.

Generate the project from STM32CubeMX first:
  1. Project Manager -> Toolchain / IDE
  2. Choose CMake or Makefile
  3. Generate code into this workspace
EOF
exit 1
