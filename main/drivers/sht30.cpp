/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "sht30.h"

#include <driver/i2c.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

static const char *TAG = "sht30";

/*
 * Default I2C pins for ESP32-C3 Super Mini / similar boards.
 * Avoid IR TX (GPIO4), IR RX (GPIO3), status LED (GPIO8), and BOOT (GPIO9).
 * Override via menuconfig symbols when present.
 */
#if defined(CONFIG_SHT30_I2C_SDA_PIN)
#define I2C_MASTER_SDA_IO CONFIG_SHT30_I2C_SDA_PIN
#else
#define I2C_MASTER_SDA_IO 5
#endif

#if defined(CONFIG_SHT30_I2C_SCL_PIN)
#define I2C_MASTER_SCL_IO CONFIG_SHT30_I2C_SCL_PIN
#else
#define I2C_MASTER_SCL_IO 6
#endif

#define I2C_MASTER_NUM I2C_NUM_0
#define I2C_MASTER_FREQ_HZ 100000

/* ADDR pin low -> 0x44; ADDR pin high -> 0x45 */
#if defined(CONFIG_SHT30_I2C_ADDR_VDD)
#define SHT30_SENSOR_ADDR 0x45
#else
#define SHT30_SENSOR_ADDR 0x44
#endif

/* Single-shot, high repeatability, clock stretching disabled */
static constexpr uint8_t SHT30_CMD_MEASURE_H[] = {0x24, 0x00};
static constexpr uint8_t SHT30_CMD_SOFT_RESET[] = {0x30, 0xA2};

typedef struct {
    sht30_sensor_config_t *config;
    TaskHandle_t task;
    bool is_initialized;
} sht30_sensor_ctx_t;

static sht30_sensor_ctx_t s_ctx = {};

