# Repository Working Rules

本仓库默认采用 `CubeMX 生成层 + User 自定义层` 的分层方式。后续在本仓库内工作的 Codex 会话应遵循以下规则，除非用户明确要求偏离。

## 1. 分层边界

### CubeMX / 生成层

以下内容视为 CubeMX 或工程生成层：

- `stm_adc_pressure.ioc`
- `Core/`
- `Drivers/`
- `startup_stm32f407xx.s`
- `STM32F407XX_FLASH.ld`
- `cmake/stm32cubemx/`

这部分的职责仅限于：

- 芯片与外设初始化
- 中断入口、启动文件、HAL 适配
- 由 CubeMX 维护的引脚宏、外设句柄、初始化函数
- 极薄的应用桥接代码

### User 自定义层

所有非 CubeMX 生成的业务/设备逻辑，默认都放在 `User/` 下，并继续按三层拆分：

- `User/Drivers`
  - 放底层驱动、总线时序、GPIO/HAL 封装、板级外设访问
  - 可以依赖 HAL 和 `Core/Inc/main.h` 中的生成引脚宏
  - 不放应用流程和页面逻辑

- `User/Components`
  - 放设备组件或可复用功能组件
  - 基于 `Drivers` 封装更稳定的组件接口
  - 不直接承担应用调度、主循环策略或 CubeMX 初始化

- `User/App`
  - 放应用入口、启动流程、轮询逻辑、任务编排、日志调用
  - 负责把多个组件组合成当前产品行为

## 2. 代码放置规则

- 非生成逻辑不要直接混入 `Core/Src` 或 `Core/Inc`。
- `Core/Src/main.c` 应保持为薄入口：
  - 初始化 CubeMX 配置的外设
  - 通过 `USER CODE` 区域调用 `App_Init()` / `App_Run()` 之类的应用层入口
- 如果某个功能与 CubeMX 无关，应优先新建 `User/...` 文件，而不是继续往 `Core/...` 里堆代码。
- `Core/Inc/main.h` 中允许存在 CubeMX 生成的引脚宏，但对这些引脚的行为封装应放在 `User/Drivers` 或 `User/Components`。

## 3. 命名与组织约定

- 底层驱动文件使用 `drv_*.c/.h`
- 组件文件使用 `comp_*.c/.h`
- 应用入口/应用逻辑文件使用 `app_*.c/.h`
- 新增用户代码时，优先接入 `User/CMakeLists.txt`，保持根 `CMakeLists.txt` 只做薄链接和总装配。
- 当前推荐继续通过 `user_layers` 静态库聚合 `User/` 下的源文件。

## 4. 变更同步规则

- 只要涉及硬件引脚、外设实例、DMA、串口、ADC、GPIO 模式等硬件配置，必须同步更新：
  - `stm_adc_pressure.ioc`
  - 对应生成出来的 `Core/...` 文件
- 不要把本应由 `.ioc` 描述的硬件变化只藏在用户代码里。
- 如果必须改动生成文件，优先只改 `USER CODE` 区域；若某段自定义逻辑会被 CubeMX 覆盖，应先把它迁出到 `User/`。

## 5. 当前仓库的默认架构取向

- OLED、串口日志、类似这类非 CubeMX 业务功能，默认属于 `User/...`，不回放到 `Core/...`
- CubeMX 层负责“把外设和引脚拉起来”
- User 层负责“这些外设如何组成产品功能”
- 后续新增功能时，优先沿用这套拆分，而不是重新把逻辑耦合回 `Core/`

## 6. FreeRTOS 约定

本仓库当前默认使用 `CubeMX 引入 FreeRTOS + User/App 编排业务任务` 的方式。后续涉及 RTOS 的修改，默认遵循以下规则：

### FreeRTOS 接入边界

- FreeRTOS 内核、中间件引入、`freertos.c` 框架、`FreeRTOSConfig.h`、`.ioc` 中的 RTOS 使能，默认由 CubeMX 管理。
- `Core/Src/main.c`、`Core/Src/freertos.c` 只保留 RTOS 启动、`MX_FREERTOS_Init()` 调用和极薄的桥接逻辑。
- 具体业务线程、消息队列、线程标志、数据处理流程，默认放在 `User/App`，当前入口为 `App_RtosInit()`。
- 除非用户明确要求，不要把具体业务循环重新塞回 CubeMX 生成的 `StartDefaultTask()` 中。

### 当前默认任务模型

