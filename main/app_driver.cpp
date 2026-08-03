/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdlib.h>
#include <string.h>
#include <memory>
#include <string>
#include <cmath>
#include <atomic>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_app_desc.h>

#include <app_priv.h>
#include <device.h>
#include <bc7215ac.hpp>
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_timer.h> // ESP software timer API
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <platform/CHIPDeviceLayer.h>

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t room_air_conditioner_endpoint_id;
extern uint16_t fan_endpoint_id;
static const char *TAG_BC7215 = "bc7215_matter";

// ESP32-C3 test pins.
// Note: constructor parameter order is uart, rx, tx, busy, mod.
static constexpr uart_port_t BC7215_UART_NUM = UART_NUM_1;
static constexpr gpio_num_t BC7215_RX_PIN = GPIO_NUM_3;
static constexpr gpio_num_t BC7215_TX_PIN = GPIO_NUM_4;
static constexpr gpio_num_t BC7215_BUSY_PIN = GPIO_NUM_5;
static constexpr gpio_num_t BC7215_MOD_PIN = GPIO_NUM_6;

static constexpr gpio_num_t SUPER_MINI_LED_GPIO = GPIO_NUM_8;
static constexpr bool SUPER_MINI_LED_ACTIVE_LOW = true;

/*
 * LED controller
 *
 * The controller has two layers:
 *   1. A persistent/normal display: static ON/OFF or a repeating pattern.
 *   2. A temporary one-shot pattern. When it finishes, the latest normal
 *      display is restored automatically.
 *
 * All GPIO writes are performed by one FreeRTOS task. This avoids races
 * between Matter callbacks, the BC7215 worker and button callbacks.
 */

enum class led_normal_type_t : uint8_t {
    STATIC,
    PERIODIC,
};

enum class led_command_type_t : uint8_t {
    SET_STATIC,
    SET_PERIODIC,
    ONE_SHOT,
    CANCEL_ONE_SHOT,
};

enum class led_phase_t : uint8_t {
    STATIC,
    NORMAL_ON,
    NORMAL_OFF,
    NORMAL_INTERVAL,
    ONE_SHOT_ON,
    ONE_SHOT_OFF,
};

struct led_pattern_t {
    uint8_t flash_count = 1;
    uint32_t on_ms = 100;
    uint32_t off_ms = 100;
    uint32_t interval_ms = 0;
};

struct led_normal_state_t {
    led_normal_type_t type = led_normal_type_t::STATIC;
    bool static_on = false;
    led_pattern_t pattern{};
};

struct led_command_t {
    led_command_type_t type;
    bool static_on;
    led_pattern_t pattern;
};

static QueueHandle_t s_led_command_queue = nullptr;
static TaskHandle_t s_led_task_handle = nullptr;

static void app_driver_led_write(bool on)
{
    const int gpio_level = SUPER_MINI_LED_ACTIVE_LOW
        ? (on ? 0 : 1)
        : (on ? 1 : 0);

    gpio_set_level(SUPER_MINI_LED_GPIO, gpio_level);
}

static TickType_t app_driver_led_ms_to_ticks(uint32_t ms)
{
    TickType_t ticks = pdMS_TO_TICKS(ms);
    return ticks == 0 ? 1 : ticks;
}

static bool app_driver_led_make_pattern(
    uint8_t flash_count,
    uint32_t on_period_ms,
    uint32_t off_period_ms,
    uint32_t cycle_period_ms,
    bool periodic,
    led_pattern_t *pattern)
{
    if (pattern == nullptr ||
        flash_count == 0 ||
        on_period_ms == 0 ||
        off_period_ms == 0) {
        return false;
    }

    const uint64_t single_flash_ms =
        static_cast<uint64_t>(on_period_ms) + off_period_ms;
    const uint64_t flash_phase_ms =
        static_cast<uint64_t>(flash_count) * single_flash_ms;

    if (periodic && cycle_period_ms < flash_phase_ms) {
        return false;
    }

    pattern->flash_count = flash_count;
    pattern->on_ms = on_period_ms;
    pattern->off_ms = off_period_ms;
    pattern->interval_ms = periodic
        ? static_cast<uint32_t>(cycle_period_ms - flash_phase_ms)
        : 0;

    return true;
}

