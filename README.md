# Matter 空调红外控制器（ESP32-S3 + 1.28″ 圆屏）

用 **ESP32-S3-WROOM-1-N16**，把支持 [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) 协议的空调接入 **Apple Home / Google Home / Home Assistant**（Matter，本地控制，不依赖厂商云）。

本固件使用普通 **红外发射管 + 红外接收头**，不再依赖 BC7215 学习模块。默认搭配 **1.28″ 圆形触摸屏（GC9A01 + IT7259）**、可选 **SHT30** 温湿度与 **WS2812** 氛围灯。

> **协议覆盖说明：** 只支持 IRremoteESP8266 已实现的空调协议，**不是**早期 BC7215 那种“万能学习”方案。若遥控协议不在库中，自动学习会失败；可用 BOOT **双击** Alt 遍历尝试库内候选协议。

---

## 功能一览

| 功能 | 说明 |
|------|------|
| 圆屏 UI（LVGL） | 默认中文，右上角可切英文；灯光页再左滑进入**组件设置**（Home 组合/分开显示） |
| Matter 配网 | 未配网时屏上显示动态 `MT:...` 二维码 + 数字配对码（与串口一致） |
| 红外学习 | 配网后屏上「开始学习」；学习中 WS2812 **黄色呼吸**；BOOT **单击不再学码** |
| Matter 空调控制 | 开关、制冷/制热、整度设定温度；风速固定为自动（不在 Home 显示风速控件） |
| 灯光页 | 空调页左滑进入；夜间关闭 / 手动 / 温感呼吸（默认）/ 纯色 / 彩虹 / 呼吸白 |
| Matter 灯光 | **On/Off + 亮度（LevelControl）**；氛围模式仅屏上选（方案 A） |
| 设备身份 | Manufacturer=`aidaegis`；设备名=`AC Remote`；序列号随机生成并以 MAC 后四位结尾 |
| SHT30（可选） | 真实室温写入 Thermostat `LocalTemperature`；湿度独立 Humidity Sensor 端点 |
| 状态 LED | GPIO11，表示配对/配网/待机等状态 |
| BOOT 键 | **双击** Alt 遍历协议；**长按 ~5s** 恢复出厂 |

Matter 端点（动态，需 `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT≥8`；Root 也占槽位）：

屏上 **组件设置**（空调页左滑→灯光→再左滑）可选 Home 显示方式（默认**组合**）：

| 模式 | 拓扑 | Home 表现 |
|------|------|-----------|
| **组合显示** | Root + RAC / Humidity / Light（扁平端点） | 一个配件，内含多项服务 |
| **分开显示** | Root + Aggregator + 三个 Bridged Node | 空调 / 湿度 / 灯为独立配件 |

切换模式会写入 NVS 并重启；之后请在 Home 中**删除旧配件并重新配对**。  
风速不通过 Matter 暴露；本机发出的红外指令固定为 **自动风速**。

---

## 硬件与 BOM（画 PCB 用）

### 推荐主控

- **ESP32-S3-WROOM-1-N16**（固件默认按此模块引脚，见 `main/board_pins.h`）
- Flash：**16 MB**（分区表按 16 MB 编写）
- 仅支持 **2.4 GHz Wi‑Fi**（Matter over Wi‑Fi）

### 显示与触摸

| 部件 | 说明 |
|------|------|
| 面板 | 1.28″ 圆形 TFT，240×240，65K 色 |
| 显示驱动 | **GC9A01**，4-line SPI |
| 触摸芯片 | **IT7259**（规格图常只标 TP_SDA/SCL/INT/RST，无写型号时按 IT7259） |
| 触摸总线 | I2C，地址默认 **`0x46`**，与 SHT30 共用 SDA/SCL |
| 背光 | LEDA/LEDK，经 MOS + GPIO PWM（默认 GPIO48） |

### 完整 GPIO 分配（固件默认，可直接画板）

