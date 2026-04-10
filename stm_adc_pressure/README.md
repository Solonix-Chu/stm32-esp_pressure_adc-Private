# stm_adc_pressure

基于 `STM32F407ZETx` 的多通道 ADC 采集与 OLED 波形显示工程。

当前工程使用：

- `TIM2 + ADC1 + Scan + DMA Circular` 采集 6 路模拟量
- `FreeRTOS(CMSIS V2)` 组织采集、显示和监控任务
- 板载 `128x64 OLED` 显示实时电压曲线
- `SPI2 + DMA` 向 `ESP32` 实时输出原始 ADC 数据包
- `USART1` 输出启动和运行日志
- `KEY2` 切换显示页面

详细的 VS Code 开发与调试说明见 [docs/vscode_stm32_basic_guide.md](docs/vscode_stm32_basic_guide.md)。

STM32 与 ESP32 的 SPI 数据链路对接说明见 [docs/esp32_spi_link_technical_guide.md](docs/esp32_spi_link_technical_guide.md)。

## 功能介绍

- 6 路 ADC 由 `TIM2` 以 `100 Hz` 触发整组扫描，DMA 循环搬运数据
- OLED 显示单通道或全部通道叠加曲线
- 曲线页使用 `左侧 Y 轴 + 底部 X 轴 + 短刻度`
- 横坐标量程为 `2 秒/格`
- 纵坐标量程为 `500 mV/格`
- 串口周期输出 ADC 统计信息和任务运行状态
- 按键去抖后触发页面切换
- SPI 数据链路向 ESP32 输出固定长度原始采样包

当前显示与采样行为：

- 页面顺序：`CH1 -> CH2 -> CH3 -> CH4 -> CH5 -> CH6 -> ALL -> CH1`
- OLED 曲线历史点更新周期：`125 ms`
- ADC 当前采样时间：`56 cycles`
- 当前每路 ADC 有效采样频率为 `100 Hz`
- SPI 原始数据包固定载荷为 `3072 bytes`

## 工程分层

仓库默认采用 `CubeMX 生成层 + User 自定义层` 分层：

- `Core/`、`Drivers/`、`stm_adc_pressure.ioc`
  - CubeMX 生成层
  - 负责芯片、GPIO、DMA、ADC、USART、FreeRTOS 基础接入
- `User/Drivers`
  - 板级驱动与 HAL 封装
  - 当前包含 OLED 总线、串口日志、按键驱动
- `User/Components`
  - 组件层
  - 当前包含 SSD1306 OLED 组件与基础绘图接口
- `User/App`
  - 应用层
  - 当前包含 ADC 数据处理、OLED 曲线渲染、按键切页、RTOS 任务编排

## 引脚连接

### ADC 输入

当前 6 路逻辑通道与物理引脚关系如下：

| 逻辑通道 | MCU 引脚 | ADC 通道 |
| --- | --- | --- |
| CH1 | `PA0` | `ADC1_IN0` |
| CH2 | `PA1` | `ADC1_IN1` |
| CH3 | `PA4` | `ADC1_IN4` |
| CH4 | `PA5` | `ADC1_IN5` |
| CH5 | `PA6` | `ADC1_IN6` |
| CH6 | `PA7` | `ADC1_IN7` |

说明：

- `PA2`、`PA3` 当前不再参与 ADC 采集
- OLED 和串口中仍以 `CH1..CH6` 的逻辑名称显示

### OLED

板载 OLED 采用 `SSD1306 + SPI3 硬件 SPI`：

| 功能 | MCU 引脚 |
| --- | --- |
| `OLED_SCLK` | `PC10` |
| `OLED_CS` | `PG11` |
| `OLED_RST` | `PG13` |
| `OLED_SDIN` | `PC12` |
| `OLED_DC` | `PG15` |

### 串口日志

| 功能 | MCU 引脚 | 参数 |
| --- | --- | --- |
| `USART1_TX` | `PB6` | `115200 8N1` |
| `USART1_RX` | `PB7` | `115200 8N1` |

### STM32 -> ESP32 数据链路

当前推荐 `ESP32` 作为 `SPI Master`，`STM32` 作为 `SPI2 Slave`：

| 功能 | MCU 引脚 | 说明 |
| --- | --- | --- |
| `ESP_LINK_RDY` | `PB5` | STM32 数据就绪输出，高电平表示有一包待取 |
| `SPI2_NSS` | `PB12` | 片选，由 ESP32 驱动 |
| `SPI2_SCK` | `PB13` | 时钟，由 ESP32 驱动 |
| `SPI2_MISO` | `PB14` | STM32 输出数据到 ESP32 |
| `SPI2_MOSI` | `PB15` | ESP32 发送占位字节到 STM32 |

推荐按当前 `esp-pilot` 默认 GPIO 做如下接线：