static esp_err_t app_driver_led_send_command(const led_command_t &command)
{
    if (s_led_command_queue == nullptr) {
        ESP_LOGE(TAG, "LED controller is not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(s_led_command_queue, &command, pdMS_TO_TICKS(20)) != pdTRUE) {
        ESP_LOGE(TAG, "LED command queue is full");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static void app_driver_led_task(void *arg)
{
    led_normal_state_t normal_state{};
    led_pattern_t active_pattern{};
    led_phase_t phase = led_phase_t::STATIC;
    uint8_t completed_flashes = 0;
    bool one_shot_active = false;
    TickType_t wait_ticks = portMAX_DELAY;

    app_driver_led_write(false);

    auto start_normal_display = [&]() {
        completed_flashes = 0;

        if (normal_state.type == led_normal_type_t::STATIC) {
            phase = led_phase_t::STATIC;
            app_driver_led_write(normal_state.static_on);
            wait_ticks = portMAX_DELAY;
            return;
        }

        active_pattern = normal_state.pattern;
        phase = led_phase_t::NORMAL_ON;
        app_driver_led_write(true);
        wait_ticks = app_driver_led_ms_to_ticks(active_pattern.on_ms);
    };

    auto start_one_shot = [&](const led_pattern_t &pattern) {
        one_shot_active = true;
        active_pattern = pattern;
        completed_flashes = 0;
        phase = led_phase_t::ONE_SHOT_ON;
        app_driver_led_write(true);
        wait_ticks = app_driver_led_ms_to_ticks(active_pattern.on_ms);
    };

    for (;;) {
        led_command_t command{};

        if (xQueueReceive(s_led_command_queue, &command, wait_ticks) == pdTRUE) {
            switch (command.type) {
                case led_command_type_t::SET_STATIC:
                    normal_state.type = led_normal_type_t::STATIC;
                    normal_state.static_on = command.static_on;

                    // A normal-state change does not interrupt a one-shot.
                    if (!one_shot_active) {
                        start_normal_display();
                    }
                    break;

                case led_command_type_t::SET_PERIODIC:
                    normal_state.type = led_normal_type_t::PERIODIC;
                    normal_state.pattern = command.pattern;

                    // A normal-state change does not interrupt a one-shot.
                    if (!one_shot_active) {
                        start_normal_display();
                    }
                    break;

                case led_command_type_t::ONE_SHOT:
                    // A new one-shot replaces any one-shot already running.
                    start_one_shot(command.pattern);
                    break;

                case led_command_type_t::CANCEL_ONE_SHOT:
                    one_shot_active = false;
                    start_normal_display();
                    break;
            }

            continue;
        }

        // Timeout: advance the currently displayed sequence by one phase.
        switch (phase) {
            case led_phase_t::STATIC:
                wait_ticks = portMAX_DELAY;
                break;

            case led_phase_t::NORMAL_ON:
                app_driver_led_write(false);
                phase = led_phase_t::NORMAL_OFF;
                wait_ticks = app_driver_led_ms_to_ticks(active_pattern.off_ms);
                break;

            case led_phase_t::NORMAL_OFF:
                ++completed_flashes;

                if (completed_flashes < active_pattern.flash_count) {
                    app_driver_led_write(true);
                    phase = led_phase_t::NORMAL_ON;
                    wait_ticks = app_driver_led_ms_to_ticks(active_pattern.on_ms);
                } else if (active_pattern.interval_ms > 0) {
                    phase = led_phase_t::NORMAL_INTERVAL;
                    wait_ticks = app_driver_led_ms_to_ticks(active_pattern.interval_ms);
                } else {
                    completed_flashes = 0;
                    app_driver_led_write(true);
                    phase = led_phase_t::NORMAL_ON;
                    wait_ticks = app_driver_led_ms_to_ticks(active_pattern.on_ms);
                }
                break;

            case led_phase_t::NORMAL_INTERVAL:
                completed_flashes = 0;
                app_driver_led_write(true);
                phase = led_phase_t::NORMAL_ON;
                wait_ticks = app_driver_led_ms_to_ticks(active_pattern.on_ms);
                break;

            case led_phase_t::ONE_SHOT_ON:
                app_driver_led_write(false);
                phase = led_phase_t::ONE_SHOT_OFF;
                wait_ticks = app_driver_led_ms_to_ticks(active_pattern.off_ms);
                break;

            case led_phase_t::ONE_SHOT_OFF:
                ++completed_flashes;

                if (completed_flashes < active_pattern.flash_count) {
                    app_driver_led_write(true);
                    phase = led_phase_t::ONE_SHOT_ON;
                    wait_ticks = app_driver_led_ms_to_ticks(active_pattern.on_ms);
                } else {
                    one_shot_active = false;
                    start_normal_display();
                }
                break;
        }
    }
}

static esp_err_t app_driver_led_controller_init()
{
    if (s_led_command_queue != nullptr && s_led_task_handle != nullptr) {
        return ESP_OK;
    }

    s_led_command_queue = xQueueCreate(8, sizeof(led_command_t));
    if (s_led_command_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create LED command queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t result = xTaskCreate(
        app_driver_led_task,
        "status_led",
        3072,
        nullptr,
        5,
        &s_led_task_handle
    );

    if (result != pdPASS) {
        ESP_LOGE(TAG, "Failed to create LED controller task");
        vQueueDelete(s_led_command_queue);
        s_led_command_queue = nullptr;
        s_led_task_handle = nullptr;
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

esp_err_t app_driver_led_set_static(bool on)
{
    const led_command_t command{
        .type = led_command_type_t::SET_STATIC,
        .static_on = on,
        .pattern = {},
    };

    return app_driver_led_send_command(command);
}

esp_err_t app_driver_led_set_periodic(
    uint8_t flashes_per_cycle,
    uint32_t on_period_ms,
    uint32_t off_period_ms,
    uint32_t cycle_period_ms)
{
    led_pattern_t pattern{};

    if (!app_driver_led_make_pattern(
            flashes_per_cycle,
            on_period_ms,
            off_period_ms,
            cycle_period_ms,
            true,
            &pattern)) {
        ESP_LOGE(TAG,
                 "Invalid periodic LED pattern: flashes=%u on=%lu off=%lu cycle=%lu",
                 static_cast<unsigned>(flashes_per_cycle),
                 static_cast<unsigned long>(on_period_ms),
                 static_cast<unsigned long>(off_period_ms),
                 static_cast<unsigned long>(cycle_period_ms));
        return ESP_ERR_INVALID_ARG;
    }

    const led_command_t command{
        .type = led_command_type_t::SET_PERIODIC,
        .static_on = false,
        .pattern = pattern,
    };

    return app_driver_led_send_command(command);
}

esp_err_t app_driver_led_one_shot(
    uint8_t flash_count,
    uint32_t on_period_ms,
    uint32_t off_period_ms)
{
    led_pattern_t pattern{};

    if (!app_driver_led_make_pattern(
            flash_count,
            on_period_ms,
            off_period_ms,
            0,
            false,
            &pattern)) {
        ESP_LOGE(TAG,
                 "Invalid one-shot LED pattern: flashes=%u on=%lu off=%lu",
                 static_cast<unsigned>(flash_count),
                 static_cast<unsigned long>(on_period_ms),
                 static_cast<unsigned long>(off_period_ms));
        return ESP_ERR_INVALID_ARG;
    }

    const led_command_t command{
        .type = led_command_type_t::ONE_SHOT,
        .static_on = false,
        .pattern = pattern,
    };

    return app_driver_led_send_command(command);
}

static esp_err_t app_driver_led_cancel_one_shot()
{
    const led_command_t command{
        .type = led_command_type_t::CANCEL_ONE_SHOT,
        .static_on = false,
        .pattern = {},
    };

    return app_driver_led_send_command(command);
}

/*
 * Compatibility wrapper for existing application code.
 * New code should prefer the parameterized APIs above.
 */
void app_driver_led_set_mode(led_display_mode_t mode)
{
    esp_err_t err = ESP_OK;

    switch (mode) {
        case LED_MODE_OFF:
            err = app_driver_led_set_static(false);
            break;

        case LED_MODE_ON:
            err = app_driver_led_set_static(true);
            break;

        case LED_MODE_BLINK_1HZ:
            // ON 500 ms, OFF 500 ms; repeats every 1000 ms.
            err = app_driver_led_set_periodic(1, 500, 500, 1000);
            break;

        case LED_MODE_BLINK_3HZ:
            // Approximately 3 Hz: ON 167 ms, OFF 167 ms.
            err = app_driver_led_set_periodic(1, 167, 167, 334);
            break;

        case LED_MODE_BREATH_3S:
            // One short flash every 3 seconds: ON 100 ms, OFF 100 ms.
            err = app_driver_led_set_periodic(1, 100, 100, 3000);
            break;
		case ONE_SHOT_FLASH_3X:
			err = app_driver_led_one_shot(3, 100, 200);
			break;
		case ONE_SHOT_ON_1S:
			err = app_driver_led_one_shot(1, 1000, 1000);
			break;
		case LED_MODE_BLINK_2X_1HZ:
			err = app_driver_led_set_periodic(2, 100, 100, 1000);
			break;
		case LED_MODE_BLINK_1X_1HZ:
			err = app_driver_led_set_periodic(1, 100, 100, 1000);
			break;
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set LED mode %d: %s",
                 static_cast<int>(mode), esp_err_to_name(err));
    }
}


static std::unique_ptr<bc7215::BC7215AC> s_ac;
static bool s_matter_syncing_from_local = false;
static int16_t s_target_temperature_x100 = 2200;
static bool s_bc7215_library_unit_celsius = true;

static TaskHandle_t s_bc7215_worker_task_handle = nullptr;
static TaskHandle_t s_factory_reset_task_handle = nullptr;
static QueueHandle_t s_bc7215_command_queue = nullptr;

static std::atomic_bool s_factory_reset_armed{false};
static std::atomic_bool s_factory_reset_in_progress{false};
static std::atomic_bool s_bc7215_pairing{false};
static std::atomic_bool s_bc7215_paired{false};
static std::atomic_bool s_matter_subscription_active{false};
static std::atomic_bool s_bc7215_last_clear_success{false};
static std::atomic_bool s_bc7215_alt_traversal_active{false};

enum class bc7215_alt_phase_t : uint8_t {
    INACTIVE,
    MATCH_NEXT,
    PREDEFINED,
};

static bc7215_alt_phase_t s_bc7215_alt_phase =
    bc7215_alt_phase_t::INACTIVE;
static uint8_t s_bc7215_match_index = 0;
static uint8_t s_bc7215_alt_predefined_number = 0;

enum class bc7215_worker_state_t : uint8_t {
    STOPPED,
    IDLE,
    PARSING,
    PAIRING,
};

enum class bc7215_command_type_t : uint8_t {
    SEND_POWER,
    SEND_STATE,
    TOGGLE_PAIRING,
    ALT_NEXT,
    CLEAR_PAIRING,
};

struct bc7215_command_t {
    bc7215_command_type_t type =
        bc7215_command_type_t::SEND_STATE;

    int temp = 25;
    int mode = 1;
    int fan = 1;
    int key = 0;
    bool power_on = false;

    /*
     * Used only by CLEAR_PAIRING. The worker notifies this task after the
     * BC7215 runtime state and saved pairing record have both been cleared.
     */
    TaskHandle_t completion_task = nullptr;
};

static bc7215_worker_state_t s_bc7215_worker_state =
    bc7215_worker_state_t::STOPPED;

static int Temp = 25;
static int Mode = 1;
static int Fan = 1;
static bool PowerOn = false;

/*
 * Fan endpoint synchronization helpers.
 *
 * FanMode Off and PercentSetting 0% are treated as whole-appliance power-off
 * commands. Non-zero percentages are normalized to 25%, 50% or 100%.
 */
static uint8_t app_driver_bc7215_fan_to_matter(
    int fan,
    bool power_on);

static uint8_t app_driver_bc7215_fan_to_percent(
    int fan,
    bool power_on);

static void app_matter_schedule_whole_device_state(
    bool power_on,
    int mode,
    int fan);

/*
 * Storage for BC7215 air-conditioner protocol pairing data.
 *
 * NVS already performs integrity checks on storage pages. The additional
 * magic, version and size fields prevent old data from being used after
 * firmware updates change the structure layout.
 */
static constexpr char BC7215_NVS_NAMESPACE[] = "bc7215_ac";
static constexpr char BC7215_NVS_PAIRING_KEY[] = "pairing";
static constexpr uint32_t BC7215_PAIRING_MAGIC = 0x42433732; // "BC72"
static constexpr uint16_t BC7215_PAIRING_VERSION = 1;

struct bc7215_stored_pairing_data_t {
    uint32_t magic;
    uint16_t version;
    uint16_t struct_size;
    uint8_t status;
    uint8_t library_unit_celsius;
    uint8_t match_index;
    uint8_t reserved;
    bc7215DataMaxPkt_t data;
    bc7215FormatPkt_t format;
};


/*
 * Queue a command for the single BC7215 worker.
 *
 * After the worker has started, no other task may directly call BC7215AC
 * capture, parsing, pairing or transmit methods.
 */
static esp_err_t app_driver_bc7215_enqueue_command(
    const bc7215_command_t &command,
    TickType_t timeout = pdMS_TO_TICKS(50))
{
    if (s_bc7215_command_queue == nullptr) {
        ESP_LOGE(
            TAG_BC7215,
            "BC7215 worker command queue is unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(
            s_bc7215_command_queue,
            &command,
            timeout) != pdTRUE) {

        ESP_LOGE(
            TAG_BC7215,
            "BC7215 worker command queue is full");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static esp_err_t app_driver_bc7215_queue_power(
    bool power_on)
{
    bc7215_command_t command{};
    command.type =
        bc7215_command_type_t::SEND_POWER;
    command.power_on = power_on;

    return app_driver_bc7215_enqueue_command(
        command);
}

static esp_err_t app_driver_bc7215_queue_state(
    int temp,
    int mode,
    int fan,
    int key,
    bool power_on)
{
    bc7215_command_t command{};
    command.type =
        bc7215_command_type_t::SEND_STATE;
    command.temp = temp;
    command.mode = mode;
    command.fan = fan;
    command.key = key;
    command.power_on = power_on;

    return app_driver_bc7215_enqueue_command(
        command);
}

static esp_err_t app_driver_bc7215_queue_pairing_toggle()
{
    bc7215_command_t command{};
    command.type =
        bc7215_command_type_t::TOGGLE_PAIRING;

    return app_driver_bc7215_enqueue_command(
        command);
}

static esp_err_t app_driver_bc7215_queue_alt_next()
{
    bc7215_command_t command{};
    command.type =
        bc7215_command_type_t::ALT_NEXT;

    return app_driver_bc7215_enqueue_command(
        command);
}


/*
 * Called by the Matter subscription callback.
 * The BC7215 worker starts/stops parsing RX according to this flag.
 */
void app_driver_set_subscription_active(bool active)
{
    const bool previous =
        s_matter_subscription_active.exchange(active);

    if (previous != active) {
        ESP_LOGI(TAG_BC7215,
                 "Matter subscription state: %s",
                 active ? "active" : "inactive");
    }
}

/* Do any conversions/remapping for the actual value here */
static void app_driver_log_current_ac_state(const char *source)
{
    ESP_LOGI(TAG,
             "AC state [%s]: Temp=%d, Mode=%d, Fan=%d, Power=%s",
             source,
             Temp,
             Mode,
             Fan,
             PowerOn ? "On" : "Off");
}

/* Do any conversions/remapping for the actual value here */
static esp_err_t app_driver_room_air_conditioner_set_power(
    esp_matter_attr_val_t *val)
{
    if (!s_bc7215_alt_traversal_active.load()) {
        app_driver_led_set_mode(
            ONE_SHOT_ON_1S);
    }

    PowerOn = val->val.b;

    esp_err_t queue_err = ESP_OK;

    if (!s_factory_reset_in_progress.load()) {
        queue_err =
            app_driver_bc7215_queue_power(
                PowerOn);

        if (queue_err != ESP_OK) {
            ESP_LOGE(
                TAG_BC7215,
                "Failed to queue Room AC power command: %s",
                esp_err_to_name(queue_err));
        }
    }

    /*
     * OnOff, Thermostat SystemMode and the separate Fan endpoint all
     * represent the same physical appliance. Publish one complete state so
     * every subscribed controller sees consistent values.
     *
     * When PowerOn becomes true, Mode still contains the last active
     * non-Off mode and is therefore used to restore SystemMode.
     */
    app_matter_schedule_whole_device_state(
        PowerOn,
        Mode,
        Fan);

    ESP_LOGI(
        TAG,
        "Set power: %d",
        static_cast<int>(PowerOn));

    app_driver_log_current_ac_state(
        "RoomAC OnOff");

    /*
     * The LED is dedicated to displaying the BC7215 pairing state:
     *   Unpaired       -> LED_MODE_BLINK_1HZ
     *   Pairing        -> LED_MODE_BLINK_3HZ
     *   Pairing complete -> LED_MODE_BREATH_3S
     *
     * Therefore, the LED is no longer changed according to the Matter OnOff
     * attribute. This prevents AC power operations or default-value
     * synchronization from overriding the pairing-state indication.
     */

    return queue_err;
}

static esp_err_t app_matter_update_display_temperature(
    int16_t temp_x100)
{
    esp_matter_attr_val_t local_temp =
        esp_matter_invalid(nullptr);

    /*
     * LocalTemperature is a nullable int16 attribute.
     * Even when writing a non-null value, the correct attribute type must
     * still be used.
     *
     * This hardware has no ambient sensor. The value reported here is the
     * commanded target temperature so phone UIs have something to show; it
     * is not a measured room temperature.
     */
    local_temp.type =
        ESP_MATTER_VAL_TYPE_NULLABLE_INT16;

    local_temp.val.i16 = temp_x100;

    s_matter_syncing_from_local = true;

    esp_err_t err = attribute::report(
        room_air_conditioner_endpoint_id,
        Thermostat::Id,
        Thermostat::Attributes::LocalTemperature::Id,
        &local_temp
    );

    s_matter_syncing_from_local = false;

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to update displayed temperature: %s",
            esp_err_to_name(err)
        );
    }

    return err;
}

static esp_err_t app_matter_report_all_temperatures_now(
    int16_t temp_x100)
{
    esp_err_t first_error = ESP_OK;

    const uint32_t setpoint_attributes[] = {
        Thermostat::Attributes::
            OccupiedCoolingSetpoint::Id,

        Thermostat::Attributes::
            OccupiedHeatingSetpoint::Id
    };

    for (uint32_t attribute_id :
         setpoint_attributes) {

        esp_matter_attr_val_t val =
            esp_matter_int16(temp_x100);

        esp_err_t err = attribute::report(
            room_air_conditioner_endpoint_id,
            Thermostat::Id,
            attribute_id,
            &val
        );

        if (err != ESP_OK &&
            first_error == ESP_OK) {
            first_error = err;
        }
    }

    esp_err_t local_err =
        app_matter_update_display_temperature(
            temp_x100);

    if (local_err != ESP_OK &&
        first_error == ESP_OK) {
        first_error = local_err;
    }

    return first_error;
}

static void app_matter_schedule_report_all_temperatures(
    int16_t temp_x100)
{
    chip::DeviceLayer::SystemLayer().ScheduleLambda(
        [temp_x100]() {
            esp_err_t err =
                app_matter_report_all_temperatures_now(
                    temp_x100);

            if (err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Scheduled temperature synchronization "
                    "failed: %s",
                    esp_err_to_name(err)
                );
            }
        }
    );
}


static uint8_t app_driver_bc7215_mode_to_matter(int mode)
{
    switch (mode) {
        case 0: return 1; // Auto
        case 1: return 3; // Cool
        case 2: return 4; // Heat
        case 3: return 8; // Dry
        case 4: return 7; // Fan only
        default: return 0; // Off/invalid fallback
    }
}

static uint8_t app_driver_bc7215_fan_to_matter(int fan, bool power_on)
{
    if (!power_on) {
        return 0; // Off
    }

    switch (fan) {
        case 0: return 5; // Auto
        case 1: return 1; // Low
        case 2: return 2; // Medium
        case 3: return 3; // High
        default: return 5;
    }
}

static uint8_t app_driver_bc7215_fan_to_percent(int fan, bool power_on)
{
    if (!power_on) {
        return 0;
    }

    switch (fan) {
        case 1: return 25;  // Low
        case 2: return 50;  // Medium
        case 3: return 100; // High
        case 0:
        default:
            /*
             * Auto has no exact fixed percentage. FanMode=Auto remains the
             * authoritative value; use 100% to keep the endpoint active.
             */
            return 100;
    }
}

static int app_driver_percent_to_bc7215_fan(uint8_t percent)
{
    /*
     * PercentSetting=0% is handled as whole-appliance Off before this helper
     * is called. The remaining non-zero range is normalized as follows:
     *   1..33%   -> Low
     *   34..66%  -> Medium
     *   67..100% -> High
     */
    if (percent <= 33) {
        return 1;
    }

    if (percent <= 66) {
        return 2;
    }

    return 3;
}

static void app_matter_log_update_error(
    const char *name,
    esp_err_t err)
{
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "Failed to synchronize parsed %s: %s",
                 name,
                 esp_err_to_name(err));
    }
}

/*
 * Update all Fan Control attributes as one logical state.
 *
 * The previous synchronization guard is restored so this helper can be used
 * both from a standalone scheduled lambda and from another local update block.
 */
static void app_matter_update_fan_endpoint_state_now(
    uint8_t matter_fan_mode,
    uint8_t fan_percent)
{
    const bool previous_sync_state =
        s_matter_syncing_from_local;

    s_matter_syncing_from_local = true;

    esp_matter_attr_val_t fan_mode_val =
        esp_matter_enum8(matter_fan_mode);

    app_matter_log_update_error(
        "FanMode",
        attribute::update(
            fan_endpoint_id,
            FanControl::Id,
            FanControl::Attributes::FanMode::Id,
            &fan_mode_val));

    esp_matter_attr_val_t percent_current =
        esp_matter_uint8(fan_percent);

    app_matter_log_update_error(
        "PercentCurrent",
        attribute::update(
            fan_endpoint_id,
            FanControl::Id,
            FanControl::Attributes::PercentCurrent::Id,
            &percent_current));

    esp_matter_attr_val_t percent_setting =
        esp_matter_nullable_uint8(fan_percent);

    app_matter_log_update_error(
        "PercentSetting",
        attribute::update(
            fan_endpoint_id,
            FanControl::Id,
            FanControl::Attributes::PercentSetting::Id,
            &percent_setting));

    s_matter_syncing_from_local =
        previous_sync_state;

    ESP_LOGI(
        TAG,
        "Fan endpoint synchronized: FanMode=%u Percent=%u",
        static_cast<unsigned>(matter_fan_mode),
        static_cast<unsigned>(fan_percent));
}

/*
 * Update every Matter attribute that represents the appliance's logical
 * power/mode/fan state. This must run on the CHIP thread.
 *
 * PowerOn is kept separate from Mode. Therefore an Off command does not erase
 * the last active BC7215 mode, and a later Fan-originated On command can
 * restore Thermostat::SystemMode correctly.
 */
static void app_matter_update_whole_device_state_now(
    bool power_on,
    int mode,
    int fan)
{
    const bool previous_sync_state =
        s_matter_syncing_from_local;

    s_matter_syncing_from_local = true;

    esp_matter_attr_val_t power_val =
        esp_matter_bool(power_on);

    app_matter_log_update_error(
        "RoomAC OnOff",
        attribute::update(
            room_air_conditioner_endpoint_id,
            OnOff::Id,
            OnOff::Attributes::OnOff::Id,
            &power_val));

    const uint8_t matter_system_mode =
        power_on
            ? app_driver_bc7215_mode_to_matter(mode)
            : static_cast<uint8_t>(
                  Thermostat::SystemModeEnum::kOff);

    esp_matter_attr_val_t system_mode_val =
        esp_matter_enum8(matter_system_mode);

    app_matter_log_update_error(
        "Thermostat SystemMode",
        attribute::update(
            room_air_conditioner_endpoint_id,
            Thermostat::Id,
            Thermostat::Attributes::SystemMode::Id,
            &system_mode_val));

    const uint8_t matter_fan_mode =
        app_driver_bc7215_fan_to_matter(
            fan,
            power_on);

    const uint8_t fan_percent =
        app_driver_bc7215_fan_to_percent(
            fan,
            power_on);

    app_matter_update_fan_endpoint_state_now(
        matter_fan_mode,
        fan_percent);

    s_matter_syncing_from_local =
        previous_sync_state;

    ESP_LOGI(
        TAG,
        "Whole device synchronized: Power=%s Mode=%d Fan=%d "
        "SystemMode=%u FanMode=%u Percent=%u",
        power_on ? "On" : "Off",
        mode,
        fan,
        static_cast<unsigned>(matter_system_mode),
        static_cast<unsigned>(matter_fan_mode),
        static_cast<unsigned>(fan_percent));
}

static void app_matter_schedule_whole_device_state(
    bool power_on,
    int mode,
    int fan)
{
    chip::DeviceLayer::SystemLayer().ScheduleLambda(
        [power_on, mode, fan]() {
            app_matter_update_whole_device_state_now(
                power_on,
                mode,
                fan);
        });
}

/*
 * Runs on the CHIP thread. attribute::update() changes the Matter data model;
 * active subscriptions then report the new values to the controller.
 */
static void app_matter_apply_parsed_ac_state_now(
    int temp_c,
    int mode,
    int fan,
    bool power_on)
{
    s_matter_syncing_from_local = true;

    esp_matter_attr_val_t power_val =
        esp_matter_bool(power_on);

    app_matter_log_update_error(
        "RoomAC OnOff",
        attribute::update(
            room_air_conditioner_endpoint_id,
            OnOff::Id,
            OnOff::Attributes::OnOff::Id,
            &power_val));

    const uint8_t matter_system_mode =
        power_on
            ? app_driver_bc7215_mode_to_matter(mode)
            : 0;

    esp_matter_attr_val_t system_mode_val =
        esp_matter_enum8(matter_system_mode);

    app_matter_log_update_error(
        "Thermostat SystemMode",
        attribute::update(
            room_air_conditioner_endpoint_id,
            Thermostat::Id,
            Thermostat::Attributes::SystemMode::Id,
            &system_mode_val));

    if (temp_c >= 16 && temp_c <= 30) {
        const int16_t temp_x100 =
            static_cast<int16_t>(temp_c * 100);

        esp_matter_attr_val_t setpoint_val =
            esp_matter_int16(temp_x100);

        app_matter_log_update_error(
            "OccupiedCoolingSetpoint",
            attribute::update(
                room_air_conditioner_endpoint_id,
                Thermostat::Id,
                Thermostat::Attributes::
                    OccupiedCoolingSetpoint::Id,
                &setpoint_val));

        setpoint_val = esp_matter_int16(temp_x100);
        app_matter_log_update_error(
            "OccupiedHeatingSetpoint",
            attribute::update(
                room_air_conditioner_endpoint_id,
                Thermostat::Id,
                Thermostat::Attributes::
                    OccupiedHeatingSetpoint::Id,
                &setpoint_val));

        esp_matter_attr_val_t local_temp =
            esp_matter_invalid(nullptr);
        local_temp.type =
            ESP_MATTER_VAL_TYPE_NULLABLE_INT16;
        /*
         * No ambient sensor: report the commanded target as
         * LocalTemperature so subscribed controllers stay consistent.
         */
        local_temp.val.i16 = temp_x100;

        app_matter_log_update_error(
            "LocalTemperature",
            attribute::update(
                room_air_conditioner_endpoint_id,
                Thermostat::Id,
                Thermostat::Attributes::
                    LocalTemperature::Id,
                &local_temp));
    }

    const uint8_t matter_fan_mode =
        app_driver_bc7215_fan_to_matter(
            fan,
            power_on);

    const uint8_t fan_percent =
        app_driver_bc7215_fan_to_percent(
            fan,
            power_on);

    /*
     * Parsed/actual AC power state is authoritative:
     *   AC off -> FanMode Off, 0%
     *   AC on  -> current fan mode and normalized percentage
     */
    app_matter_update_fan_endpoint_state_now(
        matter_fan_mode,
        fan_percent);

    s_matter_syncing_from_local = false;

    ESP_LOGI(TAG,
             "Parsed AC state synchronized: Temp=%dC Mode=%d "
             "Fan=%d Power=%s",
             temp_c,
             mode,
             fan,
             power_on ? "On" : "Off");
}

static void app_matter_schedule_parsed_ac_state(
    int temp_c,
    int mode,
    int fan,
    bool power_on)
{
    chip::DeviceLayer::SystemLayer().ScheduleLambda(
        [temp_c, mode, fan, power_on]() {
            app_matter_apply_parsed_ac_state_now(
                temp_c,
                mode,
                fan,
                power_on);
        });
}

/*
 * Convert the BC7215 library temperature to the Celsius value required by
 * Matter. Invalid parsed temperatures leave the previous temperature intact.
 */
static int app_driver_bc7215_temperature_to_celsius(int parsed_temp)
{
    if (s_bc7215_library_unit_celsius) {
        return (parsed_temp >= 16 &&
                parsed_temp <= 30)
            ? parsed_temp
            : Temp;
    }

    if (parsed_temp < 60 || parsed_temp > 88) {
        return Temp;
    }

    return static_cast<int>(
        std::lround(
            (static_cast<float>(parsed_temp) - 32.0f)
            * 5.0f / 9.0f));
}


static bool app_driver_bc7215_save_pairing();
static bool app_driver_bc7215_erase_saved_pairing();
static bool app_driver_bc7215_load_pairing();

static bool app_driver_bc7215_worker_wait_idle(
    uint32_t timeout_ms = 3000)
{
    if (!s_ac) {
        return false;
    }

    const int64_t deadline_us =
        esp_timer_get_time() +
        static_cast<int64_t>(timeout_ms) * 1000;

    while (s_ac->is_busy()) {
        if (esp_timer_get_time() >= deadline_us) {
            ESP_LOGW(
                TAG_BC7215,
                "Timed out waiting for BC7215 to become idle");
            return false;
        }

        vTaskDelay(
            pdMS_TO_TICKS(20));
    }

    return true;
}

static void app_driver_bc7215_worker_stop_capture()
{
    if (!s_ac) {
        return;
    }

    s_ac->stop_capture();

    s_bc7215_worker_state =
        bc7215_worker_state_t::IDLE;
}

/*
 * The Alt protocol traversal is advanced one candidate per double click.
 *
 * MATCH_NEXT candidates use short flashes:
 *   ON 100 ms, OFF 1500 ms.
 *
 * Predefined protocols use long flashes:
 *   ON 500 ms, OFF 1100 ms.
 *
 * The number of flashes is the 1-based candidate number.
 */
static void app_driver_bc7215_alt_reset_tracking()
{
    s_bc7215_alt_traversal_active.store(false);
    s_bc7215_alt_phase =
        bc7215_alt_phase_t::INACTIVE;
    s_bc7215_alt_predefined_number = 0;
}

static void app_driver_bc7215_alt_prepare_led()
{
    app_driver_led_cancel_one_shot();
    app_driver_led_set_static(false);
}

static void app_driver_bc7215_alt_show_match_number(
    uint8_t number)
{
    app_driver_led_one_shot(
        number,
        100,
        1500);
}

static void app_driver_bc7215_alt_show_predefined_number(
    uint8_t number)
{
    app_driver_led_one_shot(
        number,
        500,
        1100);
}

static bool app_driver_bc7215_worker_send_alt_test_signal()
{
    if (!s_ac || !s_ac->init_ok) {
        return false;
    }

    if (!app_driver_bc7215_worker_wait_idle()) {
        return false;
    }

    const int test_temperature =
        s_bc7215_library_unit_celsius
            ? 25
            : 77;

    const bc7215DataVarPkt_t *packet =
        s_ac->set_to(
            test_temperature,
            1,
            0);

    if (packet == nullptr) {
        ESP_LOGE(
            TAG_BC7215,
            "Alt traversal test command failed");
        return false;
    }

    Temp = 25;
    Mode = 1;
    Fan = 0;
    PowerOn = true;

    ESP_LOGI(
        TAG_BC7215,
        "Alt traversal test command sent: Cool/25C/Fan Auto");

    return true;
}

static void app_driver_bc7215_worker_accept_alt_selection()
{
    if (!s_bc7215_alt_traversal_active.load()) {
        return;
    }

    const bc7215_alt_phase_t accepted_phase =
        s_bc7215_alt_phase;
    const uint8_t accepted_number =
        accepted_phase ==
                bc7215_alt_phase_t::MATCH_NEXT
            ? s_bc7215_match_index
            : s_bc7215_alt_predefined_number;

    app_driver_bc7215_alt_reset_tracking();
    app_driver_led_cancel_one_shot();
    app_driver_led_set_mode(
        LED_MODE_BREATH_3S);

    ESP_LOGI(
        TAG_BC7215,
        "Alt traversal selection accepted by Matter command: "
        "type=%s number=%u",
        accepted_phase ==
                bc7215_alt_phase_t::MATCH_NEXT
            ? "match_next"
            : "predefined",
        static_cast<unsigned>(
            accepted_number));
}

static void app_driver_bc7215_worker_finish_alt_unpaired()
{
    if (s_ac) {
        s_ac->stop_capture();
        s_ac->init_ok = false;
    }

    s_bc7215_pairing.store(false);
    s_bc7215_paired.store(false);
    s_bc7215_match_index = 0;
    s_bc7215_worker_state =
        bc7215_worker_state_t::STOPPED;

    if (!app_driver_bc7215_erase_saved_pairing()) {
        ESP_LOGW(
            TAG_BC7215,
            "Alt traversal finished, but saved pairing data "
            "could not be erased");
    }

    app_driver_bc7215_alt_reset_tracking();
    app_driver_led_cancel_one_shot();
    app_driver_update_led_states();

    ESP_LOGI(
        TAG_BC7215,
        "Alt traversal completed; device returned to the unpaired state");
}

static void app_driver_bc7215_worker_commit_alt_candidate(
    const char *candidate_type,
    uint8_t candidate_number,
    bool predefined)
{
    s_bc7215_paired.store(true);
    s_bc7215_worker_state =
        bc7215_worker_state_t::IDLE;

    const bool test_sent =
        app_driver_bc7215_worker_send_alt_test_signal();

    const bool saved =
        app_driver_bc7215_save_pairing();

    if (predefined) {
        app_driver_bc7215_alt_show_predefined_number(
            candidate_number);
    } else {
        app_driver_bc7215_alt_show_match_number(
            candidate_number);
    }

    ESP_LOGI(
        TAG_BC7215,
        "Alt traversal candidate selected: type=%s number=%u "
        "saved=%d test_sent=%d",
        candidate_type,
        static_cast<unsigned>(
            candidate_number),
        static_cast<int>(saved),
        static_cast<int>(test_sent));
}

static void app_driver_bc7215_worker_alt_next()
{
    if (!s_ac ||
        s_factory_reset_in_progress.load()) {
        ESP_LOGW(
            TAG_BC7215,
            "Alt traversal ignored: BC7215 unavailable or reset active");
        return;
    }

    if (s_bc7215_pairing.load()) {
        s_ac->stop_capture();
        s_bc7215_pairing.store(false);
        s_ac->init_ok = false;
        s_bc7215_paired.store(false);
    }

    if (!s_bc7215_alt_traversal_active.load()) {
        s_bc7215_alt_traversal_active.store(true);
        s_bc7215_alt_predefined_number = 0;

        s_bc7215_alt_phase =
            s_bc7215_paired.load() &&
                    s_ac->init_ok
                ? bc7215_alt_phase_t::MATCH_NEXT
                : bc7215_alt_phase_t::PREDEFINED;

        if (s_bc7215_alt_phase ==
            bc7215_alt_phase_t::PREDEFINED) {
            s_bc7215_match_index = 0;
        }

        app_driver_bc7215_alt_prepare_led();

        ESP_LOGI(
            TAG_BC7215,
            "Alt traversal started: first phase=%s",
            s_bc7215_alt_phase ==
                    bc7215_alt_phase_t::MATCH_NEXT
                ? "match_next"
                : "predefined");
    } else {
        app_driver_bc7215_alt_prepare_led();
    }

    app_driver_bc7215_worker_stop_capture();

    if (!app_driver_bc7215_worker_wait_idle()) {
        return;
    }

    if (s_bc7215_alt_phase ==
        bc7215_alt_phase_t::MATCH_NEXT) {

        if (s_ac->match_next()) {
            ++s_bc7215_match_index;

            app_driver_bc7215_worker_commit_alt_candidate(
                "match_next",
                s_bc7215_match_index,
                false);
            return;
        }

        s_bc7215_alt_phase =
            bc7215_alt_phase_t::PREDEFINED;
        s_bc7215_match_index = 0;
        s_bc7215_alt_predefined_number = 0;

        ESP_LOGI(
            TAG_BC7215,
            "No more match_next candidates; switching to "
            "predefined protocols");
    }

    const uint8_t predefined_count =
        s_ac->predefined_count();

    while (s_bc7215_alt_predefined_number <
           predefined_count) {

        const uint8_t predefined_index =
            s_bc7215_alt_predefined_number;

        ++s_bc7215_alt_predefined_number;

        if (!s_ac->init_predefined(
                predefined_index)) {

            ESP_LOGW(
                TAG_BC7215,
                "Failed to initialize predefined protocol %u",
                static_cast<unsigned>(
                    s_bc7215_alt_predefined_number));
            continue;
        }

        app_driver_bc7215_worker_commit_alt_candidate(
            "predefined",
            s_bc7215_alt_predefined_number,
            true);
        return;
    }

    app_driver_bc7215_worker_finish_alt_unpaired();
}

static void app_driver_bc7215_worker_send_power(
    const bc7215_command_t &command)
{
    if (!s_ac ||
        !s_ac->init_ok ||
        s_bc7215_pairing.load() ||
        s_factory_reset_in_progress.load()) {

        ESP_LOGW(
            TAG_BC7215,
            "Power command ignored: ready=%d paired=%d pairing=%d reset=%d",
            s_ac != nullptr,
            s_bc7215_paired.load(),
            s_bc7215_pairing.load(),
            s_factory_reset_in_progress.load());
        return;
    }

    app_driver_bc7215_worker_accept_alt_selection();
    app_driver_bc7215_worker_stop_capture();

    if (!app_driver_bc7215_worker_wait_idle()) {
        return;
    }

    const bc7215DataVarPkt_t *packet =
        command.power_on
            ? s_ac->on()
            : s_ac->off();

    if (packet == nullptr) {
        ESP_LOGE(
            TAG_BC7215,
            "BC7215 power command failed: Power=%d",
            static_cast<int>(
                command.power_on));
    } else {
        ESP_LOGI(
            TAG_BC7215,
            "BC7215 power command submitted: Power=%d",
            static_cast<int>(
                command.power_on));
    }

    /*
     * Match ESPHome's send_current_state_(): after transmit, return the
     * receive state machine to IDLE. The worker will not re-arm capture until
     * BUSY becomes inactive.
     */
    s_bc7215_worker_state =
        bc7215_worker_state_t::IDLE;
}

static void app_driver_bc7215_worker_send_state(
    const bc7215_command_t &command)
{
    if (!s_ac ||
        !s_ac->init_ok ||
        s_bc7215_pairing.load() ||
        s_factory_reset_in_progress.load()) {

        ESP_LOGW(
            TAG_BC7215,
            "State command ignored: ready=%d paired=%d pairing=%d reset=%d",
            s_ac != nullptr,
            s_bc7215_paired.load(),
            s_bc7215_pairing.load(),
            s_factory_reset_in_progress.load());
        return;
    }

    app_driver_bc7215_worker_accept_alt_selection();
    app_driver_bc7215_worker_stop_capture();

    if (!app_driver_bc7215_worker_wait_idle()) {
        return;
    }

    const bc7215DataVarPkt_t *packet =
        s_ac->set_to(
            command.temp,
            command.mode,
            command.fan,
            command.key);

    if (packet == nullptr) {
        ESP_LOGE(
            TAG_BC7215,
            "BC7215 state command failed: "
            "Temp=%d Mode=%d Fan=%d Key=%d Power=%d",
            command.temp,
            command.mode,
            command.fan,
            command.key,
            static_cast<int>(
                command.power_on));
    } else {
        ESP_LOGI(
            TAG_BC7215,
            "BC7215 state command submitted: "
            "Temp=%d Mode=%d Fan=%d Key=%d Power=%d",
            command.temp,
            command.mode,
            command.fan,
            command.key,
            static_cast<int>(
                command.power_on));
    }

    s_bc7215_worker_state =
        bc7215_worker_state_t::IDLE;
}

static void app_driver_bc7215_worker_start_pairing()
{
    if (!s_ac ||
        s_factory_reset_in_progress.load()) {
        return;
    }

    if (s_bc7215_alt_traversal_active.load()) {
        app_driver_bc7215_alt_reset_tracking();
        app_driver_led_cancel_one_shot();
    }

    app_driver_bc7215_worker_stop_capture();

    if (!app_driver_bc7215_worker_wait_idle()) {
        ESP_LOGE(
            TAG_BC7215,
            "Cannot start pairing while BC7215 remains busy");
        return;
    }

    s_ac->init_ok = false;
    s_bc7215_paired.store(false);
    s_bc7215_match_index = 0;
    s_bc7215_pairing.store(true);

    s_ac->start_capture(1);

    s_bc7215_worker_state =
        bc7215_worker_state_t::PAIRING;

    ESP_LOGI(
        TAG_BC7215,
        "Pairing started. Set the remote to Cool/25C and press Fan.");

    app_driver_led_set_mode(
        LED_MODE_BLINK_3HZ);
}

static void app_driver_bc7215_worker_cancel_pairing()
{
    if (!s_ac ||
        !s_bc7215_pairing.load()) {
        return;
    }

    if (s_bc7215_worker_state ==
        bc7215_worker_state_t::PAIRING) {

        s_ac->stop_capture();
    }

    s_ac->init_ok = false;
    s_bc7215_pairing.store(false);

    const bool pairing_restored =
        app_driver_bc7215_load_pairing();

    s_bc7215_paired.store(
        pairing_restored);

    s_bc7215_worker_state =
        pairing_restored
            ? bc7215_worker_state_t::IDLE
            : bc7215_worker_state_t::STOPPED;

    ESP_LOGI(
        TAG_BC7215,
        "BC7215 pairing cancelled; load_pairing result: %s",
        pairing_restored
            ? "paired"
            : "unpaired");

    app_driver_update_led_states();
}

static void app_driver_bc7215_worker_toggle_pairing()
{
    if (s_bc7215_pairing.load()) {
        app_driver_bc7215_worker_cancel_pairing();
    } else {
        app_driver_bc7215_worker_start_pairing();
    }
}

static void app_driver_bc7215_worker_clear_pairing(
    const bc7215_command_t &command)
{
    app_driver_bc7215_alt_reset_tracking();
    app_driver_led_cancel_one_shot();

    if (s_ac) {
        app_driver_bc7215_worker_stop_capture();
        app_driver_bc7215_worker_wait_idle();
        s_ac->init_ok = false;
    }

    s_bc7215_pairing.store(false);
    s_bc7215_paired.store(false);
    s_bc7215_worker_state =
        bc7215_worker_state_t::STOPPED;

    const bool erased =
        app_driver_bc7215_erase_saved_pairing();

    s_bc7215_last_clear_success.store(
        erased);

    app_driver_update_led_states();

    if (command.completion_task != nullptr) {
        xTaskNotifyGive(
            command.completion_task);
    }
}

static void app_driver_bc7215_worker_process_command(
    const bc7215_command_t &command)
{
    switch (command.type) {
        case bc7215_command_type_t::SEND_POWER:
            app_driver_bc7215_worker_send_power(
                command);
            break;

        case bc7215_command_type_t::SEND_STATE:
            app_driver_bc7215_worker_send_state(
                command);
            break;

        case bc7215_command_type_t::TOGGLE_PAIRING:
            app_driver_bc7215_worker_toggle_pairing();
            break;

        case bc7215_command_type_t::ALT_NEXT:
            app_driver_bc7215_worker_alt_next();
            break;

        case bc7215_command_type_t::CLEAR_PAIRING:
            app_driver_bc7215_worker_clear_pairing(
                command);
            break;
    }
}

static void app_driver_bc7215_worker_handle_pairing()
{
    if (!s_ac ||
        !s_bc7215_pairing.load() ||
        s_bc7215_worker_state !=
            bc7215_worker_state_t::PAIRING) {
        return;
    }

    if (!s_ac->signal_captured()) {
        return;
    }

    ESP_LOGI(
        TAG_BC7215,
        "IR signal captured, initializing AC protocol...");

    s_ac->stop_capture();

    const bool init_ok =
        s_ac->init();

    s_bc7215_pairing.store(false);
    app_driver_bc7215_alt_reset_tracking();

    if (init_ok &&
        !s_factory_reset_in_progress.load()) {

        Temp = 25;
        Mode = 1;
        Fan = 1;
        PowerOn = false;
        s_bc7215_match_index = 0;

        s_bc7215_paired.store(true);
        s_bc7215_worker_state =
            bc7215_worker_state_t::IDLE;

        ESP_LOGI(
            TAG_BC7215,
            "BC7215AC pairing succeeded");

        if (!app_driver_bc7215_save_pairing()) {
            ESP_LOGW(
                TAG_BC7215,
                "Pairing succeeded, but saving pairing data failed");
        }

    } else {
        s_ac->init_ok = false;
        s_bc7215_paired.store(false);
        s_bc7215_worker_state =
            bc7215_worker_state_t::STOPPED;

        ESP_LOGE(
            TAG_BC7215,
            "BC7215AC pairing failed after IR capture");

        if (!app_driver_bc7215_erase_saved_pairing()) {
            ESP_LOGE(
                TAG_BC7215,
                "Pairing failed and saved pairing data could not be erased");
        }
    }

    app_driver_update_led_states();
}

static void app_driver_bc7215_worker_parse_signal()
{
    if (!s_ac) {
        return;
    }

    bool parsed_ok = false;

    int parsed_temp = -1;
    int parsed_mode = -1;
    int parsed_fan = -1;
    int parsed_power = -1;

    bool have_previous_base = false;
    uint8_t previous_status = 0;
    bc7215DataMaxPkt_t previous_base{};

    s_ac->stop_capture();

    const bc7215DataVarPkt_t *base_before_parse =
        s_ac->data_packet();

    if (base_before_parse != nullptr) {
        previous_status =
            s_ac->status_byte();

        previous_base =
            *(reinterpret_cast<
                const bc7215DataMaxPkt_t *>(
                    base_before_parse));

        have_previous_base = true;
    }

    parsed_ok =
        s_ac->parse(
            parsed_temp,
            parsed_mode,
            parsed_fan,
            parsed_power);

    if (parsed_ok) {
        const int new_temp =
            app_driver_bc7215_temperature_to_celsius(
                parsed_temp);

        if (parsed_mode >= 0 &&
            parsed_mode <= 4) {
            Mode = parsed_mode;
        }

        if (parsed_fan >= 0 &&
            parsed_fan <= 3) {
            Fan = parsed_fan;
        }

        Temp = new_temp;

        switch (parsed_power) {
            case 0:
                PowerOn = false;
                break;

            case 1:
                PowerOn = true;
                break;

            case 2:
                PowerOn = !PowerOn;
                break;

            default:
                PowerOn = true;
                break;
        }

        if (!PowerOn &&
            have_previous_base) {

            bc7215_ac_replace_base(
                previous_status,
                reinterpret_cast<
                    const bc7215DataVarPkt_t *>(
                        &previous_base));
        }
    }

    if (parsed_ok) {
        ESP_LOGI(
            TAG_BC7215,
            "IR parsed: raw Temp=%d Mode=%d Fan=%d "
            "Power=%d -> Temp=%d Mode=%d Fan=%d Power=%s",
            parsed_temp,
            parsed_mode,
            parsed_fan,
            parsed_power,
            Temp,
            Mode,
            Fan,
            PowerOn ? "On" : "Off");

        app_matter_schedule_parsed_ac_state(
            Temp,
            Mode,
            Fan,
            PowerOn);
    } else {
        ESP_LOGW(
            TAG_BC7215,
            "Captured IR signal could not be parsed");
    }

    s_bc7215_worker_state =
        bc7215_worker_state_t::IDLE;
}

static void app_driver_bc7215_worker_handle_parsing(
    bool &parsing_was_enabled)
{
    const bool should_parse =
        s_matter_subscription_active.load() &&
        s_bc7215_paired.load() &&
        !s_bc7215_pairing.load() &&
        !s_bc7215_alt_traversal_active.load() &&
        !s_factory_reset_in_progress.load() &&
        s_ac != nullptr &&
        s_ac->init_ok;

    if (!should_parse) {
        s_ac->stop_capture();

            s_bc7215_worker_state =
                bc7215_worker_state_t::STOPPED;

        if (parsing_was_enabled) {
            parsing_was_enabled = false;

            ESP_LOGI(
                TAG_BC7215,
                "BC7215 parsing disabled");
        }

        return;
    }

    if (!parsing_was_enabled) {
        parsing_was_enabled = true;

        ESP_LOGI(
            TAG_BC7215,
            "BC7215 parsing enabled: paired and subscribed");
    }

    if (s_ac->is_busy()) {
        return;
    }

    if (s_bc7215_worker_state ==
            bc7215_worker_state_t::STOPPED ||
        s_bc7215_worker_state ==
            bc7215_worker_state_t::IDLE) {

        s_ac->start_capture(0);

        s_bc7215_worker_state =
            bc7215_worker_state_t::PARSING;

        ESP_LOGI(
            TAG_BC7215,
            "BC7215 parsing RX armed");

        return;
    }

    if (s_bc7215_worker_state ==
            bc7215_worker_state_t::PARSING &&
        s_ac->signal_captured()) {

        app_driver_bc7215_worker_parse_signal();
    }
}

static void app_driver_bc7215_worker_task(
    void *arg)
{
    bool parsing_was_enabled = false;

    ESP_LOGI(
        TAG_BC7215,
        "BC7215 worker task started");

    for (;;) {
        bc7215_command_t command{};

        if (xQueueReceive(
                s_bc7215_command_queue,
                &command,
                pdMS_TO_TICKS(20)) == pdTRUE) {

            app_driver_bc7215_worker_process_command(
                command);

            /*
             * Drain commands already queued by rapid slider/controller
             * updates before returning to the receive state machine.
             */
            while (xQueueReceive(
                       s_bc7215_command_queue,
                       &command,
                       0) == pdTRUE) {

                app_driver_bc7215_worker_process_command(
                    command);
            }

            /*
             * Match ESPHome's cooperative loop behavior: after a control
             * operation, return to the top of the worker loop. This inserts
             * one queue-wait interval before parsing can re-arm RX, giving
             * BC7215 time to assert BUSY for the transmit operation.
             */
            continue;
        }

        if (s_bc7215_worker_state ==
            bc7215_worker_state_t::PAIRING) {

            app_driver_bc7215_worker_handle_pairing();
            continue;
        }

        app_driver_bc7215_worker_handle_parsing(
            parsing_was_enabled);
    }
}

static esp_err_t app_driver_bc7215_start_worker()
{
    if (s_bc7215_worker_task_handle != nullptr) {
        return ESP_OK;
    }

    if (s_bc7215_command_queue == nullptr) {
        s_bc7215_command_queue =
            xQueueCreate(
                12,
                sizeof(bc7215_command_t));

        if (s_bc7215_command_queue == nullptr) {
            ESP_LOGE(
                TAG_BC7215,
                "Failed to create BC7215 command queue");
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t result =
        xTaskCreate(
            app_driver_bc7215_worker_task,
            "bc7215_worker",
            6144,
            nullptr,
            5,
            &s_bc7215_worker_task_handle);

    if (result != pdPASS) {
        s_bc7215_worker_task_handle = nullptr;

        vQueueDelete(
            s_bc7215_command_queue);

        s_bc7215_command_queue = nullptr;

        ESP_LOGE(
            TAG_BC7215,
            "Failed to create BC7215 worker task");

        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void app_driver_bc7215_dump_config()
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    ESP_LOGI(TAG_BC7215, "BC7215 AC Configuration:");
    ESP_LOGI(TAG_BC7215, "  Project name: %s",
             app_desc != nullptr ? app_desc->project_name : "unknown");
    ESP_LOGI(TAG_BC7215, "  Project version: %s",
             app_desc != nullptr ? app_desc->version : "unknown");
    ESP_LOGI(TAG_BC7215, "  UART: %d", static_cast<int>(BC7215_UART_NUM));
    ESP_LOGI(TAG_BC7215, "  BC7215 TX pin: GPIO%d",
             static_cast<int>(BC7215_TX_PIN));
    ESP_LOGI(TAG_BC7215, "  BC7215 RX pin: GPIO%d",
             static_cast<int>(BC7215_RX_PIN));
    ESP_LOGI(TAG_BC7215, "  BUSY pin: GPIO%d",
             static_cast<int>(BC7215_BUSY_PIN));
    ESP_LOGI(TAG_BC7215, "  MOD pin: GPIO%d",
             static_cast<int>(BC7215_MOD_PIN));

    if (s_ac) {
        ESP_LOGI(TAG_BC7215, "  AC Library Version: %s",
                 s_ac->lib_version());
        ESP_LOGI(TAG_BC7215, "  Library temperature unit: %s",
                 s_bc7215_library_unit_celsius ? "Celsius" : "Fahrenheit");
        ESP_LOGI(TAG_BC7215, "  Paired: %s",
                 s_bc7215_paired.load() ? "yes" : "no");
    } else {
        ESP_LOGI(TAG_BC7215, "  AC Library Version: unavailable");
        ESP_LOGI(TAG_BC7215, "  Library temperature unit: Celsius");
        ESP_LOGI(TAG_BC7215, "  Paired: no");
    }
}

static bool app_driver_bc7215_save_pairing()
{
    if (!s_ac || !s_ac->init_ok) {
        ESP_LOGW(TAG_BC7215,
                 "Cannot save pairing data: BC7215AC is not initialized");
        return false;
    }

    const bc7215DataVarPkt_t *src_data = s_ac->data_packet();
    const bc7215FormatPkt_t *src_format = s_ac->format_packet();

    if (src_data == nullptr || src_format == nullptr) {
        ESP_LOGE(TAG_BC7215,
                 "Cannot save pairing data: data or format packet is null");
        return false;
    }

    bc7215_stored_pairing_data_t saved{};

    const size_t data_bytes =
        static_cast<size_t>((src_data->bitLen + 7U) / 8U);

    if (data_bytes == 0 || data_bytes > sizeof(saved.data.data)) {
        ESP_LOGE(TAG_BC7215,
                 "Cannot save pairing data: invalid data size %u",
                 static_cast<unsigned>(data_bytes));
        return false;
    }

    saved.magic = BC7215_PAIRING_MAGIC;
    saved.version = BC7215_PAIRING_VERSION;
    saved.struct_size = sizeof(saved);
    saved.status = s_ac->status_byte();
    saved.library_unit_celsius = s_bc7215_library_unit_celsius ? 1 : 0;
    saved.match_index = s_bc7215_match_index;
    saved.data.bitLen = src_data->bitLen;
    memcpy(saved.data.data, src_data->data, data_bytes);
    saved.format = *src_format;

    nvs_handle_t nvs_handle = 0;
    esp_err_t err = nvs_open(
        BC7215_NVS_NAMESPACE,
        NVS_READWRITE,
        &nvs_handle
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG_BC7215,
                 "Failed to open BC7215 NVS namespace: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(
        nvs_handle,
        BC7215_NVS_PAIRING_KEY,
        &saved,
        sizeof(saved)
    );

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG_BC7215,
                 "Failed to save BC7215 pairing data: %s",
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG_BC7215,
             "BC7215 pairing data saved: status=0x%02x, bits=%u, "
             "match_index=%u",
             saved.status,
             static_cast<unsigned>(saved.data.bitLen),
             static_cast<unsigned>(saved.match_index));
    return true;
}

/*
 * Erase only the application-owned BC7215 pairing record.
 *
 * Matter's factory reset is performed separately after this function
 * returns. ESP_ERR_NVS_NOT_FOUND is treated as success because the desired
 * end state is already satisfied.
 */
static bool app_driver_bc7215_erase_saved_pairing()
{
    nvs_handle_t nvs_handle = 0;

    esp_err_t err =
        nvs_open(
            BC7215_NVS_NAMESPACE,
            NVS_READWRITE,
            &nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(
            TAG_BC7215,
            "BC7215 pairing namespace is already absent");
        return true;
    }

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG_BC7215,
            "Failed to open BC7215 NVS namespace for erase: %s",
            esp_err_to_name(err));
        return false;
    }

    err =
        nvs_erase_key(
            nvs_handle,
            BC7215_NVS_PAIRING_KEY);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG_BC7215,
            "Failed to erase BC7215 pairing data: %s",
            esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(
        TAG_BC7215,
        "BC7215 pairing data erased");

    return true;
}

/*
 * Clear the runtime pairing state and erase the application-owned BC7215
 * pairing record. Matter factory reset is performed separately.
 */
static bool app_driver_bc7215_clear_pairing()
{
    if (s_bc7215_command_queue == nullptr ||
        s_bc7215_worker_task_handle == nullptr) {

        ESP_LOGW(
            TAG_BC7215,
            "BC7215 worker is unavailable; erasing saved pairing only");

        s_bc7215_paired.store(false);
        return app_driver_bc7215_erase_saved_pairing();
    }

    /*
     * Remove any stale notification before waiting for this clear request.
     */
    ulTaskNotifyTake(
        pdTRUE,
        0);

    s_bc7215_last_clear_success.store(false);

    bc7215_command_t command{};
    command.type =
        bc7215_command_type_t::CLEAR_PAIRING;
    command.completion_task =
        xTaskGetCurrentTaskHandle();

    esp_err_t queue_err =
        app_driver_bc7215_enqueue_command(
            command,
            pdMS_TO_TICKS(200));

    if (queue_err != ESP_OK) {
        return false;
    }

    if (ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(5000)) == 0) {

        ESP_LOGE(
            TAG_BC7215,
            "Timed out waiting for BC7215 pairing clear");
        return false;
    }

    return s_bc7215_last_clear_success.load();
}

static bool app_driver_bc7215_load_pairing()
{
    if (!s_ac) {
        return false;
    }

    /*
     * Loading is authoritative: the device is unpaired unless every stored
     * field validates and init(saved...) succeeds.
     */
    s_ac->init_ok = false;
    s_bc7215_paired.store(false);

    nvs_handle_t nvs_handle = 0;
    esp_err_t err = nvs_open(
        BC7215_NVS_NAMESPACE,
        NVS_READONLY,
        &nvs_handle
    );

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG_BC7215, "No saved BC7215 pairing data");
        return false;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG_BC7215,
                 "Failed to open BC7215 NVS namespace: %s",
                 esp_err_to_name(err));
        return false;
    }

    size_t stored_size = 0;
    err = nvs_get_blob(
        nvs_handle,
        BC7215_NVS_PAIRING_KEY,
        nullptr,
        &stored_size
    );

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        ESP_LOGI(TAG_BC7215, "No saved BC7215 pairing data");
        return false;
    }

    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGW(TAG_BC7215,
                 "Failed to query BC7215 pairing data: %s",
                 esp_err_to_name(err));
        return false;
    }

    if (stored_size != sizeof(bc7215_stored_pairing_data_t)) {
        nvs_close(nvs_handle);
        ESP_LOGW(TAG_BC7215,
                 "Saved BC7215 pairing data size mismatch: stored=%u expected=%u",
                 static_cast<unsigned>(stored_size),
                 static_cast<unsigned>(sizeof(bc7215_stored_pairing_data_t)));
        return false;
    }

    bc7215_stored_pairing_data_t saved{};
    err = nvs_get_blob(
        nvs_handle,
        BC7215_NVS_PAIRING_KEY,
        &saved,
        &stored_size
    );
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG_BC7215,
                 "Failed to read BC7215 pairing data: %s",
                 esp_err_to_name(err));
        return false;
    }

    if (saved.magic != BC7215_PAIRING_MAGIC ||
        saved.version != BC7215_PAIRING_VERSION ||
        saved.struct_size != sizeof(saved)) {
        ESP_LOGW(TAG_BC7215,
                 "Saved BC7215 pairing data has an unsupported format");
        return false;
    }

    const size_t data_bytes =
        static_cast<size_t>((saved.data.bitLen + 7U) / 8U);

    if (data_bytes == 0 || data_bytes > sizeof(saved.data.data)) {
        ESP_LOGW(TAG_BC7215,
                 "Saved BC7215 pairing data has invalid bit length: %u",
                 static_cast<unsigned>(saved.data.bitLen));
        return false;
    }

    s_bc7215_library_unit_celsius =
        saved.library_unit_celsius != 0;

    if (s_bc7215_library_unit_celsius) {
        s_ac->set_celsius();
    } else {
        s_ac->set_fahrenheit();
    }

    if (!s_ac->init(saved.status, saved.data, saved.format)) {
        ESP_LOGW(TAG_BC7215,
                 "Saved BC7215 pairing data exists, but protocol init failed");
        s_ac->init_ok = false;
        s_bc7215_library_unit_celsius = true;
        s_ac->set_celsius();
        return false;
    }

    for (uint8_t index = 0;
         index < saved.match_index;
         ++index) {

        if (!s_ac->match_next()) {
            ESP_LOGW(
                TAG_BC7215,
                "Saved match index %u cannot be restored; "
                "using the first learned match",
                static_cast<unsigned>(
                    saved.match_index));

            s_bc7215_match_index = 0;

            if (!s_ac->init(
                    saved.status,
                    saved.data,
                    saved.format)) {

                s_ac->init_ok = false;
                return false;
            }

            break;
        }

        ++s_bc7215_match_index;
    }

    s_bc7215_paired.store(true);

    ESP_LOGI(TAG_BC7215,
             "BC7215 pairing restored: status=0x%02x, bits=%u, "
             "unit=%s, match_index=%u",
             saved.status,
             static_cast<unsigned>(saved.data.bitLen),
             saved.library_unit_celsius != 0 ? "C" : "F",
             static_cast<unsigned>(s_bc7215_match_index));
    return true;
}

