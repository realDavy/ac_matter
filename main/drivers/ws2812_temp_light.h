#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    WS2812_MODE_NIGHT_OFF = 0,
    WS2812_MODE_MANUAL = 1,
    WS2812_MODE_TEMP_BREATH = 2, /* default */
    WS2812_MODE_SOLID = 3,
    WS2812_MODE_RAINBOW = 4,
    WS2812_MODE_WHITE_BREATH = 5,
} ws2812_light_mode_t;

esp_err_t ws2812_temp_light_init(void);

/** Matter On/Off. When off, LED is cleared regardless of mode. */
void ws2812_temp_light_set_enabled(bool enabled);
bool ws2812_temp_light_is_enabled(void);

/** Matter LevelControl CurrentLevel (1..254). */
void ws2812_temp_light_set_brightness(uint8_t level_1_254);
uint8_t ws2812_temp_light_get_brightness(void);

/** Screen-only effect mode (not exposed to Matter). */
void ws2812_temp_light_set_mode(ws2812_light_mode_t mode);
ws2812_light_mode_t ws2812_temp_light_get_mode(void);

/**
 * While IR learn is active, force yellow breathing and ignore other modes
 * until cleared.
 */
void ws2812_temp_light_set_learn_active(bool active);

void ws2812_temp_light_set_temperature_c(float temp_c);

bool ws2812_temp_light_is_ready(void);

#ifdef __cplusplus
}
#endif
)
