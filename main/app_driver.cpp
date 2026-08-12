/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <stdlib.h>
#include <string.h>
#include <memory>
#include <new>
#include <string>
#include <cmath>
#include <climits>
#include <atomic>

#include <esp_log.h>
#include <esp_matter.h>
#include <esp_app_desc.h>
#include <esp_system.h>

#include <app_priv.h>
#include <device.h>
#include <ir_ac_controller.hpp>
#include <driver/gpio.h>
#include <esp_timer.h> // ESP software timer API
#include <nvs.h>
#include <nvs_flash.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/queue.h>
#include <platform/CHIPDeviceLayer.h>
#include "board_pins.h"
#include "display_gc9a01.h"
#include "ws2812_temp_light.h"

using namespace chip::app::Clusters;
using namespace esp_matter;

static const char *TAG = "app_driver";
extern uint16_t room_air_conditioner_endpoint_id;
extern uint16_t temp_light_endpoint_id;
static const char *TAG_IR = "ir_ac_matter";

/*
 * When an ambient temperature sensor (SHT30) is active, LocalTemperature
 * reports measured room temperature and must not be mirrored from setpoints.
 */
static std::atomic<bool> s_ambient_sensor_active{false};

void app_driver_set_ambient_sensor_active(bool active)
{
    s_ambient_sensor_active.store(active);
}

bool app_driver_ambient_sensor_active(void)
{
    return s_ambient_sensor_active.load();
}

esp_err_t app_driver_temp_light_set_power(esp_matter_attr_val_t *val)
{
    if (val == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    const bool on = val->val.b;
    ws2812_temp_light_set_enabled(on);
    return ESP_OK;
}

esp_err_t app_driver_temp_light_set_brightness(esp_matter_attr_val_t *val)
{
    if (val == nullptr) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t level = val->val.u8;
    if (level < 1) {
        level = 1;
    }
    ws2812_temp_light_set_brightness(level);
    return ESP_OK;
}

// ESP32-S3-WROOM-1-N16 defaults (see board_pins.h).
static constexpr gpio_num_t IR_TX_PIN = BOARD_IR_TX_GPIO;
static constexpr gpio_num_t IR_RX_PIN = BOARD_IR_RX_GPIO;

static constexpr gpio_num_t SUPER_MINI_LED_GPIO = BOARD_STATUS_LED_GPIO;
static constexpr bool SUPER_MINI_LED_ACTIVE_LOW = BOARD_STATUS_LED_ACTIVE_LOW;

/*
 * LED controller
 *
 * The controller has two layers:
 *   1. A persistent/normal display: static ON/OFF or a repeating pattern.
 *   2. A temporary one-shot pattern. When it finishes, the latest normal
 *      display is restored automatically.
 *
 * All GPIO writes are performed by one FreeRTOS task. This avoids races
 * between Matter callbacks, the IR worker and button callbacks.
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


static std::unique_ptr<ir_ac::IrAcController> s_ac;
static bool s_matter_syncing_from_local = false;
static TaskHandle_t s_ir_worker_task_handle = nullptr;
static QueueHandle_t s_ir_command_queue = nullptr;

static std::atomic_bool s_factory_reset_armed{false};
static std::atomic_bool s_factory_reset_in_progress{false};
static std::atomic_bool s_ir_pairing{false};
static TickType_t s_ir_pairing_started_tick = 0;
static TickType_t s_ir_pairing_last_wait_log_tick = 0;
static TickType_t s_ir_pairing_last_rearm_tick = 0;
/* Give the user time to aim the remote; UI leaves "..." after this. */
static constexpr uint32_t k_ir_learn_timeout_ms = 60000;
static constexpr uint32_t k_ir_learn_wait_log_ms = 5000;
static constexpr uint32_t k_ir_learn_rearm_ms = 500;
/* After a successful learn, wait before arming continuous RX/parse. */
static constexpr uint32_t k_ir_parse_settle_ms = 2000;
static std::atomic_bool s_ir_paired{false};
static std::atomic_bool s_matter_subscription_active{false};
static TickType_t s_ir_parse_earliest_tick = 0;
static std::atomic_bool s_ir_last_clear_success{false};
static std::atomic_bool s_ir_alt_traversal_active{false};

enum class ir_alt_phase_t : uint8_t {
    INACTIVE,
    MATCH_NEXT,
    PREDEFINED,
};

static ir_alt_phase_t s_ir_alt_phase =
    ir_alt_phase_t::INACTIVE;
static uint8_t s_ir_match_index = 0;
static uint8_t s_ir_alt_predefined_number = 0;

enum class ir_worker_state_t : uint8_t {
    STOPPED,
    IDLE,
    PARSING,
    PAIRING,
};

enum class ir_command_type_t : uint8_t {
    SEND_POWER,
    SEND_STATE,
    TOGGLE_PAIRING,
    ALT_NEXT,
    CLEAR_PAIRING,
};

struct ir_command_t {
    ir_command_type_t type =
        ir_command_type_t::SEND_STATE;

    int temp = 25;
    int mode = 1;
    int fan = 0; /* IR Auto */
    int key = 0;
    bool power_on = false;

    /*
     * Used only by CLEAR_PAIRING. The worker notifies this task after the
     * IR runtime state and saved pairing record have both been cleared.
     */
    TaskHandle_t completion_task = nullptr;
};

static ir_worker_state_t s_ir_worker_state =
    ir_worker_state_t::STOPPED;

static int Temp = 25;
static int Mode = 1;
/* IR fan speed is fixed to Auto; Matter does not expose a fan control. */
static int Fan = 0;
static bool PowerOn = false;

static void app_matter_schedule_whole_device_state(
    bool power_on,
    int mode);

/*
 * Storage for IR air-conditioner protocol pairing data.
 *
 * NVS already performs integrity checks on storage pages. The additional
 * magic, version and size fields prevent old data from being used after
 * firmware updates change the structure layout.
 */
static constexpr char IR_NVS_NAMESPACE[] = "ir_ac";
static constexpr char IR_NVS_PAIRING_KEY[] = "pairing";
static constexpr uint32_t IR_PAIRING_MAGIC = 0x49524143; // "IRAC"
static constexpr uint16_t IR_PAIRING_VERSION = 1;

struct ir_stored_pairing_data_t {
    uint32_t magic;
    uint16_t version;
    uint16_t struct_size;
    int32_t protocol;
    int16_t model;
    uint8_t alt_index;
    uint8_t reserved;
};


/*
 * Queue a command for the single IR worker.
 *
 * After the worker has started, no other task may directly call IrAcController
 * capture, parsing, pairing or transmit methods.
 */
static esp_err_t app_driver_ir_enqueue_command(
    const ir_command_t &command,
    TickType_t timeout = pdMS_TO_TICKS(50))
{
    if (s_ir_command_queue == nullptr) {
        ESP_LOGE(
            TAG_IR,
            "IR worker command queue is unavailable");
        return ESP_ERR_INVALID_STATE;
    }

    if (xQueueSend(
            s_ir_command_queue,
            &command,
            timeout) != pdTRUE) {

        ESP_LOGE(
            TAG_IR,
            "IR worker command queue is full");
        return ESP_ERR_TIMEOUT;
    }

    return ESP_OK;
}

static esp_err_t app_driver_ir_queue_power(
    bool power_on)
{
    ir_command_t command{};
    command.type =
        ir_command_type_t::SEND_POWER;
    command.power_on = power_on;

    return app_driver_ir_enqueue_command(
        command);
}

static esp_err_t app_driver_ir_queue_state(
    int temp,
    int mode,
    int fan,
    int key,
    bool power_on)
{
    ir_command_t command{};
    command.type =
        ir_command_type_t::SEND_STATE;
    command.temp = temp;
    command.mode = mode;
    command.fan = fan;
    command.key = key;
    command.power_on = power_on;

    return app_driver_ir_enqueue_command(
        command);
}

static esp_err_t app_driver_ir_queue_pairing_toggle()
{
    ir_command_t command{};
    command.type =
        ir_command_type_t::TOGGLE_PAIRING;

    return app_driver_ir_enqueue_command(
        command);
}

static esp_err_t app_driver_ir_queue_alt_next()
{
    ir_command_t command{};
    command.type =
        ir_command_type_t::ALT_NEXT;

    return app_driver_ir_enqueue_command(
        command);
}


/*
 * Called by the Matter subscription callback.
 * The IR worker starts/stops parsing RX according to this flag.
 */
void app_driver_set_subscription_active(bool active)
{
    const bool previous =
        s_matter_subscription_active.exchange(active);

    if (previous != active) {
        ESP_LOGI(TAG_IR,
                 "Matter subscription state: %s",
                 active ? "active" : "inactive");
    }
}

/*
 * LED indication from the IR worker only — never touch FabricTable /
 * InteractionModelEngine here. Network-aware LED updates stay on the CHIP
 * stack via app_driver_update_led_states().
 */
static void app_driver_ir_apply_local_led(void)
{
    if (s_factory_reset_in_progress.load() || s_factory_reset_armed.load()) {
        app_driver_led_set_mode(LED_MODE_BLINK_3HZ);
        return;
    }
    if (s_ir_alt_traversal_active.load()) {
        app_driver_led_set_static(false);
        return;
    }
    if (s_ir_pairing.load()) {
        app_driver_led_set_mode(LED_MODE_BLINK_3HZ);
        return;
    }
    if (s_ir_paired.load()) {
        app_driver_led_set_mode(LED_MODE_BREATH_3S);
        return;
    }
    if (s_matter_subscription_active.load()) {
        app_driver_led_set_mode(LED_MODE_BLINK_1X_1HZ);
        return;
    }
    app_driver_led_set_mode(LED_MODE_BLINK_1HZ);
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
    if (!s_ir_alt_traversal_active.load()) {
        app_driver_led_set_mode(
            ONE_SHOT_ON_1S);
    }

    PowerOn = val->val.b;

    esp_err_t queue_err = ESP_OK;

    if (!s_factory_reset_in_progress.load()) {
        queue_err =
            app_driver_ir_queue_power(
                PowerOn);

        if (queue_err != ESP_OK) {
            ESP_LOGE(
                TAG_IR,
                "Failed to queue Room AC power command: %s",
                esp_err_to_name(queue_err));
        }
    }

    /*
     * Room AC OnOff and Thermostat SystemMode represent the appliance.
     * Publish one complete state so every subscribed controller sees
     * consistent values. IR fan stays Auto (no Matter fan endpoint).
     *
     * When PowerOn becomes true, Mode still contains the last active
     * non-Off mode and is therefore used to restore SystemMode.
     */
    app_matter_schedule_whole_device_state(
        PowerOn,
        Mode);

    ESP_LOGI(
        TAG,
        "Set power: %d",
        static_cast<int>(PowerOn));

    app_driver_log_current_ac_state(
        "RoomAC OnOff");

    /*
     * The LED is dedicated to displaying the IR pairing state:
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
     */
    local_temp.type =
        ESP_MATTER_VAL_TYPE_NULLABLE_INT16;

    local_temp.val.i16 = temp_x100;

    const bool previous_sync_state = s_matter_syncing_from_local;
    s_matter_syncing_from_local = true;

    esp_err_t err = attribute::report(
        room_air_conditioner_endpoint_id,
        Thermostat::Id,
        Thermostat::Attributes::LocalTemperature::Id,
        &local_temp
    );

    s_matter_syncing_from_local = previous_sync_state;

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG,
            "Failed to update displayed temperature: %s",
            esp_err_to_name(err)
        );
    }

    return err;
}