static void app_driver_bc7215_request_pairing_toggle()
{
    if (s_factory_reset_in_progress.load()) {
        ESP_LOGW(
            TAG_BC7215,
            "Cannot toggle pairing during factory reset");
        return;
    }

    esp_err_t err =
        app_driver_bc7215_queue_pairing_toggle();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG_BC7215,
            "Failed to queue BC7215 pairing toggle: %s",
            esp_err_to_name(err));
    }
}

static void app_driver_bc7215_request_alt_next()
{
    if (s_factory_reset_in_progress.load()) {
        ESP_LOGW(
            TAG_BC7215,
            "Cannot advance Alt traversal during factory reset");
        return;
    }

    esp_err_t err =
        app_driver_bc7215_queue_alt_next();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG_BC7215,
            "Failed to queue Alt traversal command: %s",
            esp_err_to_name(err));
    }
}

static void app_driver_button_single_click_cb(
    void *arg,
    void *data)
{
    ESP_LOGI(
        TAG,
        "Button single clicked: toggle BC7215AC pairing");

    app_driver_bc7215_request_pairing_toggle();
}

static void app_driver_button_double_click_cb(void *arg, void *data)
{
    ESP_LOGI(
        TAG,
        "Button double clicked: advance BC7215 Alt protocol traversal");

    app_driver_bc7215_request_alt_next();
}


