#pragma once

#include <esp_err.h>
#include <lvgl.h>

esp_err_t display_init(void);

#if LVGL_VERSION_MAJOR >= 9
lv_display_t *display_get_disp(void);
#else
lv_disp_t *display_get_disp(void);
#endif

void display_set_backlight(bool on);

bool display_is_ready(void);
