#pragma once

/*
 * Minimal Arduino compatibility layer for building IRremoteESP8266 under
 * ESP-IDF with -DUNIT_TEST -DSWIGLIB.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <algorithm>
#include <string>

#include "esp_timer.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#ifndef HIGH
#define HIGH 0x1
#endif
#ifndef LOW
#define LOW 0x0
#endif

#ifndef INPUT
#define INPUT 0x0
#endif
#ifndef OUTPUT
#define OUTPUT 0x1
#endif
#ifndef INPUT_PULLUP
#define INPUT_PULLUP 0x2
#endif

#ifndef PI
#define PI 3.1415926535897932384626433832795
#endif

typedef uint8_t byte;
typedef bool boolean;
typedef unsigned int word;

#ifndef F
#define F(x) x
#endif

#ifndef PROGMEM
#define PROGMEM
#endif

#ifndef pgm_read_byte
#define pgm_read_byte(addr) (*(const uint8_t *)(addr))
#endif
#ifndef pgm_read_word
#define pgm_read_word(addr) (*(const uint16_t *)(addr))
#endif
#ifndef pgm_read_dword
#define pgm_read_dword(addr) (*(const uint32_t *)(addr))
#endif
#ifndef memcpy_P
#define memcpy_P memcpy
#endif
#ifndef strlen_P
#define strlen_P strlen
#endif
#ifndef strcpy_P
#define strcpy_P strcpy
#endif
#ifndef strncpy_P
#define strncpy_P strncpy
#endif
#ifndef strcmp_P
#define strcmp_P strcmp
#endif
#ifndef strcasecmp_P
#define strcasecmp_P strcasecmp
#endif
#ifndef sprintf_P
#define sprintf_P sprintf
#endif
#ifndef FPSTR
#define FPSTR(X) X
#endif
#ifndef PSTR
#define PSTR(X) X
#endif

static inline unsigned long micros(void)
{
    return static_cast<unsigned long>(esp_timer_get_time());
}

static inline unsigned long millis(void)
{
    return static_cast<unsigned long>(esp_timer_get_time() / 1000ULL);
}

static inline void delay(unsigned long ms)
{
    vTaskDelay(pdMS_TO_TICKS(ms == 0 ? 1 : ms));
}

static inline void delayMicroseconds(unsigned int us)
{
    esp_rom_delay_us(us);
}

static inline void yield(void)
{
    taskYIELD();
}

static inline void pinMode(uint8_t pin, uint8_t mode)
{
    gpio_config_t cfg = {};
    cfg.pin_bit_mask = 1ULL << pin;
    cfg.mode = (mode == OUTPUT) ? GPIO_MODE_OUTPUT : GPIO_MODE_INPUT;
    cfg.pull_up_en =
        (mode == INPUT_PULLUP) ? GPIO_PULLUP_ENABLE : GPIO_PULLUP_DISABLE;
    cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    cfg.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&cfg);
}

static inline void digitalWrite(uint8_t pin, uint8_t val)
{
    gpio_set_level(static_cast<gpio_num_t>(pin), val ? 1 : 0);
}

static inline int digitalRead(uint8_t pin)
{
    return gpio_get_level(static_cast<gpio_num_t>(pin));
}

class __EspStub {
 public:
    void restart() {}
};

static __EspStub ESP;

/*
 * Dummy Serial / interrupt APIs so accidental non-UNIT_TEST paths still
 * compile. Capture/transmit never rely on these under SWIGLIB + UNIT_TEST.
 */
class __FakeSerial {
 public:
    template <typename T>
    void print(T) {}
    template <typename T>
    void println(T) {}
    void begin(unsigned long) {}
};

static __FakeSerial Serial;

static inline void attachInterrupt(uint8_t, void (*)(void), int) {}
static inline void detachInterrupt(uint8_t) {}

#ifndef CHANGE
#define CHANGE 1
#endif
#ifndef FALLING
#define FALLING 2
#endif
#ifndef RISING
#define RISING 3
#endif