/*
 * The button component's existing long-press threshold is preserved.
 * In the current project this event occurs after holding for about 5 seconds.
 *
 * Threshold reached:
 *   LED starts blinking at 3 Hz and continues while the button is held.
 *
 * Released:
 *   BC7215 pairing data is erased immediately,
 *   then Matter factory reset is started.
 */
static void app_driver_factory_reset_task(void *arg)
{
    ESP_LOGI(
        TAG,
        "Factory reset confirmed; erasing pairing information");

    const bool pairing_erased =
        app_driver_bc7215_clear_pairing();

    if (!pairing_erased) {
        /*
         * Continue with Matter reset, but leave a clear diagnostic.
         * A future boot will not silently claim that the BC7215 record was
         * successfully removed.
         */
        ESP_LOGE(
            TAG,
            "BC7215 pairing erase failed; continuing Matter factory reset");
    }

    ESP_LOGI(
        TAG,
        "Starting Matter factory reset");

    /*
     * This clears Matter fabrics/network provisioning and restarts the
     * device through the ESP-Matter factory-reset path.
     */
    esp_matter::factory_reset();

    /*
     * Normally factory_reset() causes restart. Keep cleanup here for an
     * unexpected implementation that returns without rebooting immediately.
     * The BC7215 worker remains alive in STOPPED state after CLEAR_PAIRING.
     */
    s_factory_reset_in_progress.store(false);
    s_factory_reset_task_handle = nullptr;

    app_driver_update_led_states();

    vTaskDelete(nullptr);
}