/*
 * Last setpoint (0.01°C) successfully pushed to Matter. Used to skip
 * no-op resyncs when the controller rewrites the same temperature.
 */
static int16_t s_last_synced_temp_x100 = INT16_MIN;
/* Coalesce ScheduleLambda posts — CHIP aborts if its event queue fills. */
static std::atomic<int16_t> s_pending_temp_x100{0};
static std::atomic<bool> s_temp_report_scheduled{false};

static esp_err_t app_matter_report_all_temperatures_now(
    int16_t temp_x100)
{
    esp_err_t first_error = ESP_OK;

    /*
     * attribute::report() invokes the attribute callback. Without this
     * guard, OccupiedCooling/Heating reports re-enter
     * app_driver_attribute_update() and schedule more lambdas until the
     * CHIP platform event queue overflows and the device aborts.
     */
    const bool previous_sync_state = s_matter_syncing_from_local;
    s_matter_syncing_from_local = true;

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

    /*
     * Only mirror the setpoint into LocalTemperature when no ambient
     * sensor is available. With SHT30 active, LocalTemperature is the
     * measured room temperature.
     */
    if (!app_driver_ambient_sensor_active()) {
        esp_err_t local_err =
            app_matter_update_display_temperature(
                temp_x100);

        if (local_err != ESP_OK &&
            first_error == ESP_OK) {
            first_error = local_err;
        }
    }

    s_matter_syncing_from_local = previous_sync_state;

    if (first_error == ESP_OK) {
        s_last_synced_temp_x100 = temp_x100;
    }

    return first_error;
}

static void app_matter_schedule_report_all_temperatures(
    int16_t temp_x100)
{
    s_pending_temp_x100.store(temp_x100, std::memory_order_relaxed);

    bool expected = false;
    if (!s_temp_report_scheduled.compare_exchange_strong(
            expected, true, std::memory_order_acq_rel)) {
        /* A sync is already queued; it will pick up s_pending_temp_x100. */
        return;
    }

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t) {
            const int16_t temp_x100 =
                s_pending_temp_x100.load(std::memory_order_relaxed);
            s_temp_report_scheduled.store(false, std::memory_order_release);

            esp_err_t report_err =
                app_matter_report_all_temperatures_now(temp_x100);
            if (report_err != ESP_OK) {
                ESP_LOGE(TAG,
                         "Scheduled temperature synchronization failed: %s",
                         esp_err_to_name(report_err));
            }

            /* One follow-up if the setpoint changed while we were reporting. */
            const int16_t latest =
                s_pending_temp_x100.load(std::memory_order_relaxed);
            if (latest != temp_x100) {
                app_matter_schedule_report_all_temperatures(latest);
            }
        },
        0);

    if (err != CHIP_NO_ERROR) {
        s_temp_report_scheduled.store(false, std::memory_order_release);
        ESP_LOGW(TAG,
                 "ScheduleWork for temperature sync failed: %" CHIP_ERROR_FORMAT,
                 err.Format());
    }
}


static uint8_t app_driver_ir_mode_to_matter(int mode)
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
 * Update Matter attributes that represent the appliance's logical
 * power/mode state. This must run on the CHIP thread.
 *
 * PowerOn is kept separate from Mode. Therefore an Off command does not erase
 * the last active IR mode, and a later On command can restore
 * Thermostat::SystemMode correctly.
 *
 * Fan speed is not exposed over Matter; IR always uses Auto.
 */
static void app_matter_update_whole_device_state_now(
    bool power_on,
    int mode)
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
            ? app_driver_ir_mode_to_matter(mode)
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

    s_matter_syncing_from_local =
        previous_sync_state;

    ESP_LOGI(
        TAG,
        "Whole device synchronized: Power=%s Mode=%d "
        "SystemMode=%u (IR Fan=Auto)",
        power_on ? "On" : "Off",
        mode,
        static_cast<unsigned>(matter_system_mode));
}

