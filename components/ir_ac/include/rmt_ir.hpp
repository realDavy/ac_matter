#pragma once

#include <stddef.h>
#include <stdint.h>
#include <vector>

#include "driver/gpio.h"
#include "esp_err.h"

namespace ir_ac {

esp_err_t rmt_ir_init(gpio_num_t tx_gpio, gpio_num_t rx_gpio);
void rmt_ir_deinit();

/* Transmit mark/space timings in microseconds. Odd indices are spaces. */
esp_err_t rmt_ir_transmit(const std::vector<int> &timings_us,
                          uint32_t carrier_hz = 38000);

/* Arm a non-blocking receive into an internal buffer. */
esp_err_t rmt_ir_start_receive();
void rmt_ir_stop_receive();

/* True when a complete frame was captured since the last start/consume. */
bool rmt_ir_frame_ready();

/*
 * Copy captured pulse durations (microseconds) into out_us.
 * First entry is typically a mark. Returns false if nothing ready.
 */
bool rmt_ir_consume_frame(std::vector<uint32_t> *out_us);

bool rmt_ir_is_busy();

}  // namespace ir_ac
