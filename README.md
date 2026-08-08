# Matter 空调红外控制器（ESP32-S3 + 1.28″ 圆屏）

用 **ESP32-S3-WROOM-1-N16**，把支持 [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) 协议的空调接入 **Apple Home / Google Home / Home Assistant**（Matter，本地控制）。

硬件默认搭配：

- **1.28″ 圆形触摸屏**：GC9A01（SPI）+ IT7259（I2C）
- 普通 **940 nm 红外发射管 + 38 kHz 接收头**
- 可选 **SHT30** 温湿度、**WS2812** 氛围灯

> 协议覆盖：仅支持 IRremoteESP8266 已实现的空调协议。不在库中的遥控无法自动学习；可用 BOOT **双击** Alt 遍历候选协议。

---

## 功能一览

| 功能 | 说明 |
|------|------|
| 圆屏 UI（LVGL） | 默认中文，右上角可切英文 |
| 未配网 | 屏上显示与串口一致的动态 Matter `MT:...` 二维码 + 数字配对码 |
| 已配网未学码 | 屏上「开始学习」；学习中 WS2812 **黄色呼吸** |
| 学码后 | 空调页（开关 / 降温 / 升温）；左滑进入灯光页 |
| Matter 空调 | 开关、制冷/制热、整度温度、风扇 |
| Matter 灯光 | **On/Off + 亮度（LevelControl）**；彩虹/呼吸白/温感等模式仅屏上选（方案 A） |
| BOOT 键 | **双击** Alt 遍历；**长按 ~5s** 恢复出厂（单击不再学码） |

Matter 端点（`CONFIG_ESP_MATTER_MAX_DYNAMIC_ENDPOINT_COUNT=4`）：

1. Room Air Conditioner  
2. Fan  
3. Humidity Sensor  
4. **Dimmable Light**（WS2812）

---

## 默认引脚（ESP32-S3-WROOM-1-N16）

定义见 `main/board_pins.h`。避开 Flash（26–32）、USB（19/20）。

| GPIO | 功能 |
|------|------|
| **4** | IR RX |
| **5** | IR TX（经三极管/MOS 驱动） |
| **8 / 9** | I2C SDA / SCL（SHT30 + IT7259 共用） |
| **10** | WS2812 DIN |
| **11** | 状态 LED（低电平点亮） |
| **0** | BOOT 键 |
| **12 / 13 / 14 / 21 / 47 / 48** | LCD SCLK / MOSI / CS / DC / RST / BL |
| **15 / 16** | TP INT / RST |
| IT7259 地址 | `0x46` |

触摸规格图仅标 TP_SDA/SCL/INT/RST 时，按上表接；芯片按 **IT7259** 驱动。

---

## 编译烧录

```bash
git clone --recursive https://github.com/realDavy/bc7215_ac_matter.git
cd bc7215_ac_matter
idf.py set-target esp32s3
idf.py build
idf.py -p <串口> erase-flash flash monitor
```

- Flash：**16 MB**（`CONFIG_ESPTOOLPY_FLASHSIZE_16MB`，分区见 `partitions.csv`）
- 依赖：`esp_lcd_gc9a01`、`esp_lvgl_port`、`lvgl`、`led_strip`（`main/idf_component.yml`）

---

## 使用流程

1. **配网**：屏上扫 Matter QR（与串口 `MT:...` 一致）或输入数字码。  
2. **学码**：配网后点「开始学习」，遥控对准接收头按任意键；成功后 WS2812 黄呼吸结束。  
3. **控空调**：屏上开关/升降温，与手机 Matter 双向同步。  
4. **灯光**：空调页左滑；选夜间关闭 / 手动 / 温感呼吸（默认）/ 纯色 / 彩虹 / 呼吸白；亮度条同步 Matter LevelControl。  
5. **Alt**：BOOT 双击遍历协议；空调有反应后用手机发一条 Matter 命令确认。  
6. **出厂**：BOOT 长按约 5 秒后松开。

设备身份：Manufacturer=`aidaegis`，名称=`AC Remote`，序列号=随机 8 位十六进制 + MAC 后 4 位。

---

## 状态 LED（GPIO11）

| 灯效 | 含义 |
|------|------|
| 常亮 | 未配网完成 |
| 快闪 | 红外学习 / 恢复出厂提示 |
| 慢闪 | 联网中 |
| 约每 3 秒一闪 | 正常待机 |

---

## 软件结构

```
main/
  board_pins.h / board_i2c.*
  app_main.cpp / app_driver.cpp
  drivers/   sht30, it7259, display_gc9a01, ws2812_temp_light
  ui/        LVGL 界面、中文字体子集、QR 编码
components/ir_ac/
deps/IRremoteESP8266/
```

---

## 许可证

- 工程代码：[`LICENSE`](LICENSE)（MIT）  
- IRremoteESP8266：LGPL-2.1  
- 其他组件：各自 SPDX
)