static void app_matter_schedule_whole_device_state(
    bool power_on,
    int mode)
{
    struct WholeArgs {
        bool power_on;
        int mode;
    };
    auto *args = new (std::nothrow) WholeArgs{power_on, mode};
    if (args == nullptr) {
        ESP_LOGE(TAG, "No heap to schedule whole-device state");
        return;
    }

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t arg) {
            auto *state = reinterpret_cast<WholeArgs *>(arg);
            app_matter_update_whole_device_state_now(
                state->power_on, state->mode);
            delete state;
        },
        reinterpret_cast<intptr_t>(args));

    if (err != CHIP_NO_ERROR) {
        delete args;
        ESP_LOGW(TAG,
                 "ScheduleWork for whole-device state failed: %" CHIP_ERROR_FORMAT,
                 err.Format());
    }
}

/*
 * Runs on the CHIP thread. attribute::update() changes the Matter data model;
 * active subscriptions then report the new values to the controller.
 */
static void app_matter_apply_parsed_ac_state_now(
    int temp_c,
    int mode,
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
            ? app_driver_ir_mode_to_matter(mode)
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

        /*
         * Without an ambient sensor, LocalTemperature was historically
         * mirrored from the AC setpoint so controllers still show a value.
         * With SHT30 active, leave LocalTemperature to the sensor path.
         */
        if (!app_driver_ambient_sensor_active()) {
            esp_matter_attr_val_t local_temp =
                esp_matter_invalid(nullptr);
            local_temp.type =
                ESP_MATTER_VAL_TYPE_NULLABLE_INT16;
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
    }

    s_matter_syncing_from_local = false;

    ESP_LOGI(TAG,
             "Parsed AC state synchronized: Temp=%dC Mode=%d "
             "Power=%s (IR Fan=Auto)",
             temp_c,
             mode,
             power_on ? "On" : "Off");
}

static void app_matter_schedule_parsed_ac_state(
    int temp_c,
    int mode,
    bool power_on)
{
    /*
     * IR worker is not the CHIP thread. Prefer PlatformMgr::ScheduleWork so
     * we never touch SystemLayer off-stack (boot/learn crashes).
     */
    struct ParsedArgs {
        int temp_c;
        int mode;
        bool power_on;
    };
    auto *args = new (std::nothrow) ParsedArgs{temp_c, mode, power_on};
    if (args == nullptr) {
        ESP_LOGE(TAG, "No heap to schedule parsed AC state");
        return;
    }

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t arg) {
            auto *parsed = reinterpret_cast<ParsedArgs *>(arg);
            app_matter_apply_parsed_ac_state_now(
                parsed->temp_c, parsed->mode, parsed->power_on);
            delete parsed;
        },
        reinterpret_cast<intptr_t>(args));

    if (err != CHIP_NO_ERROR) {
        delete args;
        ESP_LOGW(TAG,
                 "ScheduleWork for parsed AC state failed: %" CHIP_ERROR_FORMAT,
                 err.Format());
    }
}

/*
 * Validate a decoded Celsius temperature. Invalid values leave the previous
 * temperature intact.
 */
static int app_driver_ir_temperature_to_celsius(int parsed_temp)
{
    return (parsed_temp >= 16 && parsed_temp <= 30) ? parsed_temp : Temp;
}

static bool app_driver_ir_save_pairing();
static bool app_driver_ir_erase_saved_pairing();
static bool app_driver_ir_load_pairing();

static bool app_driver_ir_worker_wait_idle(
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
                TAG_IR,
                "Timed out waiting for IR transmit to become idle");
            return false;
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return true;
}

static void app_driver_ir_worker_stop_capture()
{
    if (!s_ac) {
        return;
    }

    s_ac->stop_capture();
    s_ir_worker_state = ir_worker_state_t::IDLE;
}

static void app_driver_ir_alt_reset_tracking()
{
    s_ir_alt_traversal_active.store(false);
    s_ir_alt_phase = ir_alt_phase_t::INACTIVE;
    s_ir_alt_predefined_number = 0;
    s_ir_match_index = 0;
}

static void app_driver_ir_alt_prepare_led()
{
    app_driver_led_cancel_one_shot();
    app_driver_led_set_static(false);
}

static void app_driver_ir_alt_show_protocol_number(uint8_t number)
{
    app_driver_led_one_shot(number == 0 ? 1 : number, 500, 1100);
}

static void app_driver_ir_worker_accept_alt_selection()
{
    if (!s_ir_alt_traversal_active.load()) {
        return;
    }

    const uint8_t accepted_number =
        s_ir_match_index == 0 ? 1 : s_ir_match_index;

    app_driver_ir_alt_reset_tracking();
    app_driver_led_cancel_one_shot();
    app_driver_led_set_mode(LED_MODE_BREATH_3S);

    ESP_LOGI(
        TAG_IR,
        "Alt protocol selection accepted by Matter command: index=%u protocol=%s",
        static_cast<unsigned>(accepted_number),
        s_ac ? s_ac->protocol_name() : "none");
}

static void app_driver_ir_worker_finish_alt_unpaired()
{
    if (s_ac) {
        s_ac->stop_capture();
        s_ac->clear_pairing();
        s_ac->init_ok = s_ac->ready();
    }

    s_ir_pairing.store(false);
    s_ir_paired.store(false);
    s_ir_match_index = 0;
    s_ir_worker_state = ir_worker_state_t::STOPPED;

    if (!app_driver_ir_erase_saved_pairing()) {
        ESP_LOGW(TAG_IR, "Alt traversal finished, but saved pairing erase failed");
    }

    app_driver_ir_alt_reset_tracking();
    app_driver_led_cancel_one_shot();
    app_driver_ir_apply_local_led();

    ESP_LOGI(TAG_IR, "Alt traversal completed; device returned to unpaired state");
}

static void app_driver_ir_worker_alt_next()
{
    if (!s_ac || s_factory_reset_in_progress.load()) {
        ESP_LOGW(TAG_IR, "Alt traversal ignored: IR controller unavailable or reset active");
        return;
    }

    if (s_ir_pairing.load()) {
        s_ac->stop_capture();
        s_ir_pairing.store(false);
        s_ac->clear_pairing();
        s_ac->init_ok = s_ac->ready();
        s_ir_paired.store(false);
    }

    if (!s_ir_alt_traversal_active.load()) {
        s_ir_alt_traversal_active.store(true);
        s_ir_alt_phase = ir_alt_phase_t::PREDEFINED;
        s_ir_match_index = 0;
        s_ir_alt_predefined_number = 0;
        app_driver_ir_alt_prepare_led();
        ESP_LOGI(TAG_IR, "Alt traversal started over IRremoteESP8266 AC protocols");
    } else {
        app_driver_ir_alt_prepare_led();
    }

    app_driver_ir_worker_stop_capture();
    if (!app_driver_ir_worker_wait_idle()) {
        return;
    }

    const size_t count = s_ac->alt_protocol_count();
    while (s_ir_match_index < count) {
        const size_t index = s_ir_match_index;
        ++s_ir_match_index;

        if (!s_ac->apply_alt_index(index)) {
            ESP_LOGW(TAG_IR, "Failed to apply alt protocol index %u",
                     static_cast<unsigned>(index + 1));
            continue;
        }

        s_ir_paired.store(true);
        s_ac->init_ok = true;
        s_ir_worker_state = ir_worker_state_t::IDLE;

        Temp = 25;
        Mode = 1;
        Fan = 0;
        PowerOn = true;

        const bool saved = app_driver_ir_save_pairing();
        app_driver_ir_alt_show_protocol_number(static_cast<uint8_t>(index + 1));

        ESP_LOGI(TAG_IR,
                 "Alt protocol candidate %u/%u selected: %s saved=%d",
                 static_cast<unsigned>(index + 1),
                 static_cast<unsigned>(count),
                 s_ac->protocol_name(),
                 static_cast<int>(saved));
        return;
    }

    app_driver_ir_worker_finish_alt_unpaired();
}

