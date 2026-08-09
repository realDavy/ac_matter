#pragma once

#include <esp_err.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * How application endpoints are presented to Apple Home / Google Home.
 *
 * COMBINED: flat multi-endpoint node (one accessory with multiple services).
 * SEPARATE: Aggregator + Bridged Nodes (independent Home tiles).
 *
 * Changing this requires reboot and re-adding the accessory in Home.
 */
typedef enum {
    APP_HOME_DISPLAY_COMBINED = 0,
    APP_HOME_DISPLAY_SEPARATE = 1,
} app_home_display_mode_t;

/** Load preference from NVS (default: COMBINED). Safe after nvs_flash_init(). */
app_home_display_mode_t app_settings_get_home_display_mode(void);

/** Persist preference. Does not reboot by itself. */
esp_err_t app_settings_set_home_display_mode(app_home_display_mode_t mode);

static inline bool app_settings_home_display_is_separate(void)
{
    return app_settings_get_home_display_mode() == APP_HOME_DISPLAY_SEPARATE;
}

#ifdef __cplusplus
}
#endif