static void app_driver_button_factory_reset_hold_cb(
    void *arg,
    void *data)
{
    bool expected = false;

    if (!s_factory_reset_armed.compare_exchange_strong(
            expected,
            true)) {
        return;
    }

    ESP_LOGI(
        TAG,
        "Factory reset armed; LED blinking at 3 Hz. "
        "Release button to erase pairing information.");

    /*
     * Reaching the 5-second threshold starts the reset warning indication.
     * The LED continues blinking until the button is released.
     */
    app_driver_led_set_mode(
        LED_MODE_BLINK_3HZ);
}

static void app_driver_button_factory_reset_release_cb(
    void *arg,
    void *data)
{
    if (!s_factory_reset_armed.exchange(false)) {
        return;
    }

    if (s_factory_reset_task_handle != nullptr ||
        s_factory_reset_in_progress.exchange(true)) {

        ESP_LOGW(
            TAG,
            "Factory reset is already running");
        return;
    }

    ESP_LOGI(
        TAG,
        "Factory reset release confirmed; starting erase immediately");

    BaseType_t task_result =
        xTaskCreate(
            app_driver_factory_reset_task,
            "factory_reset",
            4096,
            nullptr,
            6,
            &s_factory_reset_task_handle);

    if (task_result != pdPASS) {
        ESP_LOGE(
            TAG,
            "Failed to create factory-reset task");

        s_factory_reset_task_handle = nullptr;
        s_factory_reset_in_progress.store(false);
        app_driver_update_led_states();
    }
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
	int8_t Key = -1;
// Key Value Range: 0-3,
//0 - Temperature +
//1 - Temperature -
//2 - Mode
//3 - Fan Speed
//4 - Power

	(void) driver_handle;
	if (s_matter_syncing_from_local) {
	    return ESP_OK;
	}
    ESP_LOGI(TAG, "attribute_update: endpoint=0x%04x cluster=0x%08lx attribute=0x%08lx",
             endpoint_id,
             (unsigned long) cluster_id,
             (unsigned long) attribute_id);

    esp_err_t err = ESP_OK;

    if (endpoint_id == room_air_conditioner_endpoint_id) {
        if (cluster_id == OnOff::Id) {
            if (attribute_id == OnOff::Attributes::OnOff::Id) {
				err = app_driver_room_air_conditioner_set_power(val);
            }
		}
 // Branch 2: handle the Thermostat cluster
        else if (cluster_id == Thermostat::Id) {
            
            // 2.1 Handle the cooling or heating temperature setpoint
            if (attribute_id == Thermostat::Attributes::OccupiedCoolingSetpoint::Id ||
                attribute_id == Thermostat::Attributes::OccupiedHeatingSetpoint::Id) {
                
				Key = 0;
                // Core behavior: align the value to 1°C steps
                // Units are 0.01°C, so 1°C equals 100 units. Round to the nearest multiple of 100.
                int16_t raw_temp = val->val.i16;
                int16_t rounded_temp = ((raw_temp + 50) / 100) * 100;
				Temp = rounded_temp / 100;
                val->val.i16 = rounded_temp; // Write back the aligned value so the controller display is corrected too

                ESP_LOGI(TAG, "Target temperature update received (aligned to 1°C steps): %d (value sent to AC: %.1f°C)", 
                         raw_temp, (float)rounded_temp / 100.0);
                
			    /*
			     * After the current write completes, synchronize:
			     * CoolingSetpoint
			     * HeatingSetpoint
			     * LocalTemperature
			     */
			    app_matter_schedule_report_all_temperatures(rounded_temp);

            }
            
            // 2.2 Handle air-conditioner mode changes (SystemMode)
            else if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
				Key = 2;
				uint8_t matter_mode = val->val.u8;
                /*
                 * Officially exposed Thermostat features are Cooling + Heating.
                 * Off / Cool / Heat are the primary supported SystemMode values.
                 *
                 * Auto / FanOnly / Dry are still accepted when a controller
                 * sends them (for example Home Assistant) and are mapped to
                 * BC7215 modes. They are not declared as Thermostat features,
                 * so Apple Home / Google Home typically will not offer them.
                 *
                 * Matter SystemMode -> BC7215 Mode:
                 *   0 Off      -> Power=false, retain the previous Mode
                 *   1 Auto     -> Mode=0
                 *   3 Cool     -> Mode=1
                 *   4 Heat     -> Mode=2
                 *   7 FanOnly  -> Mode=4
                 *   8 Dry      -> Mode=3
                 */
                switch (matter_mode) {
                    case 0:
                        PowerOn = false;
						Key = 4;
                        break;
                    case 1:
                        Mode = 0;
                        PowerOn = true;
                        break;
                    case 3:
                        Mode = 1;
                        PowerOn = true;
                        break;
                    case 4:
                        Mode = 2;
                        PowerOn = true;
                        break;
                    case 7:
                        Mode = 4;
                        PowerOn = true;
                        break;
                    case 8:
                        Mode = 3;
                        PowerOn = true;
                        break;
                    default:
                        Key = -1;
                        ESP_LOGW(TAG, "Unsupported Matter SystemMode: %u",
                                 static_cast<unsigned>(matter_mode));
                        break;
                }

                if (Key >= 0) {
                    /*
                     * Publish the complete logical appliance state. Off keeps
                     * Mode unchanged; a non-Off mode restores both the Room AC
                     * and Fan endpoint to On consistently.
                     */
                    app_matter_schedule_whole_device_state(
                        PowerOn,
                        Mode,
                        Fan);
                }

                ESP_LOGI(TAG, "Air-conditioner operating mode update received: %d", matter_mode);
            }
		}
	}
	// Branch 3: handle fan speed (Fan Control cluster)
    else if (endpoint_id == fan_endpoint_id &&
             cluster_id == FanControl::Id) {

        if (attribute_id ==
            FanControl::Attributes::FanMode::Id) {

            const uint8_t requested_mode =
                val->val.u8;

            switch (requested_mode) {
                case static_cast<uint8_t>(
                    FanControl::FanModeEnum::kOff):

                    PowerOn = false;
                    Key = 4;

                    ESP_LOGI(
                        TAG,
                        "FanMode Off received: turning whole device Off");
                    break;

                case static_cast<uint8_t>(
                    FanControl::FanModeEnum::kLow):

                case static_cast<uint8_t>(
                    FanControl::FanModeEnum::kOn):

                    PowerOn = true;
                    Fan = 1;
                    Key = 3;

                    ESP_LOGI(
                        TAG,
                        "FanMode normalized: requested=%u -> Low/25%%",
                        static_cast<unsigned>(requested_mode));
                    break;

                case static_cast<uint8_t>(
                    FanControl::FanModeEnum::kMedium):

                    PowerOn = true;
                    Fan = 2;
                    Key = 3;

                    ESP_LOGI(
                        TAG,
                        "FanMode Medium -> 50%%");
                    break;

                case static_cast<uint8_t>(
                    FanControl::FanModeEnum::kHigh):

                    PowerOn = true;
                    Fan = 3;
                    Key = 3;

                    ESP_LOGI(
                        TAG,
                        "FanMode High -> 100%%");
                    break;

                case static_cast<uint8_t>(
                    FanControl::FanModeEnum::kAuto):

                    PowerOn = true;
                    Fan = 0;
                    Key = 3;

                    ESP_LOGI(
                        TAG,
                        "FanMode Auto -> 100%% display");
                    break;

                default:
                    Key = -1;

                    ESP_LOGW(
                        TAG,
                        "Unsupported FanMode: %u",
                        static_cast<unsigned>(requested_mode));
                    break;
            }

            if (Key >= 0) {
                /*
                 * A non-Off Fan command powers on the appliance and restores
                 * Thermostat::SystemMode from the retained Mode value.
                 */
                app_matter_schedule_whole_device_state(
                    PowerOn,
                    Mode,
                    Fan);
            }
        }
        else if (attribute_id ==
                 FanControl::Attributes::PercentSetting::Id) {

            const uint8_t requested_percent =
                val->val.u8;

            if (requested_percent == 0) {
                PowerOn = false;
                Key = 4;

                ESP_LOGI(
                    TAG,
                    "Fan PercentSetting 0%% received: "
                    "turning whole device Off");
            } else {
                PowerOn = true;

                Fan =
                    app_driver_percent_to_bc7215_fan(
                        requested_percent);

                Key = 3;

                const uint8_t normalized_mode =
                    app_driver_bc7215_fan_to_matter(
                        Fan,
                        true);

                const uint8_t normalized_percent =
                    app_driver_bc7215_fan_to_percent(
                        Fan,
                        true);

                ESP_LOGI(
                    TAG,
                    "Fan percentage normalized: requested=%u%% -> "
                    "Fan=%d FanMode=%u Percent=%u%%",
                    static_cast<unsigned>(requested_percent),
                    Fan,
                    static_cast<unsigned>(normalized_mode),
                    static_cast<unsigned>(normalized_percent));
            }

            /*
             * 0% publishes complete Off state; a non-zero setting restores
             * Room AC OnOff and Thermostat SystemMode as well as Fan state.
             */
            app_matter_schedule_whole_device_state(
                PowerOn,
                Mode,
                Fan);
        }
    }

      
    if (Key >= 0 &&
        !s_factory_reset_in_progress.load()) {

        if (!s_bc7215_alt_traversal_active.load()) {
            app_driver_led_set_mode(
                ONE_SHOT_ON_1S);
        }

        esp_err_t queue_err = ESP_OK;

        if (Key == 4) {
            queue_err =
                app_driver_bc7215_queue_power(
                    PowerOn);
        } else {
            queue_err =
                app_driver_bc7215_queue_state(
                    Temp,
                    Mode,
                    Fan,
                    Key,
                    PowerOn);
        }

        if (queue_err != ESP_OK) {
            ESP_LOGE(
                TAG_BC7215,
                "Failed to queue Matter IR command: %s",
                esp_err_to_name(queue_err));
        }
    }
    return err;
}