static void app_driver_ir_worker_send_power(const ir_command_t &command)
{
    if (!s_ac || !s_ac->init_ok || !s_ir_paired.load() ||
        s_ir_pairing.load() || s_factory_reset_in_progress.load()) {
        ESP_LOGW(TAG_IR,
                 "Power command ignored: ready=%d paired=%d pairing=%d reset=%d",
                 s_ac != nullptr && s_ac->ready(),
                 s_ir_paired.load(),
                 s_ir_pairing.load(),
                 s_factory_reset_in_progress.load());
        return;
    }

    display_activity_notify();
    app_driver_ir_worker_accept_alt_selection();
    app_driver_ir_worker_stop_capture();
    if (!app_driver_ir_worker_wait_idle()) {
        return;
    }

    if (!s_ac->send_power(command.power_on)) {
        ESP_LOGE(TAG_IR, "IR power command failed: Power=%d",
                 static_cast<int>(command.power_on));
    } else {
        ESP_LOGI(TAG_IR, "IR power command submitted: Power=%d",
                 static_cast<int>(command.power_on));
    }

    s_ir_worker_state = ir_worker_state_t::IDLE;
}

static void app_driver_ir_worker_send_state(const ir_command_t &command)
{
    if (!s_ac || !s_ac->init_ok || !s_ir_paired.load() ||
        s_ir_pairing.load() || s_factory_reset_in_progress.load()) {
        ESP_LOGW(TAG_IR,
                 "State command ignored: ready=%d paired=%d pairing=%d reset=%d",
                 s_ac != nullptr && s_ac->ready(),
                 s_ir_paired.load(),
                 s_ir_pairing.load(),
                 s_factory_reset_in_progress.load());
        return;
    }

    display_activity_notify();

    app_driver_ir_worker_accept_alt_selection();
    app_driver_ir_worker_stop_capture();
    if (!app_driver_ir_worker_wait_idle()) {
        return;
    }

    ir_ac::AcLogicalState state{};
    state.temp_c = command.temp;
    state.mode = command.mode;
    state.fan = command.fan;
    state.power = command.power_on;

    if (!s_ac->send_state(state)) {
        ESP_LOGE(TAG_IR,
                 "IR state command failed: Temp=%d Mode=%d Fan=%d Key=%d Power=%d",
                 command.temp, command.mode, command.fan, command.key,
                 static_cast<int>(command.power_on));
    } else {
        ESP_LOGI(TAG_IR,
                 "IR state command submitted: Temp=%d Mode=%d Fan=%d Key=%d Power=%d",
                 command.temp, command.mode, command.fan, command.key,
                 static_cast<int>(command.power_on));
    }

    s_ir_worker_state = ir_worker_state_t::IDLE;
}

static void app_driver_ir_worker_start_pairing()
{
    if (!s_ac || s_factory_reset_in_progress.load()) {
        return;
    }

    if (s_ir_alt_traversal_active.load()) {
        app_driver_ir_alt_reset_tracking();
        app_driver_led_cancel_one_shot();
    }

    app_driver_ir_worker_stop_capture();
    if (!app_driver_ir_worker_wait_idle()) {
        ESP_LOGE(TAG_IR, "Cannot start pairing while IR transmit remains busy");
        return;
    }

    s_ac->clear_pairing();
    s_ac->init_ok = s_ac->ready();
    s_ir_paired.store(false);
    s_ir_match_index = 0;
    s_ir_pairing.store(true);
    s_ir_pairing_started_tick = xTaskGetTickCount();
    s_ir_pairing_last_wait_log_tick = s_ir_pairing_started_tick;
    s_ir_pairing_last_rearm_tick = s_ir_pairing_started_tick;
    s_ac->start_capture();
    s_ir_worker_state = ir_worker_state_t::PAIRING;

    const int rx_level = gpio_get_level(IR_RX_PIN);
    ESP_LOGI(TAG_IR,
             "Pairing started. Point the AC remote at the IR receiver and press any button.");
    ESP_LOGI(TAG_IR,
             "IR RX GPIO%d idle level=%d (demodulator should idle HIGH=1). "
             "Timeout %u s; tap Learn again to cancel.",
             static_cast<int>(IR_RX_PIN), rx_level,
             static_cast<unsigned>(k_ir_learn_timeout_ms / 1000));
    if (rx_level == 0) {
        ESP_LOGW(TAG_IR,
                 "IR RX idle is LOW — check receiver VCC/GND and that the "
                 "yellow OUT wire is on GPIO%d (not LED/TX)",
                 static_cast<int>(IR_RX_PIN));
    }
    app_driver_led_set_mode(LED_MODE_BLINK_3HZ);
}

static void app_driver_ir_worker_cancel_pairing()
{
    if (!s_ac || !s_ir_pairing.load()) {
        return;
    }

    if (s_ir_worker_state == ir_worker_state_t::PAIRING) {
        s_ac->stop_capture();
    }

    s_ac->clear_pairing();
    s_ac->init_ok = s_ac->ready();
    s_ir_pairing.store(false);

    const bool pairing_restored = app_driver_ir_load_pairing();
    s_ir_paired.store(pairing_restored);
    s_ir_worker_state = ir_worker_state_t::STOPPED;
    if (pairing_restored && s_ir_parse_earliest_tick == 0) {
        s_ir_parse_earliest_tick =
            xTaskGetTickCount() + pdMS_TO_TICKS(k_ir_parse_settle_ms);
    }

    ESP_LOGI(TAG_IR, "IR pairing cancelled; load_pairing result: %s",
             pairing_restored ? "restored" : "unpaired");
    app_driver_ir_apply_local_led();
}

static void app_driver_ir_worker_toggle_pairing()
{
    if (s_ir_pairing.load()) {
        app_driver_ir_worker_cancel_pairing();
    } else {
        app_driver_ir_worker_start_pairing();
    }
}

static void app_driver_ir_worker_clear_pairing(const ir_command_t &command)
{
    app_driver_ir_alt_reset_tracking();

    if (s_ac) {
        app_driver_ir_worker_stop_capture();
        app_driver_ir_worker_wait_idle();
        s_ac->clear_pairing();
        s_ac->init_ok = s_ac->ready();
    }

    s_ir_pairing.store(false);
    s_ir_paired.store(false);
    s_ir_worker_state = ir_worker_state_t::STOPPED;

    const bool erased = app_driver_ir_erase_saved_pairing();
    s_ir_last_clear_success.store(erased);

    if (command.completion_task != nullptr) {
        xTaskNotifyGive(command.completion_task);
    }

    app_driver_ir_apply_local_led();
}

static void app_driver_ir_worker_process_command(const ir_command_t &command)
{
    switch (command.type) {
        case ir_command_type_t::SEND_POWER:
            app_driver_ir_worker_send_power(command);
            break;
        case ir_command_type_t::SEND_STATE:
            app_driver_ir_worker_send_state(command);
            break;
        case ir_command_type_t::TOGGLE_PAIRING:
            app_driver_ir_worker_toggle_pairing();
            break;
        case ir_command_type_t::ALT_NEXT:
            app_driver_ir_worker_alt_next();
            break;
        case ir_command_type_t::CLEAR_PAIRING:
            app_driver_ir_worker_clear_pairing(command);
            break;
    }
}