| STM32 引脚 | ESP32-S3 引脚 | 信号 |
| --- | --- | --- |
| `PB5` | `GPIO10` | `RDY` |
| `PB12` | `GPIO7` | `CS/NSS` |
| `PB13` | `GPIO4` | `SCK` |
| `PB14` | `GPIO5` | `MISO` |
| `PB15` | `GPIO6` | `MOSI` |
| `GND` | `GND` | 公共地 |

说明：

- `STM32 PB14(MISO)` 要接到 `ESP32 GPIO5(MISO)`
- `STM32 PB15(MOSI)` 要接到 `ESP32 GPIO6(MOSI)`
- 双方必须共地，且都使用 `3.3V` 逻辑电平

### 用户按键

| 按键 | MCU 引脚 | 当前用途 |
| --- | --- | --- |
| `KEY0` | `PE4` | 预留 |
| `KEY1` | `PE3` | 预留 |
| `KEY2` | `PE2` | 切换 OLED 曲线页 |

## 简要使用说明

### 1. 硬件连接

- 给开发板正常供电
- 将待测模拟信号接到 `PA0/PA1/PA4/PA5/PA6/PA7`
- 若需要查看日志，连接 `USART1` 到 USB 转串口
- 若需要接 `ESP32`，按 `PB5/PB12/PB13/PB14/PB15 + GND` 连接 SPI
  - 对应 `ESP32-S3` 默认引脚为 `GPIO10/GPIO7/GPIO4/GPIO5/GPIO6 + GND`
- 若需要烧录调试，连接 `J-Link + SWD`

建议：

- ADC 输入尽量使用低阻抗信号源
- 多通道测试时先共地，再逐路接入

### 2. 编译

推荐使用 CMake Preset：

```bash
cmake --preset Debug
cmake --build --preset Debug
```

如果你已经使用本地 `build-arm/` 目录，也可以继续：

```bash
cmake --build build-arm
```

### 3. 烧录

可使用你现有的 VS Code 任务或 OpenOCD/J-Link 流程烧录。

如果使用 VS Code，推荐直接参考 [docs/vscode_stm32_basic_guide.md](docs/vscode_stm32_basic_guide.md) 中的任务与调试配置。

### 4. 上电后的行为

- OLED 启动后先显示 boot banner
- ADC DMA 启动后开始由 `TIM2` 按 `10 ms` 周期触发扫描
- OLED 进入曲线显示页
- 串口输出启动日志、RTOS 状态和 ADC 统计信息

`KEY2` 操作：

- 每按一次切换到下一页
- 页面循环为 `CH1 -> CH2 -> CH3 -> CH4 -> CH5 -> CH6 -> ALL -> CH1`

`ALL` 页面：

- 同时叠加显示 6 路通道曲线

### 5. ESP32 侧对接约定

- `ESP32` 作为 `SPI Master` 轮询 `PB5`
- 当 `ESP_LINK_RDY=1` 时，ESP32 拉低 `NSS` 并一次性读取一整包固定长度数据
- 当前单包总长度为 `3112 bytes`
- 包格式为：
  - `40 bytes` 头部
  - `3072 bytes` 原始 ADC 载荷
- 原始载荷按当前扫描顺序交错排列：
  - `CH1, CH2, CH3, CH4, CH5, CH6` 循环
- 每个采样值为 `uint16_t` 小端格式，实际有效位宽 `12 bit`

## 运行日志示例

串口上电后可看到类似日志：

```text
[BOOT] app init
[UART] USART1 PB6/PB7 115200 8N1
[RTOS] CubeMX FreeRTOS CMSIS-V2 enabled
[KEY] KEY0=PE4 KEY1=PE3 KEY2=PE2
[OLED] SPI3 SCK=PC10 MOSI=PC12 CS=PG11 RST=PG13 DC=PG15
[OLED] plot 2S/GRID 500MV/GRID
[LINK] SPI2 slave PB12/PB13/PB14/PB15 RDY=PB5
[ADC] scan dma started, buffer=3072 samples
```

## 目录说明

- `stm_adc_pressure.ioc`
  - CubeMX 工程配置
- `Core/`
  - 外设初始化与生成代码
- `User/Drivers`
  - 按键、OLED 总线、串口日志、ESP32 链路驱动
- `User/Components`
  - OLED 组件与绘图接口
- `User/App`
  - ADC 业务、页面渲染、RTOS 任务
- `docs/`
  - 开发与调试说明文档

## 当前已知说明

- OLED 显示坐标轴与短刻度，不绘制整屏网格
- 当前标题栏仍使用简化文本，不额外显示物理引脚名
- 若改动 ADC 引脚、DMA、USART、GPIO 模式等硬件配置，应同步更新 `.ioc` 与对应生成代码
