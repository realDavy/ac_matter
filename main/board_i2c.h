#pragma once

#include <esp_err.h>
#include <driver/i2c.h>

/**
 * Idempotent init of the shared board I2C bus (SHT30 + IT7259).
 * Safe to call from multiple drivers.
 */
esp_err_t board_i2c_init(void);

bool board_i2c_is_ready(void);

i2c_port_t board_i2c_port(void);
)