static void app_driver_ir_worker_handle_pairing()
{
    if (!s_ac || !s_ir_pairing.load() ||
        s_ir_worker_state != ir_worker_state_t::PAIRING) {
        return;
    }

    const TickType_t now = xTaskGetTickCount();
    const uint32_t elapsed_ms =
        (now - s_ir_pairing_started_tick) * portTICK_PERIOD_MS;
    if (elapsed_ms >= k_ir_learn_timeout_ms) {
        ESP_LOGW(TAG_IR,
                 "IR learn timed out after %u s with no usable frame "
                 "(RX GPIO%d level=%d)",
                 static_cast<unsigned>(elapsed_ms / 1000),
                 static_cast<int>(IR_RX_PIN), gpio_get_level(IR_RX_PIN));
        app_driver_ir_worker_cancel_pairing();
        return;
    }

    if ((now - s_ir_pairing_last_wait_log_tick) * portTICK_PERIOD_MS >=
        k_ir_learn_wait_log_ms) {
        s_ir_pairing_last_wait_log_tick = now;
        ESP_LOGI(TAG_IR,
                 "Still waiting for IR on GPIO%d (level=%d, armed=%d, %u s left)",
                 static_cast<int>(IR_RX_PIN), gpio_get_level(IR_RX_PIN),
                 static_cast<int>(s_ac->capture_armed()),
                 static_cast<unsigned>(
                     (k_ir_learn_timeout_ms - elapsed_ms + 999) / 1000));
    }

    if (s_ac->signal_captured()) {
        ESP_LOGI(TAG_IR, "IR signal captured for pairing; decoding protocol");

        const bool ok = s_ac->pair_from_capture();
        s_ir_pairing.store(false);
        app_driver_ir_alt_reset_tracking();

        /*
         * pair_from_capture() already stopped RX. Keep RX unarmed and the
         * worker STOPPED through the settle window so UI can leave LEARN and
         * CHIP can process deferred LED/Matter work without racing RMT.
         */
        if (s_ac) {
            s_ac->stop_capture();
        }
        s_ir_worker_state = ir_worker_state_t::STOPPED;

        if (ok) {
            /*
             * Keep local power Off to match Matter defaults — do not imply the
             * AC is running, and do not fire an IR TX from the learn path.
             */
            Temp = 25;
            Mode = 1;
            Fan = 0;
            PowerOn = false;
            s_ir_match_index = 0;
            s_ir_paired.store(true);
            s_ac->init_ok = true;

            ESP_LOGI(TAG_IR, "IR AC pairing succeeded: protocol=%s",
                     s_ac->protocol_name());

            if (!app_driver_ir_save_pairing()) {
                ESP_LOGW(TAG_IR, "Pairing succeeded but NVS save failed");
            }

            s_ir_parse_earliest_tick =
                xTaskGetTickCount() + pdMS_TO_TICKS(k_ir_parse_settle_ms);

            app_driver_ir_apply_local_led();
            /* Matter attributes only — no IR transmit. */
            app_matter_schedule_whole_device_state(false, Mode);
            app_matter_schedule_report_all_temperatures(
                static_cast<int16_t>(Temp * 100));
        } else {
            s_ac->clear_pairing();
            s_ac->init_ok = s_ac->ready();
            s_ir_paired.store(false);
            s_ir_parse_earliest_tick = 0;

            ESP_LOGW(TAG_IR,
                     "IR AC pairing failed after IR capture "
                     "(unsupported or unrecognized AC protocol)");
            app_driver_ir_erase_saved_pairing();
            app_driver_ir_apply_local_led();
        }

        return;
    }

    /* Re-arm only when nothing is pending — never after a ready frame.
     * Throttle retries so a persistent rmt_receive failure cannot spam logs. */
    if (!s_ac->capture_armed() &&
        (now - s_ir_pairing_last_rearm_tick) * portTICK_PERIOD_MS >=
            k_ir_learn_rearm_ms) {
        s_ir_pairing_last_rearm_tick = now;
        s_ac->start_capture();
    }
}

static void app_driver_ir_worker_parse_signal()
{
    if (!s_ac) {
        s_ir_worker_state = ir_worker_state_t::STOPPED;
        return;
    }

    ir_ac::AcLogicalState parsed{};
    if (!s_ac->parse_capture(&parsed)) {
        ESP_LOGW(TAG_IR,
                 "Captured IR signal could not be parsed as the paired AC protocol");
        s_ir_worker_state = ir_worker_state_t::STOPPED;
        return;
    }

    Temp = app_driver_ir_temperature_to_celsius(parsed.temp_c);
    Mode = parsed.mode;
    /* Matter/app IR TX always uses Auto; ignore remote fan band. */
    Fan = 0;
    PowerOn = parsed.power;

    ESP_LOGI(TAG_IR,
             "Parsed remote state: Temp=%d Mode=%d Fan(remote)=%d "
             "Power=%d (kept IR Fan=Auto)",
             Temp, Mode, parsed.fan, static_cast<int>(PowerOn));

    display_activity_notify();
    app_matter_schedule_parsed_ac_state(Temp, Mode, PowerOn);
    /* STOPPED → handle_parsing re-arms RX on the next idle tick. */
    s_ir_worker_state = ir_worker_state_t::STOPPED;
}

static void app_driver_ir_worker_handle_parsing(bool &parsing_was_enabled)
{
    const bool should_parse =
        s_matter_subscription_active.load() &&
        s_ir_paired.load() &&
        !s_ir_pairing.load() &&
        !s_ir_alt_traversal_active.load() &&
        !s_factory_reset_in_progress.load() &&
        s_ac != nullptr &&
        s_ac->init_ok;

    if (!should_parse) {
        if (s_ac) {
            s_ac->stop_capture();
        }
        s_ir_worker_state = ir_worker_state_t::STOPPED;

        if (parsing_was_enabled) {
            parsing_was_enabled = false;
            ESP_LOGI(TAG_IR, "IR parsing disabled");
        }
        return;
    }

    /* Post-learn / post-restore settle: stay unarmed. */
    if (s_ir_parse_earliest_tick != 0 &&
        xTaskGetTickCount() < s_ir_parse_earliest_tick) {
        if (s_ac) {
            s_ac->stop_capture();
        }
        s_ir_worker_state = ir_worker_state_t::STOPPED;
        return;
    }
    s_ir_parse_earliest_tick = 0;

    if (!parsing_was_enabled) {
        parsing_was_enabled = true;
        ESP_LOGI(TAG_IR, "IR parsing enabled: paired and subscribed");
    }

    if (s_ac->is_busy()) {
        return;
    }

    if (s_ir_worker_state == ir_worker_state_t::STOPPED ||
        s_ir_worker_state == ir_worker_state_t::IDLE) {
        s_ac->start_capture();
        if (!s_ac->capture_armed()) {
            ESP_LOGW(TAG_IR, "IR parsing RX arm failed; will retry");
            s_ir_worker_state = ir_worker_state_t::STOPPED;
            return;
        }
        s_ir_worker_state = ir_worker_state_t::PARSING;
        ESP_LOGI(TAG_IR, "IR parsing RX armed");
        return;
    }

    if (s_ir_worker_state == ir_worker_state_t::PARSING &&
        s_ac->signal_captured()) {
        app_driver_ir_worker_parse_signal();
    }
}

static void app_driver_ir_worker_task(void *arg)
{
    bool parsing_was_enabled = false;
    ESP_LOGI(TAG_IR, "IR worker task started");

    for (;;) {
        ir_command_t command{};

        if (xQueueReceive(s_ir_command_queue, &command, pdMS_TO_TICKS(20)) ==
            pdTRUE) {
            app_driver_ir_worker_process_command(command);
            while (xQueueReceive(s_ir_command_queue, &command, 0) == pdTRUE) {
                app_driver_ir_worker_process_command(command);
            }
            continue;
        }

        if (s_ir_worker_state == ir_worker_state_t::PAIRING) {
            app_driver_ir_worker_handle_pairing();
            continue;
        }

        app_driver_ir_worker_handle_parsing(parsing_was_enabled);
    }
}

