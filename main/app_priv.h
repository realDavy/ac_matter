/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#pragma once

#include <esp_err.h>
#include <esp_matter.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include "esp_openthread_types.h"
#endif

/** Default attribute values used during initialization */
#define DEFAULT_POWER true

typedef void *app_driver_handle_t;


// Define the LED display modes
typedef enum {
    LED_MODE_OFF = 0,      // Always off
    LED_MODE_ON,           // Always on
    LED_MODE_BLINK_1HZ,    // Blink at 1 Hz (toggle every 500 ms)
    LED_MODE_BLINK_3HZ,    // Blink at 3 Hz (toggle every 166 ms)
    LED_MODE_BREATH_3S,    // Brief flash every 3 seconds
	LED_MODE_BLINK_1X_1HZ, // Flash once every second
	LED_MODE_BLINK_2X_1HZ, // Flash twice every second
	ONE_SHOT_FLASH_3X,	   // Flash three times
	ONE_SHOT_ON_1S,			// Stay on for 1 second
} led_display_mode_t;


enum class app_matter_state_t : uint8_t {
    NOT_COMMISSIONED = 0,        // Not commissioned
    COMMISSIONING,               // Commissioning in progress
    COMMISSIONED_OFFLINE,        // Commissioned, but Wi-Fi is not connected
    CONNECTED_NO_SUBSCRIPTION,   // Wi-Fi is connected, but there is no active subscription
    SUBSCRIBED                   // At least one active subscription exists
};

/** Initialize the room_air_conditioner driver
 *
 * This initializes the room_air_conditioner driver associated with the selected board.
 *
 * @return Handle on success.
 * @return NULL in case of failure.
 */
app_driver_handle_t app_driver_room_air_conditioner_init();

/** Initialize the button driver
 *
 * This initializes the button driver associated with the selected board.
 *
 * @return Handle on success.
 * @return NULL in case of failure.
 */
app_driver_handle_t app_driver_button_init();

/** Driver Update
 *
 * This API should be called to update the driver for the attribute being updated.
 * This is usually called from the common `app_attribute_update_cb()`.
 *
 * @param[in] endpoint_id Endpoint ID of the attribute.
 * @param[in] cluster_id Cluster ID of the attribute.
 * @param[in] attribute_id Attribute ID of the attribute.
 * @param[in] val Pointer to `esp_matter_attr_val_t`. Use appropriate elements as per the value type.
 *
 * @return ESP_OK on success.
 * @return error in case of failure.
 */
esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val);

/** Set defaults for room_air_conditioner driver
 *
 * Set the attribute drivers to their default values from the created data model.
 *
 * @param[in] endpoint_id Endpoint ID of the driver.
 *
 * @return ESP_OK on success.
 * @return error in case of failure.
 */
esp_err_t app_driver_room_air_conditioner_set_defaults(uint16_t endpoint_id);


void app_driver_update_led_states(void);

app_matter_state_t app_get_matter_state_locked();

void app_driver_set_subscription_active(bool active);

/**
 * When true, Thermostat LocalTemperature is driven by the ambient SHT30
 * sensor and must not be overwritten with the AC setpoint.
 */
void app_driver_set_ambient_sensor_active(bool active);

bool app_driver_ambient_sensor_active(void);

/** Apply Matter On/Off for the WS2812 temperature indicator light. */
esp_err_t app_driver_temp_light_set_power(esp_matter_attr_val_t *val);

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                           \
    {                                                                                   \
        .radio_mode = RADIO_MODE_NATIVE,                                                \
    }

#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                            \
    {                                                                                   \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                              \
    }

#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                            \
    {                                                                                   \
        .storage_partition_name = "nvs", .netif_queue_size = 10, .task_queue_size = 10, \
    }
#endif
