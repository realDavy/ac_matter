#pragma once

#include <esp_err.h>
#include <lvgl.h>

esp_err_t display_init(void);

lv_disp_t *display_get_disp(void);

void display_set_backlight(bool on);

bool display_is_ready(void);
)
