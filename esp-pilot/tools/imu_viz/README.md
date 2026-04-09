# IMU Log Visualizer (Host UI)

一个无第三方依赖的上位机可视化界面：解析 ESP-IDF 日志中的 `quat=[w x y z]` / `euler_deg=[roll pitch yaw]` / `turn_deg=[...]`，并在浏览器实时显示姿态与曲线。

## 使用方式 1：直接从 `idf.py monitor` 管道输入（推荐）

在项目根目录运行：

```bash
export IDF_TOOLS_PATH=/home/csc/.espressif/tools
source /home/csc/esp/v5.4/esp-idf/export.sh

# 终端1：可视化服务（从stdin读取）
idf.py -p /dev/ttyUSB0 monitor --disable-auto-color | python tools/imu_viz/imu_viz.py --stdin --http 0.0.0.0 --port 8008
```

浏览器打开：`http://127.0.0.1:8008/`

> 如果你想保留 monitor 的交互，建议用“方式 2/3”从日志文件或串口读取。

## 使用方式 2：从日志文件读取（tail -f）

先把 monitor 输出保存到文件：

```bash
idf.py -p /dev/ttyUSB0 monitor --disable-auto-color | tee esp.log
```

再启动可视化：

```bash
python tools/imu_viz/imu_viz.py --file esp.log --follow --port 8008
```

## 使用方式 3：直接读串口（可选）

如果本机装了 `pyserial`：

```bash
python tools/imu_viz/imu_viz.py --serial /dev/ttyUSB0 --baud 115200 --port 8008
```

## ESP 端日志格式要求

默认解析类似下面的行（任意前缀都可）：

```
... a_nav_g=[0.012 -0.034 1.001] lin_a_nav_g=[0.012 -0.034 0.001] gyro_nav_dps=[0.1 -0.2 0.0] shake=[0.023 0.087 3.21] rot=[12.3 45.6 2.10] quat=[0.999900 0.001000 0.002000 0.010000] euler_deg=[48.38 -49.64 0.00] turn_deg=[408.38 -409.64 1.00] t=28.1C ts=1960379
```

只要包含 `quat=[...]` 或 `euler_deg=[...]` 就会被提取；`turn_deg=[...]` 为可选字段（存在则用于连续角曲线），`t=`/`ts=` 为可选字段。
如果存在 `lin_a_nav_g=[...]` / `gyro_nav_dps=[...]` / `shake=[rms peak hz]` / `rot=[rms peak hz]`，界面会额外显示世界(nav)坐标的运动信息。

## 关于 180°/-180° 跳变

ESP 端为了把角度限制在 `[-180, 180]` 往往会做 wrap，因此在跨越边界时会看到 `179 -> -179` 的跳变。
本可视化在前端对 roll/pitch/yaw 做了“解包裹”(unwrap)，用于曲线和 3D 姿态显示，从而变为连续角度（数值可能超过 360°）。

> 欧拉角本身存在奇点（万向锁），建议用于直观显示与小角度控制；大范围/多圈旋转请优先使用四元数或 `turn_deg`。
