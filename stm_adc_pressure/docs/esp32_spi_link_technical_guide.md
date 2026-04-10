# STM32 -> ESP32 SPI 数据链路技术文档

本文档基于当前 `stm_adc_pressure` 工程的实际固件实现编写，用于指导 `ESP32` 侧工程生成、硬件连线、协议解析、文件记录和后续上位机处理对接。

本文档只覆盖第一跳链路：

- `STM32F407 -> ESP32`

不限定第二跳实现方式：

- `ESP32 -> SD 卡文件`
- `ESP32 -> Wi-Fi 上位机`
- `ESP32 -> USB Serial`
- `ESP32 -> WebSocket/MQTT`

## 1. 链路概览

当前链路方案固定为：

- `STM32 SPI2 Slave`
- `ESP32 SPI Master`
- `DMA` 收发
- `PB5` 作为 `DATA_RDY` / `ESP_LINK_RDY`
- 固定长度二进制包

当前实现要点：

- STM32 在 ADC 半缓冲或满缓冲就绪后封一包原始采样数据。
- STM32 拉高 `ESP_LINK_RDY` 表示“有一包待取”。
- ESP32 检测到 `ESP_LINK_RDY=1` 后，发起一次完整 SPI 事务。
- 当前线上事务长度固定为 `3116 bytes`。
- 其中前部是完整 `3112 bytes` 协议包，尾部附加 `4 bytes` 保护填充，用于吸收从机首字节输出延迟。
- 事务完成后 STM32 拉低 `ESP_LINK_RDY`。
- 若事务失败或超时，STM32 会复位本次链路状态并计入错误计数。

## 2. 当前硬件连接

### 2.1 SPI 链路引脚

| 功能 | STM32 引脚 | 方向 | 说明 |
| --- | --- | --- | --- |
| `ESP_LINK_RDY` | `PB5` | STM32 -> ESP32 | 数据就绪指示，高电平表示有一包待取 |
| `SPI2_NSS` | `PB12` | ESP32 -> STM32 | 片选 |
| `SPI2_SCK` | `PB13` | ESP32 -> STM32 | 时钟 |
| `SPI2_MISO` | `PB14` | STM32 -> ESP32 | 数据输出 |
| `SPI2_MOSI` | `PB15` | ESP32 -> STM32 | 主机发送占位字节，当前业务上不使用 |
| `GND` | `GND` | 双向参考 | 必接 |

### 2.2 电平与基础要求

- 双方均为 `3.3V` 逻辑。
- 必须共地。
- `ESP_LINK_RDY` 建议由 ESP32 配置为普通输入。
- `SPI2_NSS` 必须由 ESP32 主动控制，当前 STM32 使用硬件 `NSS input`。

## 3. STM32 当前关键参数

### 3.1 ADC 参数

当前 STM32 固件固定为：

- ADC 实例：`ADC1`
- 通道数：`6`
- 通道顺序：
  - `CH1 -> PA0 -> ADC1_IN0`
  - `CH2 -> PA1 -> ADC1_IN1`
  - `CH3 -> PA4 -> ADC1_IN4`
  - `CH4 -> PA5 -> ADC1_IN5`
  - `CH5 -> PA6 -> ADC1_IN6`
  - `CH6 -> PA7 -> ADC1_IN7`
- 分辨率：`12 bit`
- 采样时间：`56 cycles`
- 触发源：`TIM2 TRGO`
- 触发周期：`10 ms`
- 每路有效采样频率：`100 Hz`

### 3.2 DMA / 包节拍

当前应用层参数：

- `APP_ADC_DMA_FRAME_COUNT = 512`
- `APP_ADC_CHANNEL_COUNT = 6`
- `APP_ADC_DMA_BUFFER_SIZE = 3072 samples`
- `APP_ADC_HALF_BUFFER_SIZE = 1536 samples`

因此每次发包对应：

- 总样本数：`1536`
- 每通道样本数：`256`
- 包产生周期：`2560 ms`
- 包频率：约 `0.39 包/秒`

## 4. SPI 参数

STM32 当前 SPI 配置固定为：

- 外设：`SPI2`
- 角色：`Slave`
- 数据宽度：`8 bit`
- 模式：`Mode 0`
  - `CPOL=0`
  - `CPHA=0`
- bit 顺序：`MSB first`
- 线制：`2 lines full duplex`
- `NSS`：硬件输入

ESP32 侧推荐先按以下参数启动：

- SPI mode：`0`
- bit order：`MSB first`
- 事务长度：`3116 bytes`
- 主时钟：建议从 `8 MHz` 起步

说明：

- 从纯吞吐量看，链路最低可用时钟约为 `5 MHz`。
- 为保守起见，建议先以 `8 MHz` 联调，再视波形质量提升到 `10~12 MHz`。