- `adcTask`
  - 职责：启动 `ADC1 + DMA`，等待半缓冲/满缓冲事件，计算当前采样统计结果
  - 优先级：`osPriorityHigh`
  - 特点：事件驱动，不做显示刷新，不做低价值阻塞日志

- `displayTask`
  - 职责：周期性刷新 OLED，消费最新 ADC 统计值
  - 优先级：`osPriorityBelowNormal`
  - 特点：低于采集链路，不能反向阻塞采样

- `monitorTask`
  - 职责：周期性串口日志、运行状态和栈余量输出
  - 优先级：`osPriorityLow`
  - 特点：诊断任务始终低于采集和显示

- `defaultTask`
  - 默认不承载业务职责
  - 若 CubeMX 自动生成，保持为空壳或尽快退出，不把应用逻辑长期挂在其中

### 优先级分配原则

- 数据采集、实时性更强、由中断直接驱动的任务，优先级高于显示和日志任务。
- 人机显示、UI 刷新类任务，默认使用普通或偏低优先级，避免抢占采样链路。
- 日志、监控、心跳、诊断类任务，默认放在最低一档业务优先级。
- 若两个任务没有严格的抢占关系且都属于后台工作，可放在同一优先级并依赖时间片轮转。
- 若任务之间存在明确的实时先后关系，应通过不同优先级和事件唤醒表达，不要只依赖时间片“碰运气”。

### 时间片与调度原则

- 当前默认开启时间片轮转，系统 tick 频率按 `1 ms` 理解。
- 同优先级任务可以共享时间片，但不应在同优先级堆叠多个长时间占 CPU 的 busy loop。
- 周期任务优先使用 `osDelayUntil()` 维持稳定周期，而不是简单 `osDelay()` 漂移运行。
- 事件驱动任务优先阻塞等待通知，不做空转轮询。

### 线程间通信默认选型

- `ISR -> 单个高优先级任务`
  - 默认优先使用线程标志或 direct notification 这一类最低开销机制
  - 当前 `ADC DMA half/full complete -> adcTask` 就属于这种模式

- `生产者 -> 消费者` 且只关心“最新值”
  - 默认优先使用单元素队列或覆盖式队列思路
  - 队列满时丢弃旧值、保留最新值，适合显示刷新和状态日志

- `生产者 -> 消费者` 且必须保留全部事件
  - 使用正常队列，按容量显式评估峰值流量

- `共享硬件资源/共享组件实例`
  - 只有在确实存在多任务并发访问时才引入 mutex
  - 若能通过“单任务独占外设 + 消息投递”解决，优先避免共享锁

- `大块采样数据`
  - 优先通过 DMA 缓冲区 + 事件通知 + 局部统计/摘要传递处理
  - 不默认在多个低优先级任务之间整块复制原始 DMA 缓冲

### RTOS 与中断协同规则

- 任何在 ISR 中调用 FreeRTOS API 的中断，其优先级必须满足 FreeRTOS 可调用范围，不能高于 `configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY` 所允许的级别。
- 若修改了 DMA、中断优先级、SysTick、PendSV、SVC 或 FreeRTOS 配置，应同步检查：
  - `stm_adc_pressure.ioc`
  - `Core/Src/dma.c`
  - `Core/Src/stm32f4xx_it.c`
  - `Core/Src/freertos.c`
  - `Core/Inc/FreeRTOSConfig.h`

### 后续新增任务时的默认要求

- 先说明该任务属于：
  - 实时采集
  - 控制处理
  - 显示刷新
  - 日志监控
  - 其他后台任务
- 再明确它与现有任务的优先级关系，而不是直接拍一个数字。
- 再明确它与其他线程的通信方式，并说明为什么选 notification、queue、mutex 或其他机制。
- 若新增任务会改变当前 `adc > display > monitor` 的默认关系，应在实现时同步更新本文档。

## 7. 构建产物与仓库卫生

- 不要提交本地构建产物或缓存文件
- 典型忽略对象包括：
  - `build/`
  - `build-arm/`
  - `cmake-build-*/`
  - `CMakeUserPresets.json`
- 不要把生成出来的 Makefile、构建缓存、临时文件当作源码维护

## 8. 遇到冲突时的处理原则

- 如果用户要求与这些规则冲突，以用户明确要求为准
- 如果实现某个需求需要突破当前分层边界，应先说明原因，再做最小范围的偏离
- 除非有充分理由，否则默认保持当前仓库的解耦结构不变
