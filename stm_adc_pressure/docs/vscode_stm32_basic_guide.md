# Ubuntu 22.04 下在 VS Code 开发 STM32 基本指南

本文档基于当前工程实际情况编写：

- 芯片：`STM32F407xx`
- 工程名：`stm_adc_pressure`
- 构建系统：`CMake + Ninja`
- 预设：`Debug`、`Release`
- 当前调试方案：`OpenOCD + J-Link(SWD) + gdb-multiarch`
- 调试文件：`build/Debug/stm_adc_pressure.elf`

## 1. 先确认本机依赖

当前机器已经有下面这些工具：

- `arm-none-eabi-gcc`
- `gdb-multiarch`
- `openocd`

如果你换一台 Ubuntu 22.04 机器，最少安装这些包：

```bash
sudo apt update
sudo apt install -y cmake ninja-build gcc-arm-none-eabi gdb-multiarch openocd build-essential
```

VS Code 建议安装这 3 个扩展：

- `CMake Tools`
- `C/C++`
- `Cortex-Debug`

本工程已经提供了工作区推荐扩展，打开目录后 VS Code 一般会提示安装。

## 2. 打开工程

在项目根目录打开 VS Code：

```bash
code /home/csc/Develop/Pilot/hardware/stm32_esp32-pressure-adc/stm32-hardware/stm_adc_pressure
```

首次打开后，建议确认以下内容：

1. 左下角 CMake 状态栏能看到 `Debug` 预设。
2. 如果弹出 “Configure project” 提示，选择 `Debug`。
3. 如果扩展提示安装推荐插件，直接安装。

本工程已经在 `.vscode/settings.json` 中默认绑定：

- 默认配置预设：`Debug`
- 默认构建预设：`Debug`
- `C/C++` 由 `CMake Tools` 提供头文件和宏定义索引

## 3. 命令行构建怎么理解

你刚才执行的两条命令含义不一样：

```bash
cmake --build --preset Debug
```

这是正确命令，表示使用 `Debug` 这个 build preset 进行编译。

如果输出：

```text
ninja: no work to do.
```

说明当前源码没有变化，`build/Debug/stm_adc_pressure.elf` 已经是最新的，不是报错。

而下面这条：

```bash
cmake --build
```

会报 usage，是因为 `cmake --build` 必须至少给一个参数：

- 要么给构建目录：`cmake --build build/Debug`
- 要么给 preset：`cmake --build --preset Debug`

所以在这个工程里，平时推荐直接用：

```bash
cmake --preset Debug
cmake --build --preset Debug
```

## 4. 在 VS Code 里编译

这个工程已经提供了任务配置，可以直接使用：

1. 按 `Ctrl+Shift+P`
2. 输入 `Tasks: Run Task`
3. 选择 `CMake: build Debug`

也可以直接按：

- `Ctrl+Shift+B`：默认执行 `CMake: build Debug`

如果想强制全量重编译，运行：

- `CMake: rebuild Debug`

对应的命令行分别是：

```bash
cmake --preset Debug
cmake --build --preset Debug
```

和

```bash
cmake --preset Debug
cmake --build --preset Debug --clean-first
```

## 5. 在 VS Code 里烧录

当前工程默认按 `J-Link + OpenOCD + SWD` 方式烧录，已经提供任务：

1. 把开发板接上 USB
2. 确认板子能被 J-Link 识别
3. 按 `Ctrl+Shift+P`
4. 执行 `Tasks: Run Task`
5. 选择 `OpenOCD: flash Debug (J-Link SWD)`

这个任务会先自动编译，再执行：

```bash
openocd \
  -f .vscode/openocd-jlink-swd.cfg \
  -c "program build/Debug/stm_adc_pressure.elf verify reset exit"
```

说明：

- `program ...elf`：把 ELF 下载到单片机
- `verify`：烧录后校验
- `reset`：烧录完成后复位运行
- `exit`：OpenOCD 烧录完成后退出

## 6. 在 VS Code 里调试

本工程已经提供基础调试配置，文件在 `.vscode/launch.json`。

使用方法：

1. 连接开发板和 J-Link
2. 在 `Run and Debug` 面板选择 `STM32F407 Debug (J-Link SWD/OpenOCD)`
3. 按 `F5`

调试流程会自动做这些事：

1. 先执行 `CMake: build Debug`
2. Cortex-Debug 启动 `OpenOCD`
3. 使用 `/usr/bin/gdb-multiarch` 连接 GDB Server
4. 下载 `build/Debug/stm_adc_pressure.elf`
5. 在 `main` 处停住

你可以正常使用这些功能：

- 打断点
- 单步执行
- 查看变量
- 查看寄存器
- 查看调用栈
- 继续运行 / 暂停

## 7. 常见问题

### 7.1 `ninja: no work to do.`

这不是错误，只是表示没有文件变化，不需要重新编译。

### 7.2 `cmake --build` 直接报 usage

原因是少了构建目录或 preset。

正确写法：

```bash
cmake --build --preset Debug
```

或者：

```bash
cmake --build build/Debug
```

### 7.3 OpenOCD 连不上板子

优先检查：

1. USB 线是否支持数据传输
2. J-Link 是否正常枚举
3. 目标板是否上电
4. `VTref`、`GND`、`SWDIO`、`SWCLK` 是否接对
5. 如果接了复位脚，再确认 `NRST` 是否可靠

你这次的报错里最关键的一句是：

```text
Info : auto-selecting first available session transport "jtag"
```

这说明 OpenOCD 把 J-Link 连接方式自动选成了 `JTAG`，但 STM32F4 开发板日常大多接的是 `SWD`。

于是后面的这些报错就会连锁出现：

```text
JTAG scan chain interrogation failed: all zeroes
IR capture error; saw 0x00 not 0x01
Invalid ACK (0) in DAP response
```

这不是你的 ELF 文件有问题，而是调试接口模式错了。当前工程已经改成固定使用：

```text
transport select swd
```

对应配置文件是 `.vscode/openocd-jlink-swd.cfg`。

如果出现权限错误，例如 `LIBUSB_ERROR_ACCESS`，通常是 udev 规则或用户权限问题。可以先临时用 `sudo openocd ...` 验证是否为权限问题，再补齐 udev 规则，不建议长期用 sudo 作为日常方案。

如果改成 SWD 后仍然偶发连接失败，可以把 `.vscode/openocd-jlink-swd.cfg` 里的：

```text
adapter speed 2000
```

改成：

```text
adapter speed 1000
```

或更低后再试。

### 7.4 断点打不上

先确认：

1. 当前编译的是 `Debug`
2. 产物确实是 `build/Debug/stm_adc_pressure.elf`
3. 代码已经重新编译并重新下载

## 8. 这套工程最常用的 3 个动作

日常开发基本就这三个：

### 编译

```bash
cmake --preset Debug
cmake --build --preset Debug
```

### 烧录

```bash
openocd -f .vscode/openocd-jlink-swd.cfg -c "program build/Debug/stm_adc_pressure.elf verify reset exit"
```

### 调试

在 VS Code 里按 `F5`，选择 `STM32F407 Debug (J-Link SWD/OpenOCD)`。

## 9. 如果后面你要扩展

后续如果你需要，我可以继续帮你补下面这些内容：

- 自动生成 `.bin` / `.hex`
- `Release` 版本的 VS Code 任务
- 串口日志查看任务
- `.svd` 外设寄存器视图
- ST-Link 和 J-Link 双配置切换