## 5. 包格式

### 5.1 总长度

当前逻辑协议包固定长度：

- `3112 bytes`

其中：

- 包头：`40 bytes`
- 载荷：`3072 bytes`

说明：

- SPI 线上会在协议包尾部额外多传 `4 byte` 保护填充。
- ESP32 侧建议对接收缓冲的前 `0~4 byte` 偏移自动扫描，找到合法包头后再按 `3112 bytes` 协议包解析。

### 5.2 头部结构

STM32 侧结构体如下：

```c
typedef struct __attribute__((packed))
{
  uint32_t magic;
  uint16_t version;
  uint16_t header_bytes;
  uint32_t sequence;
  uint32_t tick_ms;
  uint32_t sample_rate_hz;
  uint16_t channel_count;
  uint16_t samples_per_channel;
  uint16_t bits_per_sample;
  uint16_t flags;
  uint32_t dropped_packets;
  uint32_t payload_bytes;
  uint32_t checksum;
} AppLinkPacketHeader;
```

### 5.3 头部字段定义

所有字段均为：

- 小端字节序
- 无符号整数

| 偏移 | 长度 | 字段 | 当前值/说明 |
| --- | --- | --- | --- |
| `0` | `4` | `magic` | 固定 `0x314B4E4C`，线上的字节序为 `4C 4E 4B 31`，对应 ASCII `LNK1` |
| `4` | `2` | `version` | 固定 `1` |
| `6` | `2` | `header_bytes` | 固定 `40` |
| `8` | `4` | `sequence` | 包序号，随每次半包/满包处理递增 |
| `12` | `4` | `tick_ms` | STM32 `osKernelGetTickCount()` 时间戳，单位 `ms` |
| `16` | `4` | `sample_rate_hz` | 当前固定 `100` |
| `20` | `2` | `channel_count` | 当前固定 `6` |
| `22` | `2` | `samples_per_channel` | 当前固定 `256` |
| `24` | `2` | `bits_per_sample` | 当前固定 `12` |
| `26` | `2` | `flags` | `0x0001=half`，`0x0002=full` |
| `28` | `4` | `dropped_packets` | STM32 侧累计丢包数 |
| `32` | `4` | `payload_bytes` | 当前固定 `3072` |
| `36` | `4` | `checksum` | 当前包校验值 |

### 5.4 载荷格式

载荷区从偏移 `40` 开始，总长度 `3072 bytes`。

载荷解释方式：

- 数据类型：`uint16_t`
- 小端
- 共 `1536` 个样本
- 按 ADC 扫描顺序交错排列

也就是：

```text
sample[0]  = CH1 frame0
sample[1]  = CH2 frame0
sample[2]  = CH3 frame0
sample[3]  = CH4 frame0
sample[4]  = CH5 frame0
sample[5]  = CH6 frame0
sample[6]  = CH1 frame1
sample[7]  = CH2 frame1
...
```

如果 ESP32 侧需要按通道落盘或上送，需要自行去交错。

## 6. 校验算法

当前 `checksum` 使用 `FNV-1a 32-bit`：

- 初值：`2166136261`
- 乘子：`16777619`

STM32 侧计算规则：

1. 先把 `checksum` 字段置 `0`
2. 对整包 `3112 bytes` 逐字节做 FNV-1a
3. 把结果写回 `checksum`

ESP32 校验步骤应为：

1. 读取整包
2. 保存原始 `checksum`
3. 将包内 `checksum` 字段临时清零
4. 对整包重新计算 FNV-1a
5. 与原值比较

如果校验失败：

- 当前建议丢弃该包
- 并记录日志或错误计数

## 7. 时序与握手

### 7.1 STM32 行为

- 无包待发时：`ESP_LINK_RDY = 0`
- 当一包已准备好且即将启动 SPI DMA：`ESP_LINK_RDY = 1`
- 当本次 SPI DMA 完成：`ESP_LINK_RDY = 0`
- 若 DMA 启动失败、传输错误或超时：`ESP_LINK_RDY = 0`，并复位 SPI 状态

### 7.2 ESP32 推荐事务流程

1. 轮询或中断检测 `ESP_LINK_RDY`
2. 当 `ESP_LINK_RDY=1`：
   - 拉低 `NSS`
   - 发起一次 `3116 bytes` 的 SPI 全双工事务
   - MOSI 发送任意占位字节即可，建议 `0xFF`
   - 在接收缓冲前 `0~4 byte` 偏移中搜索合法包头
3. 读取完毕后释放 `NSS`
4. 解析并校验包头
5. 将原始包写入文件或送到后续处理模块

### 7.3 时间窗口

当前链路缓存能力由 STM32 侧包池决定：

