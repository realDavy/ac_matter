#include "it7259.h"

#include "board_i2c.h"
#include "board_pins.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char *TAG = "it7259";

/* IT7259 command / point registers (common on 1.28" GC9A01 modules). */
static constexpr uint8_t kCmdQuery = 0x80;
static constexpr uint8_t kCmdPoint = 0xE0;

static bool s_ready = false;

static esp_err_t it7259_write_reg(uint8_t reg)
{
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_IT7259_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(board_i2c_port(), cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static esp_err_t it7259_read_bytes(uint8_t *buf, size_t len)
{
    if (buf == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_IT7259_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);
    esp_err_t err = i2c_master_cmd_begin(board_i2c_port(), cmd, pdMS_TO_TICKS(50));
    i2c_cmd_link_delete(cmd);
    return err;
}

static void it7259_hw_reset(void)
{
    if (BOARD_IT7259_RST_GPIO < 0) {
        return;
    }

    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOARD_IT7259_RST_GPIO;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io);

    gpio_set_level(BOARD_IT7259_RST_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(BOARD_IT7259_RST_GPIO, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

esp_err_t it7259_init(void)
{
    if (s_ready) {
        return ESP_OK;
    }

    esp_err_t err = board_i2c_init();
    if (err != ESP_OK) {
        return err;
    }

    it7259_hw_reset();

    if (BOARD_IT7259_INT_GPIO >= 0) {
        gpio_config_t io = {};
        io.pin_bit_mask = 1ULL << BOARD_IT7259_INT_GPIO;
        io.mode = GPIO_MODE_INPUT;
        io.pull_up_en = GPIO_PULLUP_ENABLE;
        io.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io.intr_type = GPIO_INTR_DISABLE;
        gpio_config(&io);
    }

    /* Probe: query register should ACK. */
    err = it7259_write_reg(kCmdQuery);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IT7259 probe failed on 0x%02X: %s",
                 BOARD_IT7259_ADDR, esp_err_to_name(err));
        return err;
    }

    uint8_t query = 0xFF;
    err = it7259_read_bytes(&query, 1);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IT7259 query read failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready = true;
    ESP_LOGI(TAG, "IT7259 ready (addr=0x%02X query=0x%02X)",
             BOARD_IT7259_ADDR, query);
    return ESP_OK;
}

esp_err_t it7259_read(it7259_point_t *point)
{
    if (point == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(point, 0, sizeof(*point));

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = it7259_write_reg(kCmdQuery);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t query = 0xFF;
    err = it7259_read_bytes(&query, 1);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Bit7 clear => point buffer has data / finger present
     * (matches common IT7257/IT7259 ESP & Rust drivers).
     */
    if (query & 0x80) {
        point->pressed = false;
        return ESP_OK;
    }

    err = it7259_write_reg(kCmdPoint);
    if (err != ESP_OK) {
        return err;
    }

    uint8_t buf[14] = {};
    err = it7259_read_bytes(buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    /* Point packet: buf[0] point info; X/Y little-endian in buf[2..5]. */
    const uint16_t x = static_cast<uint16_t>(buf[2] | (buf[3] << 8));
    const uint16_t y = static_cast<uint16_t>(buf[4] | (buf[5] << 8));

    point->x = (x >= BOARD_LCD_H_RES) ? static_cast<uint16_t>(BOARD_LCD_H_RES - 1) : x;
    point->y = (y >= BOARD_LCD_V_RES) ? static_cast<uint16_t>(BOARD_LCD_V_RES - 1) : y;
    point->pressed = (buf[0] & 0x01) != 0 || (query & 0x01) != 0 || (x | y) != 0;

    /* Some firmwares only clear bit7 while finger is down; treat that as press. */
    if (!(query & 0x80) && (x < BOARD_LCD_H_RES) && (y < BOARD_LCD_V_RES)) {
        point->pressed = true;
    }

    return ESP_OK;
}

bool it7259_is_ready(void)
{
    return s_ready;
}