| GPIO | 方向 | 功能 | PCB / 电气建议 |
|------|------|------|----------------|
| **4** | IN | 红外接收 OUT | 接 VS1838 / HX1838 等 38 kHz 解调器 OUT；模块 VCC→3V3，GND 共地 |
| **5** | OUT | 红外发射驱动 | **禁止直驱大电流 IR LED**；经 NPN / MOSFET + 限流电阻驱动 940 nm LED |
| **8** | I2C SDA | SHT30 + IT7259 SDA | 外挂 **4.7 kΩ → 3V3** 上拉（模块自带可省略） |
| **9** | I2C SCL | SHT30 + IT7259 SCL | 同上 |
| **10** | OUT | WS2812 DIN | 与灯珠共地；5 V 灯珠建议 3V3→5V 电平转换 |
| **11** | OUT | 状态 LED（**低电平点亮**） | 经电阻接到 LED 阴极，阳极→3V3 |
| **0** | IN | BOOT / 功能按键（低有效） | 按键到 GND；片内上拉，可再加 10 kΩ → 3V3 |
| **12** | OUT | LCD SCLK | GC9A01 SPI 时钟 |
| **13** | OUT | LCD MOSI | GC9A01 SPI 数据 |
| **14** | OUT | LCD CS | 片选 |
| **21** | OUT | LCD DC | 数据/命令 |
| **47** | OUT | LCD RST | 复位 |
| **48** | OUT | LCD BL | 背光 PWM（经 MOS 推 LEDA/LEDK） |
| **15** | IN | TP INT | IT7259 中断 |
| **16** | OUT | TP RST | IT7259 复位 |

### 电源与保留脚

| 网络 / 引脚 | 说明 |
|-------------|------|
| **3V3 / GND** | ESP32-S3、SHT30、红外接收头、状态 LED、触摸/屏逻辑电 |
| **5V / GND**（可选） | 仅当 WS2812 / 背光需要 5 V；必须与 ESP **共地** |
| SHT30 **ADDR** | 接 **GND** → 地址 `0x44`（固件默认）；接 VDD → `0x45`（menuconfig 打开 `CONFIG_SHT30_I2C_ADDR_VDD`） |
| **GPIO19 / 20** | USB，外设勿占用 |
| **GPIO26–32** | S3 模组内部 Flash/PSRAM 相关，勿引出外设 |
| 引脚改线 | 可改，但请同步 `main/board_pins.h` / menuconfig，并避开上表保留脚 |

### 原理图示意

```
                    3V3
                     │
        ┌────────────┼────────────┬──────────────┐
        │            │            │              │
     SHT30        VS1838      状态LED         (可选上拉)
     VCC          VCC         (anode)         4.7k×2
     GND──GND     GND──GND    resistor         │
     SDA──GPIO8               │               SDA/SCL
     SCL──GPIO9               └─GPIO11 (sink)
     ADDR──GND

  ESP32-S3 ──GPIO5──► NPN/MOSFET ──► 940nm IR LED(s) + 限流电阻 ──► GND
           ──GPIO4◄── VS1838 OUT
           ──GPIO10─► WS2812 DIN
           ──GPIO0 ◄── BOOT 按键 ── GND

  SPI LCD GC9A01:
    SCLK=12  MOSI=13  CS=14  DC=21  RST=47  BL=48

  Touch IT7259 (I2C 0x46, 与 SHT30 共总线):
    SDA=8  SCL=9  INT=15  RST=16
```

### 红外发射注意事项

- GPIO 直驱 IR LED 射程很短，PCB 上请预留三极管/MOS 驱动与足够电流回路。
- 可并联多颗 940 nm LED 以覆盖房间尺度。
- 接收头朝向“人/遥控器方向”，发射管朝向空调。

### 可选传感器与氛围灯

**SHT30**

- 有传感器：每约 5 s 更新 Thermostat `LocalTemperature` 与湿度端点。
- 无传感器：空调红外/Matter 仍可用；`LocalTemperature` 回退为镜像设定温度。

**WS2812 氛围灯**（Matter = 开关 + 亮度；模式只在屏上选）

| 屏上模式 | 行为 |
|----------|------|
| 夜间关闭 | 灯灭 |
| 手动亮度 | 白光，跟 Matter/屏上亮度 |
| 温感呼吸（默认） | 室温绿→橙呼吸（约 2.5 s） |
| 纯色 | 按温感色固定点亮 |
| 彩虹 | 色相循环 |
| 呼吸白 | 白光呼吸 |
| 红外学习中 | 强制**黄色呼吸**（临时） |

---

