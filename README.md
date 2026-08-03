# Matter Protocol Air Conditioner Controller (ESP32 + IRremoteESP8266)

## Key Features

- Connect air conditioners supported by [IRremoteESP8266](https://github.com/crankyoldgit/IRremoteESP8266) to Apple Home, Google Home, or Home Assistant via Matter
- No cloud account required for the AC control path itself
- Uses a plain IR LED + IR receiver (no BC7215 module)

> **Coverage note:** This build only supports AC brands/protocols implemented by IRremoteESP8266. It is **not** a universal learner like the earlier BC7215-based firmware. If your remote protocol is not in the library, pairing will fail—use double-click Alt traversal to try listed protocols manually.

Matter is built into iPhones and Android phones, so Matter devices can be added directly in Apple Home / Google Home without a dedicated vendor app.

## Hardware Connections

Online installation guidance targets ESP32-C3 modules. Other ESP32 variants work after changing the GPIO definitions and rebuilding.

Default IR pins (edit in `main/app_driver.cpp`):

```cpp
static constexpr gpio_num_t IR_TX_PIN = GPIO_NUM_4;  // IR LED (via transistor recommended)
static constexpr gpio_num_t IR_RX_PIN = GPIO_NUM_3;  // TSOP/VS1838 demodulator OUT
static constexpr gpio_num_t SUPER_MINI_LED_GPIO = GPIO_NUM_8;
```

Recommended wiring:

- **TX:** ESP32 GPIO → NPN/MOSFET driver → 940 nm IR LED(s) → current-limiting resistor
- **RX:** VS1838 / HX1838 / similar 38 kHz IR receiver: OUT→GPIO3, VCC→3V3, GND→GND
- **Status LED:** GPIO8 (active-low on many Super Mini boards)
- **Button:** board BOOT/Flash button (via ESP-Matter device HAL)

Driving the IR LED directly from a GPIO gives only a short range. Use a transistor and one or more IR LEDs for room-scale range.

## Firmware Installation

### Clone and Compile

Because this project uses a git submodule for IRremoteESP8266, clone recursively:

```bash
git clone --recursive https://github.com/realDavy/bc7215_ac_matter.git
```

Then in an ESP-Matter / ESP-IDF environment:

```bash
idf.py set-target esp32c3
idf.py menuconfig   # set MAX_DYNAMIC_ENDPOINT to 3 if needed
idf.py build
idf.py -p <serial-port> erase-flash flash monitor
```

Pin customization: edit `IR_TX_PIN` / `IR_RX_PIN` near the top of `main/app_driver.cpp`.

## Setup and Usage

### 1. Pair with the air conditioner

1. Power on the device. Steady LED = factory / not configured.
2. Press the button once. LED blinks rapidly = pairing mode.
3. Point the original AC remote at the IR receiver and press any button (Cool / 25°C / Fan is a good choice).
4. If the protocol is recognized, pairing completes and the LED pattern changes.
5. If pairing fails, the protocol is likely unsupported—or try Alt traversal below.

### 2. Connect to a phone (Matter)

1. Use 2.4 GHz Wi-Fi.
2. In Apple Home / Google Home, add a Matter accessory and scan the commissioning QR code from the original project docs / device label.
3. Expect an “uncertified device” warning on DIY firmware.

### Double-click Alt protocol traversal

If auto-decode fails, double-click the button to walk through a curated list of IRremoteESP8266 AC protocols. Each step transmits a Cool/25°C test frame. When the AC responds (usually a beep), send any Matter command (e.g. temperature) to accept that protocol.

LED flash count indicates the protocol index. After all candidates are tried, the device returns to unpaired.

### Factory reset

Hold the button ~5 seconds (LED blinks fast), then release. This erases IR pairing and Matter provisioning.

## LED Status Indicators

| LED Behaviour                       | Meaning                                                                             |
| ----------------------------------- | ----------------------------------------------------------------------------------- |
| Steadily on                         | Factory state; the device has not yet been configured.                              |
| Steadily off                        | Not powered, or Alt traversal active                                                |
| Rapid flashing                      | Waiting to receive an infrared signal for air conditioner pairing.                  |
| Slow flashing                       | Establishing a network connection.                                                  |
| One short flash per second          | The network is configured, but the air conditioner has not yet been paired.         |
| Two short flashes per second        | The air conditioner is paired and the device is waiting to be connected to a phone. |
| One short flash every three seconds | Pairing and connection are complete; the device is in standby mode.                 |

## Architecture

- **Matter / app logic:** `main/app_driver.cpp`, `main/app_main.cpp`
- **IR component:** `components/ir_ac`
  - ESP-IDF RMT transmit/receive
  - IRremoteESP8266 `IRac` for AC encode/decode (built with `UNIT_TEST` + `SWIGLIB` so timings are generated in software and sent via RMT)
- **Dependency:** `deps/IRremoteESP8266` (git submodule)

Supported protocols: see [SupportedProtocols.md](https://github.com/crankyoldgit/IRremoteESP8266/blob/master/SupportedProtocols.md) (AC section / `IRac::isProtocolSupported`).

## Matter limitations

Matter HVAC mapping is still limited compared with a full AC remote. This firmware currently focuses on Cooling/Heating, integer °C setpoints, and Low/Medium/High fan mapping from percentage, same as the previous release.

## License

Project code follows the repository LICENSE. IRremoteESP8266 remains under its LGPL-2.1 license.
