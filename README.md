# Matter 空调红外控制器（ESP32-C3 + IRremoteESP8266）

用一块 ESP32-C3，把支持 [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) 协议的空调接入 **Apple Home / Google Home / Home Assistant**（Matter，本地控制，不依赖厂商云）。

本固件使用普通 **红外发射管 + 红外接收头**，不再依赖 BC7215 学习模块。可选接入 **SHT30** 温湿度传感器与 **WS2812** 温感呼吸灯，用于显示真实室温与湿度。

> **协议覆盖说明：** 只支持 IRremoteESP8266 已实现的空调协议，**不是**早期 BC7215 那种“万能学习”方案。若遥控协议不在库中，自动配对会失败；可用双击 Alt 遍历尝试库内候选协议。

---

## 功能一览

| 功能 | 说明 |
|------|------|
| Matter 空调控制 | 开关、制冷/制热、整度设定温度、风扇档位 |
| 红外收发 | RMT 驱动 TX/RX；编码/解码走 IRremoteESP8266 `IRac` |
| 空调配对 | 单击进入学习；双击 Alt 遍历协议 |
| Matter 配网 | 手机扫码加入 Home / HA（2.4 GHz Wi‑Fi） |
| 设备身份 | Manufacturer=`aidaegis`；设备名=`AC Remote`；序列号随机生成并以 MAC 后四位结尾 |
| SHT30（可选） | 真实室温写入 Thermostat `LocalTemperature`；湿度独立 Humidity Sensor 端点 |
| WS2812（可选） | 按室温绿→橙呼吸闪烁；Matter On/Off 灯端点可开关 |
| 状态 LED | GPIO8，表示配对/配网/待机等状态 |
| 按键 | BOOT 键：单击配对、双击 Alt、长按恢复出厂 |

Matter 端点（动态，需 `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=4`）：

1. **Room Air Conditioner** — 开关 / 温控 / 风扇逻辑  
2. **Fan** — 风扇档位（便于部分手机 UI 露出风速）  
3. **Humidity Sensor** — 相对湿度（有 SHT30 时更新）  
4. **On/Off Light** — WS2812 温感指示灯开关  

---

## 硬件与 BOM（画 PCB 用）

### 推荐主控

- **ESP32-C3 Super Mini** 或 **Seeed XIAO ESP32-C3**（固件默认按 C3 引脚）
- Flash ≥ 4 MB（分区表按 4 MB 编写）
- 仅支持 **2.4 GHz Wi‑Fi**（Matter over Wi‑Fi）

### 完整 GPIO 分配（固件默认，可直接画板）

| GPIO | 方向 | 功能 | PCB / 电气建议 |
|------|------|------|----------------|
| **3** | IN | 红外接收 OUT | 接 VS1838 / HX1838 等 38 kHz 解调器 OUT；模块 VCC→3V3，GND 共地 |
| **4** | OUT | 红外发射驱动 | **禁止直驱大电流 IR LED**；经 NPN / MOSFET + 限流电阻驱动 940 nm LED |
| **5** | I2C SDA | SHT30 SDA | 外挂 **4.7 kΩ → 3V3** 上拉（模块自带可省略） |
| **6** | I2C SCL | SHT30 SCL | 同上 |
| **7** | OUT | WS2812 DIN | 与灯珠共地；5 V 灯珠建议 3V3→5V 电平转换 |
| **8** | OUT | 状态 LED（**低电平点亮**） | 经电阻接到 LED 阴极，阳极→3V3（或按板载 active-low 接法） |
| **9** | IN | BOOT / 功能按键（低有效） | 按键到 GND；片内上拉，可再加 10 kΩ → 3V3 |

### 电源与保留脚

| 网络 / 引脚 | 说明 |
|-------------|------|
| **3V3 / GND** | ESP32、SHT30、红外接收头、状态 LED |
| **5V / GND**（可选） | 仅当 WS2812 用 5 V 供电；必须与 ESP **共地** |
| SHT30 **ADDR** | 接 **GND** → 地址 `0x44`（固件默认）；接 VDD → `0x45`（menuconfig 打开 `CONFIG_SHT30_I2C_ADDR_VDD`） |
| **GPIO18 / 19** | USB Serial-JTAG，外设勿占用 |
| **GPIO11–17** | C3 内部 Flash，勿引出外设 |

### 原理图示意

```
                    3V3
                     │
        ┌────────────┼────────────┬──────────────┐
        │            │            │              │
     SHT30        VS1838      状态LED         (可选上拉)
     VCC          VCC         (anode)         4.7k×2
     GND──GND     GND──GND    resistor         │
     SDA──GPIO5               │               SDA/SCL
     SCL──GPIO6               └─GPIO8 (sink)
     ADDR──GND

  ESP32-C3 ──GPIO4──► NPN/MOSFET ──► 940nm IR LED(s) + 限流电阻 ──► GND
           ──GPIO3◄── VS1838 OUT
           ──GPIO7──► WS2812 DIN   (WS2812 VCC=3V3或5V, GND共地)
           ──GPIO9◄── BOOT 按键 ── GND
```