static uint8_t sht30_crc8(const uint8_t *data, size_t len)
{
    uint8_t crc = 0xFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; ++bit) {
            if (crc & 0x80) {
                crc = static_cast<uint8_t>((crc << 1) ^ 0x31);
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

static esp_err_t sht30_write_command(const uint8_t cmd[2])
{
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SHT30_SENSOR_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write(handle, const_cast<uint8_t *>(cmd), 2, true);
    i2c_master_stop(handle);
    esp_err_t err = i2c_master_cmd_begin(I2C_MASTER_NUM, handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(handle);
    return err;
}

static esp_err_t sht30_init_i2c()
{
    i2c_config_t i2c_conf = {};
    i2c_conf.mode = I2C_MODE_MASTER;
    i2c_conf.sda_io_num = I2C_MASTER_SDA_IO;
    i2c_conf.scl_io_num = I2C_MASTER_SCL_IO;
    i2c_conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    i2c_conf.master.clk_speed = I2C_MASTER_FREQ_HZ;

    esp_err_t err = i2c_param_config(I2C_MASTER_NUM, &i2c_conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(I2C_MASTER_NUM, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    return ESP_OK;
}

static esp_err_t sht30_measure(float *temperature_c, float *humidity_pct)
{
    esp_err_t err = sht30_write_command(SHT30_CMD_MEASURE_H);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "measure command failed: %s", esp_err_to_name(err));
        return err;
    }

    /* High-repeatability single-shot typically completes within ~15 ms */
    vTaskDelay(pdMS_TO_TICKS(20));

    uint8_t data[6] = {};
    i2c_cmd_handle_t handle = i2c_cmd_link_create();
    i2c_master_start(handle);
    i2c_master_write_byte(handle, (SHT30_SENSOR_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(handle, data, sizeof(data), I2C_MASTER_LAST_NACK);
    i2c_master_stop(handle);
    err = i2c_master_cmd_begin(I2C_MASTER_NUM, handle, pdMS_TO_TICKS(1000));
    i2c_cmd_link_delete(handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "read failed: %s", esp_err_to_name(err));
        return err;
    }

    if (sht30_crc8(data, 2) != data[2] || sht30_crc8(data + 3, 2) != data[5]) {
        ESP_LOGW(TAG, "CRC mismatch on sensor frame");
        return ESP_ERR_INVALID_CRC;
    }

    const uint16_t raw_temp = static_cast<uint16_t>((data[0] << 8) | data[1]);
    const uint16_t raw_hum = static_cast<uint16_t>((data[3] << 8) | data[4]);

    if (temperature_c) {
        *temperature_c = -45.0f + 175.0f * (static_cast<float>(raw_temp) / 65535.0f);
    }
    if (humidity_pct) {
        *humidity_pct = 100.0f * (static_cast<float>(raw_hum) / 65535.0f);
        if (*humidity_pct < 0.0f) {
            *humidity_pct = 0.0f;
        } else if (*humidity_pct > 100.0f) {
            *humidity_pct = 100.0f;
        }
    }

    return ESP_OK;
}

static void sht30_notify(sht30_sensor_config_t *config, float temp, float humidity)
{
    if (config->temperature.cb) {
        config->temperature.cb(config->temperature.endpoint_id, temp, config->user_data);
    }
    if (config->humidity.cb) {
        config->humidity.cb(config->humidity.endpoint_id, humidity, config->user_data);
    }
}

/*
 * Sampling runs in a FreeRTOS task (not esp_timer) because the measurement
 * path needs a short blocking wait for the conversion to finish.
 */
static void sht30_poll_task(void *arg)
{
    auto *ctx = static_cast<sht30_sensor_ctx_t *>(arg);
    const uint32_t interval_ms =
        (ctx->config && ctx->config->interval_ms) ? ctx->config->interval_ms : 5000;

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(interval_ms));

        float temp = 0.0f;
        float humidity = 0.0f;
        if (sht30_measure(&temp, &humidity) == ESP_OK && ctx->config) {
            sht30_notify(ctx->config, temp, humidity);
        }
    }
}

esp_err_t sht30_sensor_init(sht30_sensor_config_t *config)
{
    if (config == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (config->temperature.cb == nullptr && config->humidity.cb == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_ctx.is_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = sht30_init_i2c();
    if (err != ESP_OK) {
        return err;
    }

    err = sht30_write_command(SHT30_CMD_SOFT_RESET);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "SHT30 not responding on I2C (SDA=%d SCL=%d ADDR=0x%02X): %s",
                 I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, SHT30_SENSOR_ADDR,
                 esp_err_to_name(err));
        i2c_driver_delete(I2C_MASTER_NUM);
        return err;
    }
    vTaskDelay(pdMS_TO_TICKS(2));

    float temp = 0.0f;
    float humidity = 0.0f;
    err = sht30_measure(&temp, &humidity);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Initial SHT30 measurement failed: %s", esp_err_to_name(err));
        i2c_driver_delete(I2C_MASTER_NUM);
        return err;
    }

    s_ctx.config = config;

    const uint32_t interval_ms = config->interval_ms ? config->interval_ms : 5000;
    BaseType_t created =
        xTaskCreate(sht30_poll_task, "sht30_poll", 3072, &s_ctx, 5, &s_ctx.task);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create SHT30 poll task");
        s_ctx.config = nullptr;
        i2c_driver_delete(I2C_MASTER_NUM);
        return ESP_ERR_NO_MEM;
    }

    s_ctx.is_initialized = true;
    ESP_LOGI(TAG,
             "SHT30 ready (SDA=%d SCL=%d ADDR=0x%02X): %.2f°C / %.2f%%RH, period=%lums",
             I2C_MASTER_SDA_IO, I2C_MASTER_SCL_IO, SHT30_SENSOR_ADDR, temp, humidity,
             static_cast<unsigned long>(interval_ms));

    /* Push the first sample immediately so Matter attributes are not stale. */
    sht30_notify(config, temp, humidity);

    return ESP_OK;
}
