/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <stdbool.h>

/**
 * @brief Initialize one WS2812 LED used as an ambient-temperature indicator.
 *
 * Maps room temperature to a green→orange hue and applies a breathing
 * brightness envelope. Power is controlled separately via
 * ws2812_temp_light_set_enabled().
 *
 * @return ESP_OK on success; non-fatal for the rest of the app on failure.
 */
esp_err_t ws2812_temp_light_init(void);

/** Enable or disable the indicator (Matter On/Off). Off clears the LED. */
void ws2812_temp_light_set_enabled(bool enabled);

bool ws2812_temp_light_is_enabled(void);

/**
 * @brief Update the color mapping from the latest SHT30 Celsius reading.
 *
 * Comfort scale (clamped):
 *   18 °C → green
 *   24 °C → yellow / amber
 *   30 °C → orange
 */
void ws2812_temp_light_set_temperature_c(float temp_c);

bool ws2812_temp_light_is_ready(void);
