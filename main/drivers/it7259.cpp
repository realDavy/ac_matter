#include "it7259.h"

#include "board_i2c.h"
#include "board_pins.h"

#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <string.h>

static const char *TAG = "it7259";

/*
 * IT7259 buffer indexes (ITE Cap Sensor Programming Guide):
 *   0x80 Query Buffer
 *   0xE0 Point Information Buffer
 *
 * I2C 7-bit address is 0x46 (write 0x8C / read 0x8D on the wire).
 */
static constexpr uint8_t kCmdQuery = 0x80;
static constexpr uint8_t kCmdPoint = 0xE0;

/* Query bits 7:6 = Packet Information Status. */
static constexpr uint8_t kQueryNewPacket = 0x80; /* 1xb: new packet available */
static constexpr uint8_t kQueryStillTouch = 0x40; /* 01b: finger down, no new pkt */
static constexpr uint8_t kQueryCmdBusy = 0x01;    /* command status busy */

/* Point report byte0: format tag 0000b, low nibble = point/finger flags. */
static constexpr uint8_t kPoint0Valid = 0x01;
static constexpr uint8_t kPointFormatMask = 0xF0;

static bool s_ready = false;
static uint16_t s_last_x = 0;
static uint16_t s_last_y = 0;

/**
 * Write buffer index then repeated-start read (S W reg Sr R data P).
 * Matches the ITE programming-guide I2C examples.
 */
static esp_err_t it7259_read_reg(uint8_t reg, uint8_t *buf, size_t len)
{
    if (buf == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_IT7259_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (BOARD_IT7259_ADDR << 1) | I2C_MASTER_READ, true);
    if (len > 1) {
        i2c_master_read(cmd, buf, len - 1, I2C_MASTER_ACK);
    }
    i2c_master_read_byte(cmd, buf + len - 1, I2C_MASTER_NACK);
    i2c_master_stop(cmd);

    board_i2c_lock();
    esp_err_t err = i2c_master_cmd_begin(board_i2c_port(), cmd, pdMS_TO_TICKS(50));
    board_i2c_unlock();
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

    /* Probe Query Buffer; NAK while the controller is still booting is OK. */
    uint8_t query = 0xFF;
    for (int attempt = 0; attempt < 20; ++attempt) {
        err = it7259_read_reg(kCmdQuery, &query, 1);
        if (err == ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "IT7259 probe failed on 0x%02X: %s",
                 BOARD_IT7259_ADDR, esp_err_to_name(err));
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
    /* LVGL expects last coordinates on release samples. */
    point->x = s_last_x;
    point->y = s_last_y;
    point->pressed = false;

    if (!s_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t query = 0xFF;
    esp_err_t err = it7259_read_reg(kCmdQuery, &query, 1);
    if (err != ESP_OK) {
        return err;
    }

    /*
     * Bits 7:6 Packet Information Status (ITE guide):
     *   00b — idle, no packet
     *   1xb — new packet available → read Point Buffer
     *   01b — finger still down, no new packet → keep pressed at last XY
     * Ignore command-busy (bit0) samples; retry next LVGL poll.
     */
    if (query & kQueryCmdBusy) {
        return ESP_OK;
    }

    const bool new_packet = (query & kQueryNewPacket) != 0;
    const bool still_touch = (query & kQueryStillTouch) != 0;
    if (!new_packet && !still_touch) {
        return ESP_OK;
    }

    if (!new_packet) {
        /* Finger held without a fresh report. */
        point->pressed = true;
        return ESP_OK;
    }

    uint8_t buf[14] = {};
    err = it7259_read_reg(kCmdPoint, buf, sizeof(buf));
    if (err != ESP_OK) {
        return err;
    }

    /* Point reports use format tag 0000b; 1000b is gesture — ignore gestures. */
    if ((buf[0] & kPointFormatMask) != 0) {
        return ESP_OK;
    }

    /* 000b in point-info nibble => all fingers removed. */
    if ((buf[0] & 0x07) == 0) {
        return ESP_OK;
    }

    if ((buf[0] & kPoint0Valid) == 0) {
        return ESP_OK;
    }

    /*
     * Packed 12-bit coordinates (ITE guide §6.1):
     *   buf[2]      = X[7:0]
     *   buf[3][3:0] = X[11:8]
     *   buf[3][7:4] = Y[11:8]
     *   buf[4]      = Y[7:0]
     */
    uint16_t x = static_cast<uint16_t>(buf[2] | ((buf[3] & 0x0F) << 8));
    uint16_t y = static_cast<uint16_t>(buf[4] | ((buf[3] & 0xF0) << 4));

    if (x >= BOARD_LCD_H_RES) {
        x = BOARD_LCD_H_RES - 1;
    }
    if (y >= BOARD_LCD_V_RES) {
        y = BOARD_LCD_V_RES - 1;
    }

    s_last_x = x;
    s_last_y = y;
    point->x = x;
    point->y = y;
    point->pressed = true;
    return ESP_OK;
}

bool it7259_is_ready(void)
{
    return s_ready;
}
