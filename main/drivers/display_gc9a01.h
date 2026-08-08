#pragma once

#include <esp_err.h>
#include <lvgl.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t display_init(void);

/**
 * Tear down LVGL (task + draw buffers) while keeping the GC9A01 panel/SPI and
 * backlight up so the last drawn frame (e.g. "配对中...") remains visible. Used
 * during Matter PASE; call display_init() afterwards to bring the UI back.
 */
void display_suspend_lvgl(void);

lv_disp_t *display_get_disp(void);

void display_set_backlight(bool on);

bool display_is_ready(void);

bool display_is_backlight_on(void);

/**
 * User / system activity: turn backlight on and restart the 60s idle timer.
 * Safe to call from any task.
 */
void display_activity_notify(void);

/**
 * While hold is true, backlight stays on and the idle timer does not fire
 * (pairing QR, pairing-busy, IR learn, Matter PASE). Absolute (not nested).
 */
void display_set_idle_hold(bool hold);

#ifdef __cplusplus
}
#endif
