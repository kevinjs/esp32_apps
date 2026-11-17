# ride-gmeter · 骑行加速度指示器

基于板载 QMI8658 六轴 IMU 的骑行加速度指示器。**不依赖手机、网络与地图**。

当前为**阶段二**：屏幕气泡界面 + 串口数据流。阶段一（纯串口探测）已完成，数据质量验证通过。

| 项 | 内容 |
| --- | --- |
| 硬件 | Waveshare ESP32-S3-Touch-AMOLED-1.75C（ESP32-S3R8 / 8MB PSRAM / 32MB Flash / 466×466 AMOLED） |
| 传感器 | QMI8658，I2C 地址 `0x6B`，与触摸/音频/PMIC 共用总线（SDA=GPIO15，SCL=GPIO14） |
| 框架 | ESP-IDF v5.5+（v6.x 亦通过编译），组件：`waveshare/esp32_s3_touch_amoled_1_75c` + `waveshare/qmi8658` |
| 状态 | 阶段二可用（气泡界面 + 串口） |

## 为什么主传感器是加速度计

“加速度指示器”的物理量是加速度。加速度计给出的是「重力 + 运动」，扣除重力后才是要显示的线性加速度。陀螺仪在本项目里用于后续的压弯角速度与路面粗糙度，不是主力。

## 构建

```powershell
cd d:\code\esp32_learning\esp32_apps\ride-gmeter
idf.py set-target esp32s3
idf.py build

# selector 落地前：写到 factory 槽（otadata 为空时 bootloader 就启动 factory）
..\tools\flash_app.ps1 -Bin build\ride-gmeter.bin -Slot selector -Port COMx

# selector 落地后：改写到运行槽 ota_0，由 selector 选择启动
# ..\tools\flash_app.ps1 -Bin build\ride-gmeter.bin -Slot 0 -Port COMx

idf.py -p COMx monitor
```

控制台走 **USB Serial JTAG**（板载 Type-C 直连 ESP32-S3 原生 USB），`monitor` 的波特率无意义，直接 `idf.py -p COMx monitor` 即可。

分区表已迁移到统一表 `../partitions/fleet.csv`（副本 `partitions.csv`）：`factory` 放 selector、`ota_0` 放本应用、`storage` 放镜像库。`nvs` 的偏移和大小未变，标定数据不会因为换分区表而丢失。

## 串口输出

上电后先静止 **2 秒**完成零偏标定，然后输出两条流：

```
# t_ms,ax,ay,az,|a|,gx,gy,gz                     ← 表头
12345,0.021,-0.004,0.113,0.115,0.4,-0.2,0.1      ← 50Hz，ax/ay/az 已去重力（m/s²）
# g=[0.02 -0.11 9.79] tilt pitch=-0.1 roll=-0.6   ← 1Hz，重力向量与姿态角（'#' 为注释）
```

- `ax/ay/az`：**线性加速度**（已去重力、5Hz 低通、含死区）
- `gx/gy/gz`：角速度（°/s）
- `#` 开头的行是注释，串口绘图工具通常会忽略

按 **BOOT 键（GPIO0）** 可随时重新标定（保持静止 2 秒）。

## 屏幕界面（阶段二）

466×466 圆形屏幕由外到内的布局：

| 位置 | 内容 |
| --- | --- |
| 最顶部 | `ROAD` + 小进度条：垂直方向加速度平滑值（路面颠簸强度），青 → 黄 → 红 |
| 顶部 | `MAX 4.2`：本次上电以来的加速度峰值 |
| 顶部下方 | `TURN +12 dps`：绕 Z 轴角速度（已去零偏，正值右转） |
| 中心 | 气泡：偏移方向 = 合力方向，颜色随强度 绿 → 黄 → 红（红需持续 200ms） |
| 中心下方 | 大号数字：当前加速度模长（m/s²） |
| 底部 | 双极性圆弧：`ACCEL` 绿色向右、`BRAKE` 红色向左，各以 6 点钟方向为 0 点 |

### 抗干扰设计

| 机制 | 参数 | 作用 |
| --- | --- | --- |
| 姿态门控 | `GMETER_GYRO_GATE_DPS` = 120 | 合角速度超阈值时把送显加速度在 ~100ms 内衰减到 0，抑制手持晃动/翻转产生的假加速度 |
| 状态阈值 | `GMETER_UI_STATE_MPS2` = 1.2 | 压掉踩踏（1–1.5 Hz，0.3–1 m/s²）造成的弧条误闪 |
| 红色需持续 | `GMETER_UI_RED_MPS2` = 5.0 / `RED_HOLD_MS` = 200 | 过坑的单次脉冲不会立刻变红，避免随机闪红 |