### 红外发射注意事项

- GPIO 直驱 IR LED 射程很短，PCB 上请预留三极管/MOS 驱动与足够电流回路。  
- 可并联多颗 940 nm LED 以覆盖房间尺度。  
- 接收头朝向“人/遥控器方向”，发射管朝向空调。

### 可选传感器与指示灯

**SHT30**

- 有传感器：每约 5 s 更新 Thermostat `LocalTemperature` 与湿度端点。  
- 无传感器：空调红外/Matter 仍可用；`LocalTemperature` 回退为镜像设定温度。

**WS2812 温感呼吸灯**（周期约 2.5 s）

| 室温 | 颜色 |
|------|------|
| ≤ 18 °C | 绿色 |
| ~ 24 °C | 黄 / 琥珀 |
| ≥ 30 °C | 橙色 |

手机里会出现一盏 **On/Off 灯**，用于开关该指示灯（不影响空调控制）。

---

## 固件编译与烧录

### 环境

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/)（与所用 [ESP-Matter](https://github.com/espressif/esp-matter) 版本匹配）
- 已安装并配置好 `ESP_MATTER_PATH`、`IDF_PATH`
- 目标芯片：`esp32c3`

### 获取源码

本仓库通过 submodule 引用 IRremoteESP8266，请递归克隆：

```bash
git clone --recursive https://github.com/realDavy/bc7215_ac_matter.git
cd bc7215_ac_matter
```

若已克隆但未拉子模块：

```bash
git submodule update --init --recursive
```

### 编译烧录

```bash
idf.py set-target esp32c3
idf.py menuconfig    # 确认 MAX_DYNAMIC_ENDPOINT_COUNT=4；可改 SHT30/WS2812 引脚
idf.py build
idf.py -p <串口> erase-flash flash monitor
```

首次建议 `erase-flash`，避免旧分区/配网数据干扰。

### 可配置项

| 项 | 位置 | 默认 |
|----|------|------|
| IR TX / RX | `main/app_driver.cpp` 顶部 `IR_TX_PIN` / `IR_RX_PIN` | GPIO4 / GPIO3 |
| 状态 LED | 同文件 `SUPER_MINI_LED_GPIO` | GPIO8（active-low） |
| SHT30 SDA/SCL/ADDR | menuconfig → **SHT30 Temperature / Humidity Sensor** | 5 / 6 / 0x44 |
| WS2812 DIN | menuconfig → **WS2812 Temperature Indicator** | GPIO7 |
| 动态端点数 | `sdkconfig.defaults` → `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT` | **4** |
| Manufacturer / 设备名 | `main/CHIPProjectConfig.h`、`CMakeLists.txt` | `aidaegis` / `AC Remote` |
| 序列号 | 运行时写入 chip-factory（`serial-num`） | 随机 8 位 + MAC 后 4 位 |

依赖组件：`espressif/led_strip`（见 `main/idf_component.yml`），首次构建会从组件仓库拉取。

### 分区与版本

- 自定义分区：`partitions.csv`（含 OTA 双区、`fctry` 等）
- 工程版本号：`CMakeLists.txt` 中 `PROJECT_VER`（当前如 `2.0-irremote`）

---

## 使用说明

### 1. 与空调红外配对

1. 上电。状态灯**常亮** ≈ 出厂 / 未配好。  
2. **单击** BOOT 键 → 灯**快闪** = 进入红外学习。  
3. 用原装遥控器对准本机接收头，按任意键（建议：制冷 / 25 °C / 风速）。  
4. 协议可识别则配对完成，灯模式变化。  
5. 失败则协议可能不受支持，或改用下方 Alt 遍历。

### 2. Matter 配网（手机）

1. 手机与设备使用 **2.4 GHz** Wi‑Fi。  
2. 打开 Apple Home / Google Home / HA Companion，添加 Matter 配件。  
3. 扫描设备标签或文档中的 Matter 二维码（仓库内可参考 `img/matter_qr.png`、`img/manual_QR.png`）。  
4. DIY 固件通常会提示“未认证设备”，按指引继续即可。

配网成功后，控制器中可见的设备身份默认如下（定义见 `main/CHIPProjectConfig.h` / `main/app_main.cpp`）：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| Manufacturer（VendorName） | `aidaegis` | Matter Basic Information |
| 设备名（ProductName / NodeLabel） | `AC Remote` | 手机里显示的名称；用户可在 App 中改名 |
| SerialNumber | `RRRRRRRRMMMM` | 首次启动生成：8 位随机十六进制 + Wi‑Fi STA MAC 后 4 位；写入 `chip-factory` NVS 后固定 |

串口日志会出现 `Generated SerialNumber: ...`（首次）或 `SerialNumber: ...`（后续启动）。若需重新生成序列号，需擦除 flash / 清除 factory 区后再烧录。

### 3. 双击 Alt 协议遍历

自动解码失败时：

1. **双击**按键，按列表依次发送“制冷 / 25 °C”测试帧。  
2. 空调有反应（如滴一声）后，在手机上发任意 Matter 命令（如改温度）以**确认当前协议**。  
3. 灯闪次数对应协议序号；试完一轮仍无结果则回到未配对。

支持协议列表见：[IRremoteESP8266 SupportedProtocols](https://github.com/crankyoldgit/IRremoteESP8266/blob/master/SupportedProtocols.md)（空调 / `IRac::isProtocolSupported`）。

### 4. 恢复出厂

**长按**按键约 **5 秒**（灯快闪）后松开：清除红外配对与 Matter 配网数据。

---

## 状态 LED 含义（GPIO8）

| 灯效 | 含义 |
|------|------|
| 常亮 | 出厂 / 尚未配置完成 |
| 常灭 | 未上电，或 Alt 遍历进行中 |
| 快闪 | 等待红外学习信号 |
| 慢闪 | 正在建立网络连接 |
| 每秒闪 1 次 | 网络已配好，空调尚未红外配对 |
| 每秒闪 2 次 | 空调已配对，等待手机 Matter 连接 / 订阅 |
| 约每 3 秒闪一下 | 配对与连接完成，待机 |

> 状态 LED 与 WS2812 温感灯相互独立：前者表示系统/配对状态，后者表示室温且可被 Matter 开关。

---

## 软件架构

```
main/
  app_main.cpp          Matter 节点、端点创建、SHT30/WS2812 启动
  app_driver.cpp        红外配对/控制、状态 LED、按键、属性写回
  drivers/sht30.*       SHT30 I2C 单次测量（CRC），FreeRTOS 轮询任务
  drivers/ws2812_temp_light.*
                        WS2812 呼吸灯（led_strip + RMT）
components/ir_ac/       RMT 收发 + IRac 封装
deps/IRremoteESP8266/   git submodule（UNIT_TEST + SWIGLIB，时序由软件生成再经 RMT 发出）
```

**RMT 占用（ESP32-C3）：** 红外 TX 1 路 + 红外 RX 1 路 + WS2812 TX 1 路，资源足够。

---

## Matter 能力与限制

**已支持（与常见手机 UI 对齐）：**

- 开关、制冷 / 制热  
- 目标温度：按 **整度 °C** 对齐（0.5 °C 类输入会四舍五入到整度）  
- 风扇：低 / 中 / 高（由百分比映射）  
- 有 SHT30：当前室温 + 湿度  
- WS2812：独立 On/Off  

**限制：**

- 无独立“自动 / 除湿 / 仅通风”等完整官方声明时，部分控制器仍可能下发；固件在收到时会尽量映射到红外模式。  
- 半度温控、细粒度扫风摆叶等 Matter HVAC 表达仍弱于原装遥控。  
- 非 IRremoteESP8266 协议的空调无法通过本固件配对。

---

## 外壳与放置

仓库提供 3D 外壳与装配说明：

- 模型：`3d/*.step`（Super Mini / XIAO 底壳等）  
- 说明：[`docs/casing.md`](docs/casing.md)  
- 配图：`img/`

放置原则：靠近空调；发射管朝空调；接收头朝向使用遥控的一侧。

---

## 故障排查

| 现象 | 排查 |
|------|------|
| 编译缺 `IRremoteESP8266` | `git submodule update --init --recursive` |
| 端点创建失败 / 湿度或灯不出现 | 确认 `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=4` 后重新 `fullclean` + 编译 |
| 红外配对无反应 | 检查 GPIO3 接收头接线与朝向；确认遥控对准接收头 |
| 有配对但空调不动 | 检查 GPIO4 驱动电路与发射管方向/电流；试 Alt 遍历换协议 |
| 日志 `SHT30 not available` | 检查 SDA/SCL/ADDR/供电与上拉；无传感器属正常降级 |
| WS2812 不亮 | 查 GPIO7、共地、5 V 电平时序；Matter 灯端点是否被关掉 |
| Matter 搜不到 | 必须 2.4 GHz；尝试恢复出厂后重新配网；看串口日志 |

串口监视：

```bash
idf.py -p <串口> monitor
```

关注日志标签：`app_main`、`app_driver`、`ir_ac` / `rmt_ir`、`sht30`、`ws2812_temp`。

---

## 目录结构（简）

```
├── main/                 应用与传感器/指示灯驱动
├── components/ir_ac/     红外 RMT + IRac
├── deps/IRremoteESP8266/ 子模块
├── docs/                 外壳说明、Web 安装页等
├── 3d/                   外壳 STEP
├── img/                  装配与 Matter 二维码等图片
├── partitions.csv
├── sdkconfig.defaults
└── README.md
```

---

## 许可证

- 本仓库工程代码：见根目录 [`LICENSE`](LICENSE)（MIT）。  
- [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266)：LGPL-2.1，请遵守其条款。  
- `espressif/led_strip` 等组件：遵循各自 SPDX / 组件许可证。