static esp_err_t app_matter_update_target_temperature(int16_t temp_x100, bool cooling)
{
    uint32_t attr_id = cooling
        ? Thermostat::Attributes::OccupiedCoolingSetpoint::Id
        : Thermostat::Attributes::OccupiedHeatingSetpoint::Id;

    esp_matter_attr_val_t val = esp_matter_int16(temp_x100);

    s_matter_syncing_from_local = true;
    esp_err_t err = attribute::update(
        room_air_conditioner_endpoint_id,
        Thermostat::Id,
        attr_id,
        &val
    );
    s_matter_syncing_from_local = false;

    return err;
}

esp_err_t app_driver_room_air_conditioner_set_defaults(
    uint16_t endpoint_id)
{
    esp_err_t first_error = ESP_OK;

    /*
     * Force a deterministic whole-device startup state:
     *   Room AC OnOff        = Off
     *   Thermostat SystemMode = Off
     *   FanMode              = Off
     *   PercentSetting       = 0%
     *   PercentCurrent       = 0%
     *
     * Keep the last/default BC7215 fan level at Low for the next power-on
     * or non-zero fan-speed command.
     */
    PowerOn = false;
    Mode = 1;
    Fan = 1;

    /*
     * Also request a physical AC Off command. The worker serializes this
     * transmit operation with parsing and pairing.
     */
    esp_err_t initial_off_err =
        app_driver_bc7215_queue_power(false);

    if (initial_off_err != ESP_OK) {
        ESP_LOGW(
            TAG_BC7215,
            "Failed to queue initial AC Off command: %s",
            esp_err_to_name(initial_off_err));
    }

    s_matter_syncing_from_local = true;

    auto record_error =
        [&first_error](
            const char *name,
            esp_err_t err) {

            if (err != ESP_OK) {
                ESP_LOGE(
                    TAG,
                    "Failed to set initial %s: %s",
                    name,
                    esp_err_to_name(err));

                if (first_error == ESP_OK) {
                    first_error = err;
                }
            }
        };

    esp_matter_attr_val_t power_val =
        esp_matter_bool(false);

    record_error(
        "RoomAC OnOff",
        attribute::update(
            endpoint_id,
            OnOff::Id,
            OnOff::Attributes::OnOff::Id,
            &power_val));

    esp_matter_attr_val_t system_mode_val =
        esp_matter_enum8(
            static_cast<uint8_t>(
                Thermostat::SystemModeEnum::kOff));

    record_error(
        "Thermostat SystemMode",
        attribute::update(
            endpoint_id,
            Thermostat::Id,
            Thermostat::Attributes::SystemMode::Id,
            &system_mode_val));

    esp_matter_attr_val_t fan_mode_val =
        esp_matter_enum8(
            static_cast<uint8_t>(
                FanControl::FanModeEnum::kOff));

    record_error(
        "FanMode",
        attribute::update(
            fan_endpoint_id,
            FanControl::Id,
            FanControl::Attributes::FanMode::Id,
            &fan_mode_val));

    esp_matter_attr_val_t percent_current =
        esp_matter_uint8(0);

    record_error(
        "PercentCurrent",
        attribute::update(
            fan_endpoint_id,
            FanControl::Id,
            FanControl::Attributes::PercentCurrent::Id,
            &percent_current));

    esp_matter_attr_val_t percent_setting =
        esp_matter_nullable_uint8(0);

    record_error(
        "PercentSetting",
        attribute::update(
            fan_endpoint_id,
            FanControl::Id,
            FanControl::Attributes::PercentSetting::Id,
            &percent_setting));

    s_matter_syncing_from_local = false;

    ESP_LOGI(
        TAG,
        "Initial whole-device state forced to Off");

    return first_error;
}

