#include "board_i2c.h"
#include "board_pins.h"

#include <esp_log.h>
#include <atomic>

static const char *TAG = "board_i2c";
static std::atomic<bool> s_ready{false};

esp_err_t board_i2c_init(void)
{
    if (s_ready.load()) {
        return ESP_OK;
    }

    i2c_config_t conf = {};
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = BOARD_I2C_SDA_GPIO;
    conf.scl_io_num = BOARD_I2C_SCL_GPIO;
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = BOARD_I2C_FREQ_HZ;

    esp_err_t err = i2c_param_config(BOARD_I2C_PORT, &conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_param_config failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2c_driver_install(BOARD_I2C_PORT, conf.mode, 0, 0, 0);
    if (err == ESP_ERR_INVALID_STATE) {
        /* Already installed by another caller — treat as success. */
        err = ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_driver_install failed: %s", esp_err_to_name(err));
        return err;
    }

    s_ready.store(true);
    ESP_LOGI(TAG, "I2C ready on SDA=GPIO%d SCL=GPIO%d @ %d Hz",
             static_cast<int>(BOARD_I2C_SDA_GPIO),
             static_cast<int>(BOARD_I2C_SCL_GPIO),
             BOARD_I2C_FREQ_HZ);
    return ESP_OK;
}

bool board_i2c_is_ready(void)
{
    return s_ready.load();
}

i2c_port_t board_i2c_port(void)
{
    return BOARD_I2C_PORT;
}
)