- 包池深度：`4`
- 每包覆盖时间：`2560 ms`
- 理论连续缓存窗口：约 `1.024 s`

这意味着：

- ESP32 端平均处理速度必须高于约 `0.39 包/秒`
- 如果持续约 `1 s` 都未能取走数据，STM32 侧就可能开始丢包

另外，STM32 侧单次 SPI 事务等待超时为：

- `100 ms`

如果 ESP32 拉高准备后长期不发起读取，STM32 会认为本次事务失败并重置链路。

## 8. 吞吐量预算

当前链路的典型数据量级：

- 单包：`3112 bytes`
- 包频率：约 `0.39 包/秒`
- 平均链路负载：约 `12.2 kB/s`

换算成 SPI 线速需求：

- 最低理论速率：约 `0.10 MHz`
- 推荐起步速率：`2 MHz`

如果后续你继续提高 ADC 采样率、增加通道数或增大包头，ESP32 侧 SPI 时钟需要同步评估。

## 9. ESP32 工程推荐结构

推荐 ESP32 侧至少拆成三个职责模块：

### 9.1 `spi_link`

职责：

- 管理 `RDY` 引脚监听
- 发起固定长度 SPI 事务
- 完成包头和 checksum 校验

推荐输出：

- `struct link_packet`
- 或直接输出完整原始二进制包

### 9.2 `file_logger`

职责：

- 将收到的包顺序写入 SD 卡或 Flash 文件
- 文件格式建议直接保存原始包流，不做文本转换

推荐：

- 按二进制顺序追加写入 `.bin`
- 单独生成一个简单的 `.json` 或 `.txt` 元信息文件，记录采样率、通道数、协议版本

### 9.3 `uplink_processor`

职责：

- 进行去交错、降采样、频谱处理、滤波或上位机传输

建议：

- 文件记录与实时上传分离
- 原始记录优先，在线上送可以做降采样摘要

## 10. ESP32 端建议默认参数

若使用 `ESP-IDF`，建议默认按以下方向初始化：

- SPI host：任选空闲主机控制器
- 模式：`mode 0`
- DMA：启用
- 单事务长度：固定 `3112`
- 事务队列：建议 `2~4`
- 接收缓冲：至少 `2 * 3112 bytes`
- 文件写入缓存：建议 `>= 16 KB`

软件调度上建议：

- `spi_rx_task` 优先级最高
- `file_writer_task` 次高
- `network/uplink_task` 再次

## 11. 接收端解析示例

接收一包后，建议按下面顺序处理：

1. 校验总长度是否为 `3112`
2. 检查 `magic == 0x314B4E4C`
3. 检查 `version == 1`
4. 检查 `header_bytes == 40`
5. 检查 `channel_count == 6`
6. 检查 `samples_per_channel == 256`
7. 检查 `payload_bytes == 3072`
8. 校验 checksum
9. 若全部通过：
   - 直接落盘原始包
   - 或将 `payload` 去交错成 6 路数组

## 12. 文件记录建议

若 ESP32 负责文件记录，推荐直接保存“完整包流”，不要先转 CSV：

- 优点：写入快
- 优点：保留原始协议字段和丢包计数
- 优点：后处理更灵活

推荐格式：

- `adc_raw_YYYYMMDD_HHMMSS.bin`

可选元信息文件：

- `adc_raw_YYYYMMDD_HHMMSS.json`

可选元信息内容：

- 板卡型号
- 固件版本
- 通道映射
- 采样率
- 协议版本
- 起始时间

## 13. 联调检查项

### 13.1 STM32 串口日志

STM32 上电后应看到类似日志：

```text
[LINK] SPI2 slave PB12/PB13/PB14/PB15 RDY=PB5
[LINK] pkt=3112B wire=3116B payload=3072B 100Hz/ch
[ADC] scan dma armed, trigger=TIM2 100Hz, buffer=3072 samples
```

### 13.2 示波器/逻辑分析仪建议观察

- `PB5` 是否周期性拉高
- `PB12` 是否在 `PB5` 拉高后被 ESP32 拉低
- `PB13` 是否有稳定时钟
- `PB14` 是否输出连续数据

### 13.3 运行统计

当前 STM32 监控日志中会输出：

- `link sent`
- `drop`
- `err`
- `queued`

联调目标：

- `drop = 0`
- `err = 0`

## 14. 当前版本的边界说明

- 当前 MOSI 数据未被 STM32 业务层使用，ESP32 可发送固定填充值。
- 当前协议是固定长度包，不支持变长事务。
- 当前协议未做命令下发、参数回写或流控协商。
- 当前文档对应协议版本 `1`，如后续修改包头字段或长度，需要同步升级 `version`。
