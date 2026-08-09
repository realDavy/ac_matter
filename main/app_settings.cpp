#include "app_settings.h"

#include <esp_log.h>
#include <nvs.h>
#include <nvs_flash.h>

static const char *TAG = "app_settings";
static constexpr char kNs[] = "ui_pref";
static constexpr char kHomeDispKey[] = "home_disp";

extern "C" {

app_home_display_mode_t app_settings_get_home_display_mode(void)
{
    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNs, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return APP_HOME_DISPLAY_COMBINED;
    }

    uint8_t value = APP_HOME_DISPLAY_COMBINED;
    err = nvs_get_u8(handle, kHomeDispKey, &value);
    nvs_close(handle);

    if (err != ESP_OK) {
        return APP_HOME_DISPLAY_COMBINED;
    }

    if (value == APP_HOME_DISPLAY_SEPARATE) {
        return APP_HOME_DISPLAY_SEPARATE;
    }
    return APP_HOME_DISPLAY_COMBINED;
}

esp_err_t app_settings_set_home_display_mode(app_home_display_mode_t mode)
{
    const uint8_t value =
        (mode == APP_HOME_DISPLAY_SEPARATE)
            ? APP_HOME_DISPLAY_SEPARATE
            : APP_HOME_DISPLAY_COMBINED;

    nvs_handle_t handle = 0;
    esp_err_t err = nvs_open(kNs, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    err = nvs_set_u8(handle, kHomeDispKey, value);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save home_disp=%u: %s",
                 static_cast<unsigned>(value),
                 esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Saved home display mode: %s",
             value == APP_HOME_DISPLAY_SEPARATE ? "separate" : "combined");
    return ESP_OK;
}

} // extern "C"