- 气泡位置 = `横向(加速度Y) × 23 px`、`纵向(加速度X) × -23 px`，满量程 6 m/s² 对应 140 px。
- **装到车把上如果方向反了**，改 `gmeter_config.h` 里的 `GMETER_UI_LONG_SIGN` / `GMETER_UI_LAT_SIGN`（或交换 `GMETER_UI_AXIS_LONG/LAT`）。
- 按 **BOOT 键**重新标定，屏幕会先显示 `CALIBRATING`，完成后变回 `READY`。
- 静止时数字与气泡都几乎不动，这是死区 + 低通在起作用，属正常现象。

## 采集工具

```powershell
cd tools
.\capture.ps1 -Port COM3 -Seconds 15
```

3 秒倒计时后开始采集，屏幕上打印各轴 min/max/avg、活动样本数与峰值时刻，原始数据存成 `capture_<时间戳>.csv`（可直接用 Excel 或串口绘图工具查看）。

## 验证四组动作

| 动作 | 预期 |
| --- | --- |
| 静止放置 | `ax/ay/az` 均接近 0，`|a|` < 0.15（死区内），`tilt` 稳定不漂 |
| 水平直线加速 | 前进方向出现 1–3 m/s²，方向与运动一致 |
| 急刹 | 前进方向出现负值，2–5 m/s² |
| 压弯 | 侧向出现持续 1–3 m/s²；`roll` 明显变化 |
| 放在车把上骑行 | 数值抖动幅度 < 0.3 m/s²（否则降低 `GMETER_LP_CUTOFF_HZ`） |

## 可调参数

全部集中在 `main/gmeter_config.h`：

| 宏 | 默认 | 说明 |
| --- | --- | --- |
| `GMETER_SAMPLE_PERIOD_MS` | 10 | 100Hz 采样 |
| `GMETER_GRAVITY_TAU_S` | 0.5 | 重力估计时间常数；太小会吃掉真实加减速 |
| `GMETER_LP_CUTOFF_HZ` | 5.0 | 线性加速度低通；车把振动大可降到 3Hz |
| `GMETER_DEADBAND_MPS2` | 0.15 | 死区，消除静止抖动 |
| `GMETER_CALIB_SAMPLES` | 200 | 标定采样数（×10ms = 2s） |

## 实现要点

- **去重力用互补形式**（长时间常数低通估计重力），不是对原始信号做高通：高通会带来相位失真和漂移。
- **两级一阶低通级联**构成二阶低通，比单级更陡且无相位突跳。
- **标定只修正沿重力方向的零偏**：静止时垂直于重力方向的零偏不可观测，数学上无法标定（`bias = mean - normalize(mean) × 9.80665`）。
- **不做速度积分**：`v = ∫a dt` 在消费级 IMU 上几分钟就会漂到几十 km/h。
- **`waveshare/qmi8658` v2.0.2 的一个坑**：头文件声明了 `qmi8658_read_accel_mps2` / `qmi8658_read_gyro_dps`，但 `.c` 里没有实现，直接调用会 `undefined reference`。统一使用 `qmi8658_read_sensor_data()`，单位在 init 时设好即可。
- **`qmi8658.h` 无条件 `#define M_PI`**，与 IDF v6（picolibc）的 `math.h` 冲突，include 前需 `#undef M_PI`（官方示例 04 也这么做）。

## 已知限制

1. 没有 UI，只能看串口。
2. **安装朝向（yaw）未知**：重力只能确定垂直方向，无法确定车头方向，需在 `gmeter_config.h` 里配好轴映射宏。
3. 标定需保持静止 2 秒；标定期间若搬动设备，陀螺零偏会不准（日志里 `|g|` 偏离 9.81 会给出警告）。
4. 界面为 30fps 全量刷新控件，未做脏区域优化；后续若加动画/粒子需关注帧率。

## 后续阶段

1. 轴映射做成屏幕上的设置项（当前需改宏并重编译）
2. 急刹提示（边框闪红）、压弯倾角、踏频估算（纵向加速度 1–1.5 Hz 带通/FFT）、路面颠簸指数
3. 续航策略（常亮 2 小时以上需要调亮度/降采样）
