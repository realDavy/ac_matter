/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <stdint.h>

using sht30_sensor_cb_t = void (*)(uint16_t endpoint_id, float value, void *user_data);

typedef struct {
    struct {
        /** Called periodically with Celsius temperature. */
        sht30_sensor_cb_t cb;
        /** Matter endpoint that should receive temperature updates. */
        uint16_t endpoint_id;
    } temperature;

    struct {
        /** Called periodically with relative humidity in percent. */
        sht30_sensor_cb_t cb;
        /** Matter endpoint that should receive humidity updates. */
        uint16_t endpoint_id;
    } humidity;

    void *user_data;

    /** Polling interval in milliseconds (use 5000 if zero). */
    uint32_t interval_ms;
} sht30_sensor_config_t;

/**
 * @brief Initialize the SHT30 driver and start periodic sampling.
 *
 * At least one of temperature.cb / humidity.cb must be set. The pointed-to
 * config must outlive the driver.
 *
 * @return ESP_OK on success; ESP_ERR_INVALID_ARG / ESP_ERR_INVALID_STATE /
 *         or an I2C / timer error otherwise. Failure is non-fatal for the
 *         rest of the application when the sensor is not wired.
 */
esp_err_t sht30_sensor_init(sht30_sensor_config_t *config);