static esp_err_t app_driver_bc7215_init()
{
    ESP_LOGI(TAG_BC7215, "BC7215AC init start");
    ESP_LOGI(TAG_BC7215, "UART=%d RX=GPIO%d TX=GPIO%d BUSY=GPIO%d MOD=GPIO%d",
             static_cast<int>(BC7215_UART_NUM),
             static_cast<int>(BC7215_RX_PIN),
             static_cast<int>(BC7215_TX_PIN),
             static_cast<int>(BC7215_BUSY_PIN),
             static_cast<int>(BC7215_MOD_PIN));

    s_ac = std::make_unique<bc7215::BC7215AC>(
        BC7215_UART_NUM,
        BC7215_RX_PIN,
        BC7215_TX_PIN,
        BC7215_BUSY_PIN,
        BC7215_MOD_PIN
    );

    esp_err_t err = s_ac->begin();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_BC7215, "BC7215AC begin failed: %s", esp_err_to_name(err));
        s_ac.reset();
        return err;
    }

    // Use Celsius by default; restore Fahrenheit if it is stored in NVS.
    s_bc7215_library_unit_celsius = true;
    s_ac->set_celsius();
	s_ac->stop_capture();

    ESP_LOGI(TAG_BC7215, "BC7215AC begin OK");

    /*
     * begin() initializes only the BC7215 hardware. Next, attempt to
     * initialize BC7215AC using protocol data stored in NVS. Dump the
     * configuration after restoration so the Paired field is accurate.
     */
    app_driver_bc7215_load_pairing();
    app_driver_bc7215_dump_config();

    return ESP_OK;
}

