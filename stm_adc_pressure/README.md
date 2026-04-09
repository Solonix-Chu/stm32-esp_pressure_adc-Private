# stm_adc_pressure

基于 `STM32F407ZETx` 的多通道 ADC 采集与 OLED 波形显示工程。

当前工程使用：

- `ADC1 + Scan + DMA Circular` 采集 6 路模拟量
- `FreeRTOS(CMSIS V2)` 组织采集、显示和监控任务
- 板载 `128x64 OLED` 显示实时电压曲线
- `USART1` 输出启动和运行日志
- `KEY2` 切换显示页面

详细的 VS Code 开发与调试说明见 [docs/vscode_stm32_basic_guide.md](docs/vscode_stm32_basic_guide.md)。

## 功能介绍

- 6 路 ADC 连续扫描采样，DMA 循环搬运数据
- OLED 显示单通道或全部通道叠加曲线
- 曲线页使用 `左侧 Y 轴 + 底部 X 轴 + 短刻度`
- 横坐标量程为 `2 秒/格`
- 纵坐标量程为 `500 mV/格`
- 串口周期输出 ADC 统计信息和任务运行状态
- 按键去抖后触发页面切换

当前显示与采样行为：

- 页面顺序：`CH1 -> CH2 -> CH3 -> CH4 -> CH5 -> CH6 -> ALL -> CH1`
- OLED 曲线历史点更新周期：`125 ms`
- ADC 当前采样时间：`56 cycles`
- 当前每路 ADC 有效采样频率约为 `51.5 kHz`

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

板载 OLED 采用固定 `SSD1306 + 4 线软件 SPI`：

| 功能 | MCU 引脚 |
| --- | --- |
| `OLED_SCLK` | `PG10` |
| `OLED_CS` | `PG11` |
| `OLED_RST` | `PG13` |
| `OLED_SDIN` | `PG14` |
| `OLED_DC` | `PG15` |

### 串口日志

| 功能 | MCU 引脚 | 参数 |
| --- | --- | --- |
| `USART1_TX` | `PB6` | `115200 8N1` |
| `USART1_RX` | `PB7` | `115200 8N1` |

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
- ADC DMA 启动后开始连续采样
- OLED 进入曲线显示页
- 串口输出启动日志、RTOS 状态和 ADC 统计信息

`KEY2` 操作：

- 每按一次切换到下一页
- 页面循环为 `CH1 -> CH2 -> CH3 -> CH4 -> CH5 -> CH6 -> ALL -> CH1`

`ALL` 页面：

- 同时叠加显示 6 路通道曲线

## 运行日志示例

串口上电后可看到类似日志：

```text
[BOOT] app init
[UART] USART1 PB6/PB7 115200 8N1
[RTOS] CubeMX FreeRTOS CMSIS-V2 enabled
[KEY] KEY0=PE4 KEY1=PE3 KEY2=PE2
[OLED] plot 2S/GRID 500MV/GRID
[ADC] scan dma started, buffer=384 samples
```

## 目录说明

- `stm_adc_pressure.ioc`
  - CubeMX 工程配置
- `Core/`
  - 外设初始化与生成代码
- `User/Drivers`
  - 按键、OLED 总线、串口日志驱动
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