static esp_err_t app_driver_ir_start_worker()
{
    if (s_ir_worker_task_handle != nullptr) {
        return ESP_OK;
    }

    if (s_ir_command_queue == nullptr) {
        s_ir_command_queue =
            xQueueCreate(12, sizeof(ir_command_t));
        if (s_ir_command_queue == nullptr) {
            ESP_LOGE(TAG_IR, "Failed to create IR command queue");
            return ESP_ERR_NO_MEM;
        }
    }

    BaseType_t result = xTaskCreate(
        app_driver_ir_worker_task,
        "ir_worker",
        /* AC protocol decode needs a deep stack; 8KB crashed in TLSF. */
        24576,
        nullptr,
        5,
        &s_ir_worker_task_handle);

    if (result != pdPASS) {
        s_ir_worker_task_handle = nullptr;
        vQueueDelete(s_ir_command_queue);
        s_ir_command_queue = nullptr;
        ESP_LOGE(TAG_IR, "Failed to create IR worker task");
        return ESP_ERR_NO_MEM;
    }

    return ESP_OK;
}

static void app_driver_ir_dump_config()
{
    const esp_app_desc_t *app_desc = esp_app_get_description();

    ESP_LOGI(TAG_IR, "IRremoteESP8266 AC Configuration:");
    ESP_LOGI(TAG_IR, "  Project name: %s",
             app_desc != nullptr ? app_desc->project_name : "unknown");
    ESP_LOGI(TAG_IR, "  Project version: %s",
             app_desc != nullptr ? app_desc->version : "unknown");
    ESP_LOGI(TAG_IR, "  IR TX pin: GPIO%d", static_cast<int>(IR_TX_PIN));
    ESP_LOGI(TAG_IR, "  IR RX pin: GPIO%d", static_cast<int>(IR_RX_PIN));

    if (s_ac) {
        ESP_LOGI(TAG_IR, "  IR Library Version: %s", s_ac->lib_version());
        ESP_LOGI(TAG_IR, "  Paired: %s", s_ir_paired.load() ? "yes" : "no");
        if (s_ir_paired.load()) {
            ESP_LOGI(TAG_IR, "  Protocol: %s", s_ac->protocol_name());
        }
    } else {
        ESP_LOGI(TAG_IR, "  IR Library Version: unavailable");
        ESP_LOGI(TAG_IR, "  Paired: no");
    }
}


static bool app_driver_ir_save_pairing()
{
    if (!s_ac || !s_ac->init_ok || !s_ac->paired()) {
        ESP_LOGW(TAG_IR,
                 "Cannot save pairing data: IR AC is not paired");
        return false;
    }

    const ir_ac::AcPairingInfo info = s_ac->pairing();

    ir_stored_pairing_data_t saved{};
    saved.magic = IR_PAIRING_MAGIC;
    saved.version = IR_PAIRING_VERSION;
    saved.struct_size = sizeof(saved);
    saved.protocol = info.protocol;
    saved.model = info.model;
    saved.alt_index = static_cast<uint8_t>(s_ac->current_alt_index());
    saved.reserved = 0;

    nvs_handle_t nvs_handle = 0;
    esp_err_t err = nvs_open(
        IR_NVS_NAMESPACE,
        NVS_READWRITE,
        &nvs_handle
    );

    if (err != ESP_OK) {
        ESP_LOGE(TAG_IR,
                 "Failed to open IR NVS namespace: %s",
                 esp_err_to_name(err));
        return false;
    }

    err = nvs_set_blob(
        nvs_handle,
        IR_NVS_PAIRING_KEY,
        &saved,
        sizeof(saved)
    );

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(TAG_IR,
                 "Failed to save IR pairing data: %s",
                 esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(TAG_IR,
             "IR pairing data saved: protocol=%d model=%d alt_index=%u",
             static_cast<int>(saved.protocol),
             static_cast<int>(saved.model),
             static_cast<unsigned>(saved.alt_index));
    return true;
}

/*
 * Erase only the application-owned IR pairing record.
 *
 * Matter's factory reset is performed separately after this function
 * returns. ESP_ERR_NVS_NOT_FOUND is treated as success because the desired
 * end state is already satisfied.
 */
static bool app_driver_ir_erase_saved_pairing()
{
    nvs_handle_t nvs_handle = 0;

    esp_err_t err =
        nvs_open(
            IR_NVS_NAMESPACE,
            NVS_READWRITE,
            &nvs_handle);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(
            TAG_IR,
            "IR pairing namespace is already absent");
        return true;
    }

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG_IR,
            "Failed to open IR NVS namespace for erase: %s",
            esp_err_to_name(err));
        return false;
    }

    err =
        nvs_erase_key(
            nvs_handle,
            IR_NVS_PAIRING_KEY);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }

    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }

    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG_IR,
            "Failed to erase IR pairing data: %s",
            esp_err_to_name(err));
        return false;
    }

    ESP_LOGI(
        TAG_IR,
        "IR pairing data erased");

    return true;
}

/*
 * Clear the runtime pairing state and erase the application-owned IR
 * pairing record. Matter factory reset is performed separately.
 */
static bool app_driver_ir_clear_pairing()
{
    if (s_ir_command_queue == nullptr ||
        s_ir_worker_task_handle == nullptr) {

        ESP_LOGW(
            TAG_IR,
            "IR worker is unavailable; erasing saved pairing only");

        s_ir_paired.store(false);
        return app_driver_ir_erase_saved_pairing();
    }

    /*
     * Remove any stale notification before waiting for this clear request.
     */
    ulTaskNotifyTake(
        pdTRUE,
        0);

    s_ir_last_clear_success.store(false);

    ir_command_t command{};
    command.type =
        ir_command_type_t::CLEAR_PAIRING;
    command.completion_task =
        xTaskGetCurrentTaskHandle();

    esp_err_t queue_err =
        app_driver_ir_enqueue_command(
            command,
            pdMS_TO_TICKS(200));

    if (queue_err != ESP_OK) {
        return false;
    }

    if (ulTaskNotifyTake(
            pdTRUE,
            pdMS_TO_TICKS(5000)) == 0) {

        ESP_LOGE(
            TAG_IR,
            "Timed out waiting for IR pairing clear");
        return false;
    }

    return s_ir_last_clear_success.load();
}

