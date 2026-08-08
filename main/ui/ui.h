#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ui_init(void);

/**
 * Replace the QR / code screen with a light "Pairing..." message and flush it
 * to the panel before LVGL is suspended for Matter PASE.
 */
esp_err_t ui_show_commissioning_busy(void);

/** Stop the UI task so LVGL can be suspended for Matter PASE. */
void ui_deinit(void);

void ui_tick(void); /* optional; LVGL port owns timer */

/** Refresh screens from Matter / IR / light state. Safe to call often. */
void ui_update(void);

void ui_set_language_english(bool english);
bool ui_is_language_english(void);

#ifdef __cplusplus
}
#endif