app_driver_handle_t app_driver_room_air_conditioner_init()
{
// 1. Initialize GPIO
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << SUPER_MINI_LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE
    };
    gpio_config(&io_conf);
    gpio_set_level(SUPER_MINI_LED_GPIO, 1); // Off by default

    // 2. Start the LED state-machine task. All subsequent LED operations use the command queue.
    esp_err_t led_err = app_driver_led_controller_init();
    if (led_err != ESP_OK) {
        ESP_LOGE(TAG, "LED controller init failed: %s", esp_err_to_name(led_err));
    } else {
        // Unpaired: blink at 1 Hz. After pairing, switch to one short flash every 3 seconds.
        app_driver_led_set_mode(LED_MODE_BLINK_1HZ);
    }

    /* Initialize BC7215AC hardware */
    esp_err_t err = app_driver_bc7215_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_BC7215,
                 "BC7215AC init failed, Matter app will continue");
    } else {
        esp_err_t worker_err =
            app_driver_bc7215_start_worker();

        if (worker_err != ESP_OK) {
            ESP_LOGE(
                TAG_BC7215,
                "BC7215 worker task init failed: %s",
                esp_err_to_name(worker_err));
        }

        if (s_bc7215_paired.load()) {
            app_driver_led_set_mode(
                LED_MODE_BREATH_3S);
        }
    }

// Return a dummy address (placeholder) to prevent crashes if lower layers check `if (handle == NULL)` or dereference it
    //return (app_driver_handle_t)1;
	return nullptr;
}

app_driver_handle_t app_driver_button_init()
{
    /* Initialize button */
    button_handle_t handle = NULL;
    const button_config_t btn_cfg = {0};
    const button_gpio_config_t btn_gpio_cfg = button_driver_get_config();

    if (iot_button_new_gpio_device(&btn_cfg, &btn_gpio_cfg, &handle) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create button device");
        return NULL;
    }

    esp_err_t register_err = ESP_OK;

    register_err |=
        iot_button_register_cb(
            handle,
            BUTTON_SINGLE_CLICK,
            NULL,
            app_driver_button_single_click_cb,
            NULL);

    register_err |=
        iot_button_register_cb(
            handle,
            BUTTON_DOUBLE_CLICK,
            NULL,
            app_driver_button_double_click_cb,
            NULL);

    /*
     * These are the same event types used by ESP-Matter's common app_reset:
     * long-press threshold reached, followed by release confirmation.
     */
    register_err |=
        iot_button_register_cb(
            handle,
            BUTTON_LONG_PRESS_HOLD,
            NULL,
            app_driver_button_factory_reset_hold_cb,
            NULL);

    register_err |=
        iot_button_register_cb(
            handle,
            BUTTON_PRESS_UP,
            NULL,
            app_driver_button_factory_reset_release_cb,
            NULL);

    if (register_err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to register one or more button callbacks: %s",
            esp_err_to_name(register_err));
    }

    return (app_driver_handle_t)handle;
}

void app_driver_update_led_states()
{
	app_matter_state_t matter_state;

    /*
     * Factory-reset indication has priority over pairing/network states.
     */
    if (s_factory_reset_in_progress.load()) {
        app_driver_led_set_mode(
            LED_MODE_BLINK_3HZ);
        return;
    }

    if (s_factory_reset_armed.load()) {
        app_driver_led_set_mode(
            LED_MODE_BLINK_3HZ);
        return;
    }

    if (s_bc7215_alt_traversal_active.load()) {
        app_driver_led_set_static(false);
        return;
    }

	matter_state = app_get_matter_state_locked();

	if (!s_bc7215_paired.load() &&
        matter_state == app_matter_state_t::NOT_COMMISSIONED) {
		app_driver_led_set_mode(LED_MODE_ON);
		return;
	}

	if ((matter_state == app_matter_state_t::COMMISSIONING) || 
		(matter_state == app_matter_state_t::COMMISSIONED_OFFLINE)  ||
		(matter_state == app_matter_state_t::CONNECTED_NO_SUBSCRIPTION ))  {
		
		app_driver_led_set_mode(LED_MODE_BLINK_1HZ);
		return;
	}

	if (s_bc7215_pairing.load()) {
		app_driver_led_set_mode(LED_MODE_BLINK_3HZ);
        return;
	}	

    if (!s_bc7215_paired.load() &&
        matter_state == app_matter_state_t::SUBSCRIBED) {

        app_driver_led_set_mode(
            LED_MODE_BLINK_1X_1HZ);
        return;
    }

    if (s_bc7215_paired.load()) {
        if (matter_state ==
            app_matter_state_t::NOT_COMMISSIONED) {

            app_driver_led_set_mode(
                LED_MODE_BLINK_2X_1HZ);
            return;
        }

        app_driver_led_set_mode(
            LED_MODE_BREATH_3S);
        return;
    }


	app_driver_led_set_mode(LED_MODE_OFF);
}
