#!/usr/bin/env bash
set -euo pipefail

status() {
  local label="$1"
  local value="$2"
  printf '%-26s %s\n' "$label" "$value"
}

find_first_existing() {
  local candidate
  for candidate in "$@"; do
    if [[ -n "$candidate" && -e "$candidate" ]]; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

find_in_path_or_common_dirs() {
  local tool="$1"
  shift
  if command -v "$tool" >/dev/null 2>&1; then
    command -v "$tool"
    return 0
  fi
  find_first_existing "$@"
}

home_dir="${HOME:-}"
shopt -s nullglob

cube_mx_path="$(
  find_in_path_or_common_dirs \
    stm32cubemx \
    "$home_dir/STM32CubeMX/STM32CubeMX" \
    "$home_dir/STM32CubeMX"/*/STM32CubeMX \
    /usr/local/bin/stm32cubemx \
    /opt/st/stm32cubemx/STM32CubeMX \
    2>/dev/null || true
)"

cubeprog_path="$(
  find_in_path_or_common_dirs \
    STM32_Programmer_CLI \
    /usr/local/bin/STM32_Programmer_CLI \
    /usr/bin/STM32_Programmer_CLI \
    "$home_dir/STMicroelectronics/STM32Cube/STM32CubeProgrammer/bin/STM32_Programmer_CLI" \
    /opt/st/*/STM32CubeProgrammer/bin/STM32_Programmer_CLI \
    /opt/st/stm32cubeide*/plugins/*/tools/bin/STM32_Programmer_CLI \
    2>/dev/null || true
)"

jlink_server_path="$(
  find_in_path_or_common_dirs \
    JLinkGDBServerCLExe \
    /opt/SEGGER/JLink/JLinkGDBServerCLExe \
    /usr/local/bin/JLinkGDBServerCLExe \
    2>/dev/null || true
)"

openocd_path="$(
  find_in_path_or_common_dirs \
    openocd \
    /usr/bin/openocd \
    /usr/local/bin/openocd \
    2>/dev/null || true
)"

pyocd_path="$(
  find_in_path_or_common_dirs \
    pyocd \
    "$home_dir/.local/bin/pyocd" \
    /usr/local/bin/pyocd \
    /usr/bin/pyocd \
    2>/dev/null || true
)"

gdb_path="$(
  find_in_path_or_common_dirs \
    gdb-multiarch \
    /usr/bin/gdb-multiarch \
    "$home_dir/STMicroelectronics/STM32CubeCLT/GNU-tools-for-STM32/bin/arm-none-eabi-gdb" \
    /opt/st/*/GNU-tools-for-STM32/bin/arm-none-eabi-gdb \
    /usr/bin/arm-none-eabi-gdb \
    2>/dev/null || true
)"

gcc_path="$(
  find_in_path_or_common_dirs \
    arm-none-eabi-gcc \
    /usr/bin/arm-none-eabi-gcc \
    "$home_dir/STMicroelectronics/STM32CubeCLT/GNU-tools-for-STM32/bin/arm-none-eabi-gcc" \
    /opt/st/*/GNU-tools-for-STM32/bin/arm-none-eabi-gcc \
    2>/dev/null || true
)"

status "STM32CubeMX" "${cube_mx_path:-missing}"
status "arm-none-eabi-gcc" "${gcc_path:-missing}"
status "GDB" "${gdb_path:-missing}"
status "OpenOCD" "${openocd_path:-missing}"
status "pyOCD" "${pyocd_path:-missing}"
status "STM32_Programmer_CLI" "${cubeprog_path:-missing}"
status "JLinkGDBServerCLExe" "${jlink_server_path:-missing}"

echo
echo "Recommended installs on Ubuntu 22.04:"
echo "  sudo apt update"
echo "  sudo apt install -y build-essential cmake ninja-build openocd gdb-multiarch"
echo "  pipx install pyocd"
echo
echo "Probe notes:"
echo "  ST-Link  : OpenOCD or STM32CubeProgrammer"
echo "  DAPLink  : pyOCD preferred, OpenOCD CMSIS-DAP also works"
echo "  J-Link   : install SEGGER J-Link Software Pack"