static bool app_driver_ir_load_pairing()
{
    if (!s_ac) {
        return false;
    }

    s_ac->clear_pairing();
    s_ac->init_ok = s_ac->ready();
    s_ir_paired.store(false);

    nvs_handle_t nvs_handle = 0;
    esp_err_t err = nvs_open(
        IR_NVS_NAMESPACE,
        NVS_READONLY,
        &nvs_handle
    );

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG_IR, "No saved IR pairing data");
        return false;
    }

    if (err != ESP_OK) {
        ESP_LOGW(TAG_IR,
                 "Failed to open IR NVS namespace: %s",
                 esp_err_to_name(err));
        return false;
    }

    size_t stored_size = 0;
    err = nvs_get_blob(
        nvs_handle,
        IR_NVS_PAIRING_KEY,
        nullptr,
        &stored_size
    );

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        nvs_close(nvs_handle);
        ESP_LOGI(TAG_IR, "No saved IR pairing data");
        return false;
    }

    if (err != ESP_OK) {
        nvs_close(nvs_handle);
        ESP_LOGW(TAG_IR,
                 "Failed to query IR pairing data: %s",
                 esp_err_to_name(err));
        return false;
    }

    if (stored_size != sizeof(ir_stored_pairing_data_t)) {
        nvs_close(nvs_handle);
        ESP_LOGW(TAG_IR,
                 "Saved IR pairing data size mismatch: stored=%u expected=%u",
                 static_cast<unsigned>(stored_size),
                 static_cast<unsigned>(sizeof(ir_stored_pairing_data_t)));
        return false;
    }

    ir_stored_pairing_data_t saved{};
    err = nvs_get_blob(
        nvs_handle,
        IR_NVS_PAIRING_KEY,
        &saved,
        &stored_size
    );
    nvs_close(nvs_handle);

    if (err != ESP_OK) {
        ESP_LOGW(TAG_IR,
                 "Failed to read IR pairing data: %s",
                 esp_err_to_name(err));
        return false;
    }

    if (saved.magic != IR_PAIRING_MAGIC ||
        saved.version != IR_PAIRING_VERSION ||
        saved.struct_size != sizeof(saved)) {
        ESP_LOGW(TAG_IR,
                 "Saved IR pairing data has an unsupported format");
        return false;
    }

    ir_ac::AcPairingInfo info{};
    info.protocol = saved.protocol;
    info.model = saved.model;
    info.valid = true;

    if (!s_ac->apply_pairing(info)) {
        ESP_LOGW(TAG_IR,
                 "Saved IR pairing data exists, but protocol apply failed");
        s_ac->clear_pairing();
        s_ac->init_ok = s_ac->ready();
        return false;
    }

    s_ir_match_index = saved.alt_index;
    s_ac->init_ok = true;
    s_ir_paired.store(true);
    /* Avoid arming continuous IR RX while Matter is still bringing up. */
    s_ir_parse_earliest_tick =
        xTaskGetTickCount() + pdMS_TO_TICKS(2000);

    ESP_LOGI(TAG_IR,
             "IR pairing restored: protocol=%s model=%d alt_index=%u",
             s_ac->protocol_name(),
             static_cast<int>(saved.model),
             static_cast<unsigned>(saved.alt_index));
    return true;
}

static esp_err_t app_driver_ir_init();
static esp_err_t app_driver_ir_start_worker();

esp_err_t app_driver_ir_ensure_ready(void)
{
    if (s_ac == nullptr) {
        ESP_LOGW(TAG_IR,
                 "IR controller missing; initializing now (free heap=%u)",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        esp_err_t err = app_driver_ir_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG_IR, "IR init failed: %s (free heap=%u)",
                     esp_err_to_name(err),
                     static_cast<unsigned>(esp_get_free_heap_size()));
            return err;
        }
    }

    if (s_ir_command_queue == nullptr ||
        s_ir_worker_task_handle == nullptr) {
        ESP_LOGW(TAG_IR,
                 "IR worker missing; starting now (free heap=%u)",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        esp_err_t err = app_driver_ir_start_worker();
        if (err != ESP_OK) {
            ESP_LOGE(TAG_IR, "IR worker start failed: %s (free heap=%u)",
                     esp_err_to_name(err),
                     static_cast<unsigned>(esp_get_free_heap_size()));
            return err;
        }
    }

    return ESP_OK;
}

static void app_driver_ir_request_pairing_toggle()
{
    if (s_factory_reset_in_progress.load()) {
        ESP_LOGW(
            TAG_IR,
            "Cannot toggle pairing during factory reset");
        return;
    }

    if (app_driver_ir_ensure_ready() != ESP_OK) {
        ESP_LOGE(TAG_IR, "Cannot toggle pairing: IR path not ready");
        return;
    }

    esp_err_t err =
        app_driver_ir_queue_pairing_toggle();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG_IR,
            "Failed to queue IR pairing toggle: %s",
            esp_err_to_name(err));
    }
}

static void app_driver_ir_request_alt_next()
{
    if (s_factory_reset_in_progress.load()) {
        ESP_LOGW(
            TAG_IR,
            "Cannot advance Alt traversal during factory reset");
        return;
    }

    esp_err_t err =
        app_driver_ir_queue_alt_next();

    if (err != ESP_OK) {
        ESP_LOGE(
            TAG_IR,
            "Failed to queue Alt traversal command: %s",
            esp_err_to_name(err));
    }
}

/*
 * BOOT single-click no longer starts IR learn — that is done from the
 * touchscreen Learn button after Matter commissioning.
 */
static void app_driver_button_double_click_cb(void *arg, void *data)
{
    ESP_LOGI(
        TAG,
        "Button double clicked: advance IR Alt protocol traversal");

    app_driver_ir_request_alt_next();
}

bool app_driver_ir_is_paired(void)
{
    return s_ir_paired.load();
}

bool app_driver_ir_is_pairing(void)
{
    return s_ir_pairing.load();
}

void app_driver_ir_start_learn(void)
{
    if (s_factory_reset_in_progress.load()) {
        return;
    }
    /* Toggle: first tap starts learn, second tap cancels and restores the button. */
    app_driver_ir_request_pairing_toggle();
}

void app_driver_ui_get_ac_state(int *temp_c, bool *power_on)
{
    if (temp_c) {
        *temp_c = Temp;
    }
    if (power_on) {
        *power_on = PowerOn;
    }
}

void app_driver_ui_toggle_power(void)
{
    esp_matter_attr_val_t val = esp_matter_bool(!PowerOn);
    app_driver_room_air_conditioner_set_power(&val);
}

void app_driver_ui_adjust_temp(int delta)
{
    int next = Temp + delta;
    if (next < 16) {
        next = 16;
    } else if (next > 30) {
        next = 30;
    }
    Temp = next;
    PowerOn = true;

    const int16_t temp_x100 = static_cast<int16_t>(Temp * 100);
    app_matter_schedule_report_all_temperatures(temp_x100);
    app_matter_schedule_whole_device_state(PowerOn, Mode);
    app_driver_ir_queue_state(Temp, Mode, Fan, 0, PowerOn);
}

void app_driver_ui_set_light_brightness(uint8_t level_1_254)
{
    if (level_1_254 < 1) {
        level_1_254 = 1;
    }
    ws2812_temp_light_set_brightness(level_1_254);

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        [](intptr_t arg) {
            const uint8_t level = static_cast<uint8_t>(arg);
            esp_matter_attr_val_t val = esp_matter_nullable_uint8(level);
            s_matter_syncing_from_local = true;
            attribute::update(
                temp_light_endpoint_id,
                LevelControl::Id,
                LevelControl::Attributes::CurrentLevel::Id,
                &val);
            s_matter_syncing_from_local = false;
        },
        static_cast<intptr_t>(level_1_254));
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG,
                 "ScheduleWork for light brightness failed: %" CHIP_ERROR_FORMAT,
                 err.Format());
    }
}


/*
 * The button component's existing long-press threshold is preserved.
 * In the current project this event occurs after holding for about 5 seconds.
 *
 * Threshold reached:
 *   LED starts blinking at 3 Hz and continues while the button is held.
 *
 * Released:
 *   IR pairing data is erased immediately,
 *   then Matter factory reset is started.
 *
 * Do not spawn a dedicated FreeRTOS task here: under Matter + BLE + Wi-Fi the
 * heap is often too fragmented to allocate another stack (seen as
 * "Failed to create factory-reset task"). Run on the CHIP work queue, with a
 * same-context fallback if scheduling fails.
 */
