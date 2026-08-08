#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint16_t x;
    uint16_t y;
    bool pressed;
} it7259_point_t;

esp_err_t it7259_init(void);

/** Poll one touch point. Coordinates are in LCD pixels (0..239). */
esp_err_t it7259_read(it7259_point_t *point);

bool it7259_is_ready(void);