## 固件编译与烧录

### 环境（一次性装好，避免 `idf.py` / `gn` / 组件仓库问题）

依赖：

- [ESP-IDF](https://docs.espressif.com/projects/esp-idf/)（与所用 [ESP-Matter](https://github.com/espressif/esp-matter) 版本匹配；本工程日志常见 IDF **v5.5.3**）
- 已 clone 的 [ESP-Matter](https://github.com/espressif/esp-matter)（含 `connectedhomeip` 子模块）
- 目标芯片：`esp32s3`

本工程常见编译失败与根因：

| 报错 | 原因 | 处理 |
|------|------|------|
| `idf.py: command not found` | 未 source IDF | `. $IDF_PATH/export.sh` |
| `Cannot establish a connection…components-file.espressif.com` | 国内拉组件慢/失败 | `export IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn`（只改存储镜像） |
| `Access forbidden … components-file.espressif.cn/api` **403** | 把 `.cn` 误设成了 `IDF_COMPONENT_REGISTRY_URL` | **取消**该变量；改用上面的 `IDF_COMPONENT_STORAGE_URL` |
| `The 'gn' command was not found` | 未安装 / 未 source ESP-Matter | 在已 source IDF 后执行 `esp-matter/install.sh`，再 `. $ESP_MATTER_PATH/export.sh` |

#### 一次安装（本机只需成功跑一遍）

路径按你机器修改（下面与 `~/esp-adf/esp-idf/esp-matter/examples/ac_matter` 布局一致）：

```bash
export IDF_PATH=~/esp-adf/esp-idf
export ESP_MATTER_PATH=~/esp-adf/esp-idf/esp-matter
# 国内：只镜像组件文件 CDN；注册表 API 仍用默认 components.espressif.com
unset IDF_COMPONENT_REGISTRY_URL
export IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn

# 1) IDF 工具链（若早已 install 过可跳过）
cd "$IDF_PATH"
./install.sh esp32s3
. ./export.sh

# 2) Matter 主机工具（安装 gn 等；必须先 source 过 IDF）
cd "$ESP_MATTER_PATH"
./install.sh
. ./export.sh

# 3) 自检
which idf.py && which gn && gn --version
```

仓库内也提供封装脚本（路径相对本仓库根目录）：

```bash
# 在 ac_matter 仓库根目录
bash scripts/setup_build_env.sh --install
```

#### 每次新开终端（编译前必做）

```bash
export IDF_PATH=~/esp-adf/esp-idf
export ESP_MATTER_PATH=~/esp-adf/esp-idf/esp-matter
unset IDF_COMPONENT_REGISTRY_URL
export IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn
. "$IDF_PATH/export.sh"
. "$ESP_MATTER_PATH/export.sh"
# 或：. /path/to/ac_matter/scripts/setup_build_env.sh
```

建议写入 `~/.bashrc`（永久生效路径与存储镜像）：

```bash
export IDF_PATH=~/esp-adf/esp-idf
export ESP_MATTER_PATH=~/esp-adf/esp-idf/esp-matter
unset IDF_COMPONENT_REGISTRY_URL
export IDF_COMPONENT_STORAGE_URL=https://components-file.espressif.cn
```

### 获取源码

本仓库通过 submodule 引用 IRremoteESP8266，请递归克隆：

```bash
git clone --recursive https://github.com/realDavy/ac_matter.git
cd ac_matter
```

若已克隆但未拉子模块：

```bash
git submodule update --init --recursive
```

若工程放在 `esp-matter/examples/ac_matter` 下，确保已 `git pull` 到含 `MAX_DYNAMIC_ENDPOINT_COUNT=8` 的最新 `main`。

### 编译烧录

```bash
# 先完成上一节环境 source，确认 which gn / which idf.py 都有输出
idf.py set-target esp32s3
idf.py menuconfig    # 确认 MAX_DYNAMIC_ENDPOINT_COUNT≥8；可改 SHT30/WS2812 引脚
idf.py build
idf.py -p <串口> erase-flash flash monitor
```

首次建议 `erase-flash`，避免旧分区/配网数据干扰。

组件拉失败时清缓存再试：

```bash
rm -rf build managed_components dependencies.lock
idf.py set-target esp32s3
```

### 可配置项

| 项 | 位置 | 默认 |
|----|------|------|
| 板级引脚总表 | `main/board_pins.h` | 见上表 |
| IR TX / RX | `board_pins.h` → `BOARD_IR_TX_GPIO` / `BOARD_IR_RX_GPIO` | GPIO5 / GPIO4 |
| 状态 LED | `BOARD_STATUS_LED_GPIO` | GPIO11（active-low） |
| SHT30 / 触摸 I2C | menuconfig → **SHT30…** 或 `board_pins.h` | SDA=8 / SCL=9 / ADDR=0x44 |
| WS2812 DIN | menuconfig → **WS2812…** | GPIO10 |
| 动态端点数 | `sdkconfig.defaults` → `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT` | **8** |
| Manufacturer / 设备名 | `main/CHIPProjectConfig.h`、`CMakeLists.txt` | `aidaegis` / `AC Remote` |
| 序列号 | 运行时写入 chip-factory（`serial-num`） | 随机 8 位 + MAC 后 4 位 |
| 配对码 / Discriminator | 运行时写入 chip-factory（`pin-code` 等） | 首次启动每台设备唯一生成 |
| Flash / 分区 | `sdkconfig.defaults`、`partitions.csv` | 16 MB，OTA 双区各约 6 MB |

依赖组件（见 `main/idf_component.yml`）：`espressif/led_strip`、`espressif/esp_lcd_gc9a01`、`espressif/esp_lvgl_port`、`lvgl/lvgl`。首次构建会从组件仓库拉取（国内请用上面的 `.cn` 镜像）。

### 分区与版本

- 自定义分区：`partitions.csv`（16 MB：OTA 双区、`fctry` 等）
- 工程版本号：`CMakeLists.txt` 中 `PROJECT_VER`（当前如 `3.0-s3-ui`）

---

## 用户说明书（PDF）

面向最终用户的操作说明（配网、红外学习、屏上操作、按键与常见问题）：

- [`docs/aidaegis_ac_remote_user_manual.pdf`](docs/aidaegis_ac_remote_user_manual.pdf)

重新生成：

```bash
pip install fpdf2 fonttools   # 系统需安装 fonts-wqy-microhei
python3 scripts/gen_user_manual_pdf.py
```

## 使用说明

### 1. Matter 配网（手机 / 屏上扫码）

1. 手机与设备使用 **2.4 GHz** Wi‑Fi。
2. 设备未入网时，屏上显示 Matter 二维码与数字配对码；内容由固件当前 `MT:...` **动态生成**，与串口 onboarding 打印一致。手机 BLE 连上后屏会改为显示「配对中...」并释放 LVGL 内存完成配对，配网成功后恢复完整界面。
3. 打开 Apple Home / Google Home / HA Companion，添加 Matter 配件并扫屏上码（或输入数字码）。
4. DIY 固件通常会提示“未认证设备”，按指引继续即可。

配网成功后，控制器中可见的设备身份默认如下：

| 字段 | 默认值 | 说明 |
|------|--------|------|
| Manufacturer（VendorName） | `aidaegis` | Matter Basic Information |
| 设备名（ProductName / NodeLabel） | `AC Remote` | 手机里显示的名称；用户可在 App 中改名 |
| SerialNumber | `RRRRRRRRMMMM` | 首次启动生成：8 位随机十六进制 + Wi‑Fi STA MAC 后 4 位；写入 `chip-factory` NVS 后固定 |
| Setup Passcode / Discriminator | 每台设备随机 | 通过 `CONFIG_CUSTOM_COMMISSIONABLE_DATA_PROVIDER` + `unique_commissionable_data_provider` 在首次启动写入 `chip-factory`（含 SPAKE2+ salt）；决定屏上/串口 QR 与数字配对码，**各设备唯一**。不再使用共享测试码 `20202021` / `34970112332`。 |

串口日志会出现 `Generated SerialNumber: ...`（首次）或 `SerialNumber: ...`（后续启动），以及 `unique_cdp: Generated unique commissionable data: passcode=... discriminator=...`（首次）或 `Loaded commissionable data: ...`（后续）。若仍看到 `34970112332`，说明仍在用 Test provider：请 `idf.py fullclean` 后按本仓库 `sdkconfig.defaults` 重新配置并烧录；若需重新生成配对码，擦除 flash / factory 分区后再烧（量产请用 `esp-matter-mfg-tool` 预置工厂分区）。

### 2. 与空调红外学习

1. 完成 Matter 配网后，若尚未学码，屏进入 **红外学习** 页。
2. 点击 **「开始学习」** → WS2812 黄呼吸；用原装遥控器对准本机接收头，按任意键（建议：制冷 / 25 °C / 风速）。
3. 协议可识别则学习完成，进入空调控制页。
4. 失败则协议可能不受支持，或改用下方 Alt 遍历。

> BOOT **单击不再进入学习**；学习入口在触摸屏。

### 3. 屏上空调 / 灯光

- **空调页**：开启/关闭、降温、升温；与 Matter 状态双向同步。
- **左滑**进入灯光页；右滑返回。
- 灯光模式（仅本地）：夜间关闭、手动亮度、温感呼吸、纯色、彩虹、呼吸白。
- 亮度滑条对应 Matter **LevelControl**；手机 Home 仍可开关灯与调亮度。
- 右上角 **EN / 中文** 切换界面语言（默认中文）。

### 4. 双击 Alt 协议遍历

自动解码失败时：

1. **双击** BOOT，按列表依次发送“制冷 / 25 °C”测试帧。
2. 空调有反应（如滴一声）后，在手机上发任意 Matter 命令（如改温度）以**确认当前协议**。
3. 灯闪次数对应协议序号；试完一轮仍无结果则回到未配对。

支持协议列表见：[IRremoteESP8266 SupportedProtocols](https://github.com/crankyoldgit/IRremoteESP8266/blob/master/SupportedProtocols.md)（空调 / `IRac::isProtocolSupported`）。

### 5. 恢复出厂

**长按** BOOT 约 **5 秒**（灯快闪）后松开：清除红外配对与 Matter 配网数据。

---

## 状态 LED 含义（GPIO11）

| 灯效 | 含义 |
|------|------|
| 常亮 | 出厂 / 尚未配置完成 |
| 常灭 | 未上电，或 Alt 遍历进行中 |
| 快闪 | 等待红外学习信号 / 恢复出厂提示 |
| 慢闪 | 正在建立网络连接 |
| 每秒闪 1 次 | 网络已配好，空调尚未红外配对 |
| 每秒闪 2 次 | 空调已配对，等待手机 Matter 连接 / 订阅 |
| 约每 3 秒闪一下 | 配对与连接完成，待机 |

> 状态 LED 与 WS2812 氛围灯相互独立：前者表示系统/配对状态，后者表示氛围效果且可被 Matter 开关/调亮度。

---

## 软件架构

```
main/
  board_pins.h          ESP32-S3 默认引脚
  board_i2c.*           共享 I2C（SHT30 + IT7259）
  app_main.cpp          Matter 节点、端点创建、显示/传感器启动
  app_driver.cpp        红外配对/控制、状态 LED、按键、属性写回、UI 回调
  drivers/
    sht30.*             SHT30 I2C 测量
    it7259.*            触摸
    display_gc9a01.*    GC9A01 + LVGL port
    ws2812_temp_light.* 氛围灯模式 + Matter 亮度
  ui/                   LVGL 界面、中文字体子集、QR 编码
components/ir_ac/       RMT 收发 + IRac 封装
deps/IRremoteESP8266/   git submodule（UNIT_TEST + SWIGLIB）
```

**RMT 占用（ESP32-S3）：** 红外 TX 1 路 + 红外 RX 1 路 + WS2812 TX 1 路，资源足够。

---

## Matter 能力与限制

**已支持（与常见手机 UI 对齐）：**

- 空调：开关、制冷 / 制热、目标温度按 **整度 °C** 对齐、风扇低/中/高
- 有 SHT30：当前室温 + 湿度
- 氛围灯：On/Off + LevelControl 亮度

**屏上有、手机 Home 一般没有：**

- 彩虹 / 呼吸白 / 温感呼吸 / 纯色 / 夜间关闭等氛围模式（方案 A：模式只在本地选）

**限制：**

- 无独立“自动 / 除湿 / 仅通风”等完整官方声明时，部分控制器仍可能下发；固件在收到时会尽量映射到红外模式。
- 半度温控、细粒度扫风摆叶等 Matter HVAC 表达仍弱于原装遥控。
- 非 IRremoteESP8266 协议的空调无法通过本固件学习。

---

## 故障排查

| 现象 | 排查 |
|------|------|
| 编译缺 `IRremoteESP8266` | `git submodule update --init --recursive` |
| 目标芯片不对 | 必须 `idf.py set-target esp32s3`（不再支持默认 C3） |
| 端点创建失败 / 湿度或灯不出现 / 启动 abort 重启 | 确认 `CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT≥8` 后重新 `fullclean` + 编译；串口若见 bridged/aggregator 创建失败即为此项 |
| 想让空调和灯分开/合并 | 屏上左滑到灯光再左滑进入**组件设置**，选「分开显示」或「组合显示」→ 应用并重启 → Home 中删除旧配件后重新配对 |
| 看不到 Matter 配对码 | 设备必须稳定跑过 `esp_matter::start`；配网页在圆屏，串口会打 `UI Matter code:` / CHIP onboarding QR |
| 屏上二维码扫不上 | 配网页 QR 约 175px（黑底白码）；请正对圆屏、关闭强光反射；也可直接输入屏上数字配对码 |
| 屏上文字花屏/色带 | 确认固件启用了 `CONFIG_LV_COLOR_16_SWAP`（GC9A01 SPI 必需）；工程已用 CMake 强制 `-DLV_COLOR_16_SWAP=1`。若仍色带：`rm sdkconfig && idf.py fullclean && idf.py build flash` |
| 屏上文字缺笔画/发虚 | 同上（色带破坏抗锯齿）；并确认已拉取含 bpp8 文泉驿字库的最新 `main` |
| 屏上文字左右颠倒（EN 变成 NE） | 已由 `BOARD_LCD_MIRROR_X` 校正本模组扫描方向；确认已拉取最新 `main` 并重刷 |
| 屏不亮 / 花屏 | 查 SPI 脚 12/13/14/21/47、背光 48、供电与 `board_pins.h`；若日志有 LVGL buffer OOM，固件会降级单缓冲重试 |
| 触摸无反应 | 查 I2C 8/9、INT/RST 15/16、地址 `0x46`；与 SHT30 共总线时确认上拉；日志看 `it7259` probe。固件需按 ITE 手册解析 Query bit7/坐标打包 |
| 红外学习无反应 | 查 GPIO4 接收头接线与朝向；确认已点屏上「开始学习」 |
| 有学习但空调不动 | 查 GPIO5 驱动电路与发射管方向/电流；试 Alt 遍历换协议 |
| 日志 `SHT30 not available` | 检查 SDA/SCL/ADDR/供电与上拉；无传感器属正常降级 |
| WS2812 不亮 | 查 GPIO10、共地、5 V 电平时序；Matter 灯端点是否被关掉 / 亮度是否过低 |
| 屏上 QR 与串口不一致 | 不应发生；两者都来自 `GetQRCode`；若异常看 `ui` / CHIP 日志 |
| Matter 搜不到 | 必须 2.4 GHz；尝试恢复出厂后重新配网；看串口日志 |

串口监视：

```bash
idf.py -p <串口> monitor
```

关注日志标签：`app_main`、`app_driver`、`ir_ac` / `rmt_ir`、`sht30`、`ws2812_temp`、`display`、`ui`、`it7259`。

---

## 目录结构（简）

```
├── main/                 应用、板级引脚、显示/触摸/传感器、UI
├── components/ir_ac/     红外 RMT + IRac
├── deps/IRremoteESP8266/ 子模块
├── docs/                 外壳说明、Web 安装页等
├── 3d/                   外壳 STEP（若仍按旧 C3 板型，需自行改）
├── img/                  装配等图片
├── partitions.csv        16 MB 分区
├── sdkconfig.defaults
└── README.md
```

---

## 许可证

- 本仓库工程代码：见根目录 [`LICENSE`](LICENSE)（MIT）。
- [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266)：LGPL-2.1，请遵守其条款。
- `espressif/led_strip`、`esp_lcd_gc9a01`、`esp_lvgl_port`、`lvgl` 等组件：遵循各自 SPDX / 组件许可证。
