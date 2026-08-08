#pragma once

#include <esp_err.h>
#include <lvgl.h>

esp_err_t display_init(void);

/**
 * Tear down LVGL (task + draw buffers) while keeping the GC9A01 panel/SPI up.
 * Used during Matter PASE so BLE crypto has enough heap; call display_init()
 * again afterwards to bring the UI back.
 */
void display_suspend_lvgl(void);

lv_disp_t *display_get_disp(void);

void display_set_backlight(bool on);

bool display_is_ready(void);
