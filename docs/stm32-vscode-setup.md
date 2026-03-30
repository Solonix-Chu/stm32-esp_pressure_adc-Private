# STM32 VSCode Workspace Setup

这个工作区已经补好了 VSCode 模板，目标是让你在 Ubuntu 22.04 下直接用 VSCode 完成 STM32 的编译、烧录和调试。

## 1. 现有前提

- 已安装 `STM32CubeMX`
- 当前系统已存在 `arm-none-eabi-gcc`、`cmake`、`ninja`、`make`
- 当前系统还缺少 `openocd`、`gdb-multiarch`、`STM32_Programmer_CLI`、`JLinkGDBServerCLExe`

## 2. 推荐安装

### Ubuntu 包

```bash
sudo apt update
sudo apt install -y build-essential cmake ninja-build openocd gdb-multiarch
```

### DAPLink / CMSIS-DAP

推荐安装 `pyocd`，DAPLink 调试体验通常比直接走 OpenOCD 更省事：

```bash
pipx install pyocd
```

如果你没有 `pipx`：

```bash
sudo apt install -y pipx
pipx ensurepath
pipx install pyocd
```

### J-Link

安装 SEGGER 的 J-Link Software Pack，确保系统里有：

- `JLinkExe`
- `JLinkGDBServerCLExe`

### STM32 官方烧录工具

如果你希望用 ST 官方 CLI 烧录，安装 STM32CubeProgrammer，确保系统里有：

- `STM32_Programmer_CLI`

## 3. USB 权限

Linux 下如果不配 `udev`，常见现象是 VSCode 能看到配置但不能正常连接探针。

样例规则已经放在：

- `docs/60-stm32-probes.rules.sample`

复制到系统目录后执行：

```bash
sudo cp docs/60-stm32-probes.rules.sample /etc/udev/rules.d/60-stm32-probes.rules
sudo udevadm control --reload-rules
sudo udevadm trigger
sudo usermod -aG plugdev "$USER"
```

重新登录后生效。

如果你的 DAPLink 不是 `0d28` 厂商 ID，用 `lsusb` 查实际 `idVendor` 后改规则。

## 4. CubeMX 里怎么导出工程

最适合 VSCode 的方式是直接让 CubeMX 生成 `CMake` 工程。

CubeMX 里建议这样做：

1. `Project Manager -> Toolchain / IDE`
2. 选 `CMake`
3. `Generate Under Root` 按你的工程习惯决定
4. 代码直接生成到这个工作区目录

如果你仍然想用 `Makefile`，这个工作区也能工作，`STM32: Build` 会自动在 `CMakeLists.txt` 和 `Makefile` 之间切换。

## 5. 需要你改的 VSCode 参数

打开 `.vscode/settings.json`，至少把下面几项改成你的真实工程值：

```json
"stm32.device": "STM32F103C8",
"stm32.elfPath": "${workspaceFolder}/build/firmware.elf",
"stm32.binPath": "${workspaceFolder}/build/firmware.bin",
"stm32.openocd.target": "target/stm32f1x.cfg",
"stm32.jlink.device": "STM32F103C8",
"stm32.pyocd.target": "stm32f103c8"
```

你真正要改的是：

- 芯片型号
- ELF/BIN 路径
- OpenOCD 的 target 配置
- J-Link 的 device 名
- pyOCD 的 target 名

如果你有对应芯片的 `svd` 文件，也可以补上：

```json
"stm32.svdFile": "/absolute/path/to/your-device.svd"
```

## 6. VSCode 里怎么用

### 扩展

工作区已经推荐安装这些扩展：

- `ms-vscode.cpptools`
- `ms-vscode.cmake-tools`
- `ms-vscode.makefile-tools`
- `marus25.cortex-debug`

### 常用任务

- `STM32: Check Environment`
- `STM32: Build`
- `STM32: Flash (STM32CubeProgrammer)`
- `STM32: Flash (ST-Link/OpenOCD)`
- `STM32: Flash (DAPLink/OpenOCD)`
- `STM32: Flash (DAPLink/pyOCD)`
- `STM32: Flash (J-Link)`

### 调试配置

`launch.json` 里已经准备了四个入口：

- `STM32: ST-Link (OpenOCD)`
- `STM32: DAPLink (OpenOCD CMSIS-DAP)`
- `STM32: DAPLink (pyOCD)`
- `STM32: J-Link`

通常建议：

- `ST-Link` 用 `OpenOCD`
- `DAPLink` 优先用 `pyOCD`
- `J-Link` 用 `J-Link GDB Server`

## 7. 首次验证顺序

建议第一次按这个顺序检查：

1. VSCode 打开工作区
2. 运行 `STM32: Check Environment`
3. 用 CubeMX 生成工程到当前目录
4. 修改 `.vscode/settings.json`
5. 运行 `STM32: Build`
6. 先跑一次烧录任务
7. 最后再启动 `launch.json` 调试

## 8. 备注

- `STM32: Build` 会自动优先使用 `CMakeLists.txt`
- 如果只有 `Makefile`，它会自动走 `make`
- `flash-openocd.sh` 对 `elf` 和 `bin` 都能处理
- `flash-jlink.sh` 对 `elf` 和 `bin` 都能处理
- `flash-cubeprog.sh` 对 `bin` 会自动带上烧录地址