static void app_driver_factory_reset_work(intptr_t /*arg*/)
{
    ESP_LOGI(
        TAG,
        "Factory reset confirmed; erasing pairing information");

    const bool pairing_erased =
        app_driver_ir_clear_pairing();

    if (!pairing_erased) {
        /*
         * Continue with Matter reset, but leave a clear diagnostic.
         * A future boot will not silently claim that the IR record was
         * successfully removed.
         */
        ESP_LOGE(
            TAG,
            "IR pairing erase failed; continuing Matter factory reset");
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
     * The IR worker remains alive in STOPPED state after CLEAR_PAIRING.
     */
    s_factory_reset_in_progress.store(false);

    app_driver_update_led_states();
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

    if (s_factory_reset_in_progress.exchange(true)) {
        ESP_LOGW(
            TAG,
            "Factory reset is already running");
        return;
    }

    ESP_LOGI(
        TAG,
        "Factory reset release confirmed; starting erase immediately "
        "(free heap=%u)",
        static_cast<unsigned>(esp_get_free_heap_size()));

    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
        app_driver_factory_reset_work, 0);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(
            TAG,
            "CHIP ScheduleWork failed (%" CHIP_ERROR_FORMAT "); "
            "running factory reset on button task",
            err.Format());
        app_driver_factory_reset_work(0);
    }
}

esp_err_t app_driver_attribute_update(app_driver_handle_t driver_handle, uint16_t endpoint_id, uint32_t cluster_id,
                                      uint32_t attribute_id, esp_matter_attr_val_t *val)
{
	int8_t Key = -1;
// Key Value Range:
//0 - Temperature +
//1 - Temperature -
//2 - Mode
//3 - Fan Speed (unused; IR fan fixed to Auto)
//4 - Power

	(void) driver_handle;
	if (s_matter_syncing_from_local) {
	    return ESP_OK;
	}
    /* Controller / local UI writes count as user activity — wake the LCD. */
    display_activity_notify();
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
			     * After the current write completes, synchronize cooling
			     * and heating setpoints (and LocalTemperature when no SHT30).
			     * Skip when Matter already has this setpoint — controllers
			     * often rewrite the same value and used to flood the CHIP
			     * event queue via report→callback→reschedule loops.
			     */
			    if (rounded_temp != s_last_synced_temp_x100) {
			        app_matter_schedule_report_all_temperatures(rounded_temp);
			    }

            }
            
            // 2.2 Handle air-conditioner mode changes (SystemMode)
            else if (attribute_id == Thermostat::Attributes::SystemMode::Id) {
				Key = 2;
				uint8_t matter_mode = val->val.u8;
                /*
                 * Matter SystemMode -> IR Mode:
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
                     * Mode unchanged; a non-Off mode restores OnOff and
                     * SystemMode consistently. IR fan remains Auto.
                     */
                    app_matter_schedule_whole_device_state(
                        PowerOn,
                        Mode);
                }

                ESP_LOGI(TAG, "Air-conditioner operating mode update received: %d", matter_mode);
            }
		}
	}
    else if (endpoint_id == temp_light_endpoint_id) {
        if (cluster_id == OnOff::Id &&
            attribute_id == OnOff::Attributes::OnOff::Id) {
            err = app_driver_temp_light_set_power(val);
        } else if (cluster_id == LevelControl::Id &&
                   attribute_id == LevelControl::Attributes::CurrentLevel::Id) {
            err = app_driver_temp_light_set_brightness(val);
        }
    }

      
    if (Key >= 0 &&
        !s_factory_reset_in_progress.load()) {

        if (!s_ir_alt_traversal_active.load()) {
            app_driver_led_set_mode(
                ONE_SHOT_ON_1S);
        }

        esp_err_t queue_err = ESP_OK;

        if (Key == 4) {
            queue_err =
                app_driver_ir_queue_power(
                    PowerOn);
        } else {
            queue_err =
                app_driver_ir_queue_state(
                    Temp,
                    Mode,
                    Fan,
                    Key,
                    PowerOn);
        }

        if (queue_err != ESP_OK) {
            ESP_LOGE(
                TAG_IR,
                "Failed to queue Matter IR command: %s",
                esp_err_to_name(queue_err));
        }
    }
    return err;
}

esp_err_t app_driver_room_air_conditioner_set_defaults(
    uint16_t endpoint_id)
{
    esp_err_t first_error = ESP_OK;

    /*
     * Force a deterministic whole-device startup state:
     *   Room AC OnOff         = Off
     *   Thermostat SystemMode = Off
     *   IR fan                = Auto (not exposed over Matter)
     *
     * Do NOT queue a physical IR Off here. When a protocol such as BOSCH144
     * is already paired, encode+RMT during Matter start races the CHIP event
     * queue (null-queue assert / "Failed to post event") and boot-loops.
     * Controllers / the on-device UI still send IR via the normal path.
     */
    PowerOn = false;
    Mode = 1;
    Fan = 0;

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

    s_matter_syncing_from_local = false;

    ESP_LOGI(
        TAG,
        "Initial whole-device state forced to Off "
        "(IR Fan=Auto; no Matter fan endpoint)");

    return first_error;
}

static esp_err_t app_driver_ir_init()
{
    if (s_ac != nullptr) {
        return ESP_OK;
    }

    ESP_LOGI(TAG_IR, "IRremoteESP8266 AC init start (free heap=%u)",
             static_cast<unsigned>(esp_get_free_heap_size()));
    ESP_LOGI(TAG_IR, "TX=GPIO%d RX=GPIO%d",
             static_cast<int>(IR_TX_PIN),
             static_cast<int>(IR_RX_PIN));

    s_ac = std::make_unique<ir_ac::IrAcController>();

    esp_err_t err = s_ac->begin(IR_TX_PIN, IR_RX_PIN);
    if (err != ESP_OK) {
        ESP_LOGE(TAG_IR, "IrAcController begin failed: %s (free heap=%u)",
                 esp_err_to_name(err),
                 static_cast<unsigned>(esp_get_free_heap_size()));
        s_ac.reset();
        return err;
    }

    s_ac->stop_capture();

    ESP_LOGI(TAG_IR, "IrAcController begin OK");

    /*
     * begin() initializes only the IR hardware path. Next, attempt to
     * restore a previously saved protocol pairing from NVS.
     */
    app_driver_ir_load_pairing();
    app_driver_ir_dump_config();

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

    /* Initialize IRremoteESP8266 AC hardware path */
    esp_err_t err = app_driver_ir_ensure_ready();
    if (err != ESP_OK) {
        ESP_LOGE(TAG_IR,
                 "IR AC init/worker failed at boot (%s); will retry on Learn",
                 esp_err_to_name(err));
    } else if (s_ir_paired.load()) {
        app_driver_led_set_mode(LED_MODE_BREATH_3S);
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
    /*
     * FabricTable / InteractionModelEngine must only be touched on the CHIP
     * stack. The IR worker used to call this directly after learn/clear and
     * could LoadProhibited (null IM engine fields) or corrupt CHIP state.
     */
    if (!chip::DeviceLayer::PlatformMgr().IsChipStackLockedByCurrentThread()) {
        CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(
            [](intptr_t) { app_driver_update_led_states(); }, 0);
        if (err != CHIP_NO_ERROR) {
            ESP_LOGW(TAG,
                     "Deferred LED update schedule failed: %" CHIP_ERROR_FORMAT,
                     err.Format());
        }
        return;
    }

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

    if (s_ir_alt_traversal_active.load()) {
        app_driver_led_set_static(false);
        return;
    }

	matter_state = app_get_matter_state_locked();

	if (!s_ir_paired.load() &&
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

	if (s_ir_pairing.load()) {
		app_driver_led_set_mode(LED_MODE_BLINK_3HZ);
        return;
	}	

    if (!s_ir_paired.load() &&
        matter_state == app_matter_state_t::SUBSCRIBED) {

        app_driver_led_set_mode(
            LED_MODE_BLINK_1X_1HZ);
        return;
    }

    if (s_ir_paired.load()) {
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
