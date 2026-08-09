/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_event.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <nvs_flash.h>
#include <cstdio>
#include <cstring>

#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE || CONFIG_SW_COEXIST_ENABLE
#include <esp_coexist.h>
#endif

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <common_macros.h>
#include <app_priv.h>
//#include <app_reset.h>
#include <atomic>

#include <platform/CHIPDeviceLayer.h>
#include <platform/ESP32/ESP32Config.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadHandler.h>

#include "sht30.h"
#include "ws2812_temp_light.h"
#include "board_i2c.h"
#include "display_gc9a01.h"
#include "ui.h"
#include "app_settings.h"

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>

static const char *TAG = "app_main";
uint16_t room_air_conditioner_endpoint_id = 0;
uint16_t humidity_sensor_endpoint_id = 0;
uint16_t temp_light_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;
static bool s_commissioning_in_progress = false;
static bool s_display_ui_started = false;
static bool s_sht30_started = false;
/* True while a CHIPoBLE link is up (PASE / commissioning). */
static bool s_chipoble_connected = false;
/*
 * After CommissioningComplete, restore LCD/SHT30 once BLE has released heap.
 * Starting LVGL while CHIPoBLE is still alive often leaves ~20KB free and the
 * 20-line draw buffer fails.
 */
static bool s_pending_post_commission_ui = false;

static constexpr const char *k_device_name = "AC Remote";
static constexpr size_t k_serial_buf_size = 17; // 12 hex chars + NUL, with headroom

static void sht30_temperature_notification(uint16_t endpoint_id, float temp_c,
                                           void *user_data);
static void sht30_humidity_notification(uint16_t endpoint_id, float humidity_pct,
                                        void *user_data);
static void app_start_display_ui(void);
static void app_start_pairing_display(void);
static void app_suspend_display_for_pase(void);
static void app_start_sht30(void);
static void app_start_ui_peripherals(void);
static void app_request_post_commission_ui(void);
static void app_try_start_pending_post_commission_ui(void);

static void app_schedule_work(chip::DeviceLayer::AsyncWorkFunct work,
                              intptr_t arg = 0)
{
    CHIP_ERROR err = chip::DeviceLayer::PlatformMgr().ScheduleWork(work, arg);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGW(TAG, "ScheduleWork failed: %" CHIP_ERROR_FORMAT, err.Format());
        /* Event-handler context is already on the CHIP stack; run inline. */
        work(arg);
    }
}

/**
 * Ensure a persistent Matter SerialNumber exists.
 * Format: 8 random hex digits + last 4 hex digits of the Wi-Fi STA MAC.
 * Example: A1B2C3D4E5F6 where E5F6 comes from MAC bytes 4 and 5.
 */
static void app_ensure_serial_number()
{
    using chip::DeviceLayer::Internal::ESP32Config;

    char serial[k_serial_buf_size] = {};
    size_t serial_len = 0;

    CHIP_ERROR err = ESP32Config::ReadConfigValueStr(
        ESP32Config::kConfigKey_SerialNum, serial, sizeof(serial), serial_len);

    if (err == CHIP_NO_ERROR && serial_len > 0) {
        ESP_LOGI(TAG, "SerialNumber: %s", serial);
        return;
    }

    uint8_t mac[6] = {};
    esp_err_t mac_err = esp_read_mac(mac, ESP_MAC_WIFI_STA);
    if (mac_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to read MAC for SerialNumber, err=%d", mac_err);
        return;
    }

    uint8_t rnd[4] = {};
    esp_fill_random(rnd, sizeof(rnd));

    std::snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X",
                  rnd[0], rnd[1], rnd[2], rnd[3], mac[4], mac[5]);

    err = ESP32Config::WriteConfigValueStr(ESP32Config::kConfigKey_SerialNum, serial);
    if (err != CHIP_NO_ERROR) {
        ESP_LOGE(TAG, "Failed to store SerialNumber, err=%" CHIP_ERROR_FORMAT, err.Format());
        return;
    }

    ESP_LOGI(TAG, "Generated SerialNumber: %s (MAC suffix %02X%02X)", serial, mac[4], mac[5]);
}

/** Set default NodeLabel so controllers show "AC Remote" before the user renames it. */
static void app_set_default_node_label(node_t *node)
{
    if (node == nullptr) {
        return;
    }

    endpoint_t *root = endpoint::get(node, 0);
    if (root == nullptr) {
        ESP_LOGE(TAG, "Root endpoint missing; cannot set NodeLabel");
        return;
    }

    cluster_t *basic = cluster::get(root, BasicInformation::Id);
    if (basic == nullptr) {
        ESP_LOGE(TAG, "Basic Information cluster missing; cannot set NodeLabel");
        return;
    }

    attribute_t *node_label =
        attribute::get(basic, BasicInformation::Attributes::NodeLabel::Id);
    if (node_label == nullptr) {
        ESP_LOGE(TAG, "NodeLabel attribute missing");
        return;
    }

    esp_matter_attr_val_t current = {};
    if (attribute::get_val(node_label, &current) == ESP_OK &&
        current.val.a.s > 0) {
        ESP_LOGI(TAG, "NodeLabel already set; leaving unchanged");
        return;
    }

    char name[32];
    std::strncpy(name, k_device_name, sizeof(name) - 1);
    name[sizeof(name) - 1] = '\0';

    esp_matter_attr_val_t val =
        esp_matter_char_str(name, static_cast<uint16_t>(std::strlen(name)));
    esp_err_t set_err = attribute::set_val(node_label, &val);
    if (set_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set NodeLabel, err=%d", set_err);
        return;
    }

    ESP_LOGI(TAG, "NodeLabel set to \"%s\"", name);
}

/*
 * Apple Home treats a flat multi-endpoint Matter node as one accessory with
 * multiple services. Expose application endpoints as Bridged Nodes under an
 * Aggregator so AC / humidity / light appear as separate Home accessories.
 */
static void app_set_bridged_device_identity(
    endpoint_t *endpoint,
    const char *node_label,
    const char *product_name)
{
    cluster_t *basic =
        cluster::get(endpoint, BridgedDeviceBasicInformation::Id);
    ABORT_APP_ON_FAILURE(
        basic != nullptr,
        ESP_LOGE(TAG, "Bridged Device Basic Information missing"));

    char label[cluster::bridged_device_basic_information::k_max_node_label_length + 1];
    char product[cluster::bridged_device_basic_information::k_max_product_name_length + 1];
    char vendor[] = "aidaegis";

    std::strncpy(label, node_label, sizeof(label) - 1);
    label[sizeof(label) - 1] = '\0';
    std::strncpy(product, product_name, sizeof(product) - 1);
    product[sizeof(product) - 1] = '\0';

    ABORT_APP_ON_FAILURE(
        cluster::bridged_device_basic_information::attribute::create_node_label(
            basic,
            label,
            static_cast<uint16_t>(std::strlen(label))) != nullptr,
        ESP_LOGE(TAG, "Failed to create bridged NodeLabel \"%s\"", label));

    ABORT_APP_ON_FAILURE(
        cluster::bridged_device_basic_information::attribute::create_product_name(
            basic,
            product,
            static_cast<uint16_t>(std::strlen(product))) != nullptr,
        ESP_LOGE(TAG, "Failed to create bridged ProductName \"%s\"", product));

    ABORT_APP_ON_FAILURE(
        cluster::bridged_device_basic_information::attribute::create_vendor_name(
            basic,
            vendor,
            static_cast<uint16_t>(std::strlen(vendor))) != nullptr,
        ESP_LOGE(TAG, "Failed to create bridged VendorName"));
}

static endpoint_t *app_create_bridged_endpoint(
    node_t *node,
    endpoint_t *aggregator,
    const char *node_label,
    const char *product_name,
    void *priv_data)
{
    bridged_node::config_t bridged_config;
    bridged_config.bridged_device_basic_information.reachable = true;

    endpoint_t *endpoint = bridged_node::create(
        node,
        &bridged_config,
        ENDPOINT_FLAG_BRIDGE,
        priv_data);
    ABORT_APP_ON_FAILURE(
        endpoint != nullptr,
        ESP_LOGE(TAG, "Failed to create bridged endpoint for \"%s\"",
                 node_label));

    app_set_bridged_device_identity(endpoint, node_label, product_name);

    ABORT_APP_ON_FAILURE(
        endpoint::set_parent_endpoint(endpoint, aggregator) == ESP_OK,
        ESP_LOGE(TAG, "Failed to parent bridged endpoint \"%s\" under aggregator",
                 node_label));

    ESP_LOGI(TAG,
             "Bridged endpoint \"%s\" created with endpoint_id %d",
             node_label,
             endpoint::get_id(endpoint));
    return endpoint;
}

static void app_configure_thermostat_limits(endpoint_t *endpoint)
{
    cluster_t *thermostat_cluster = cluster::get(endpoint, Thermostat::Id);
    ABORT_APP_ON_FAILURE(thermostat_cluster != nullptr,
        ESP_LOGE(TAG, "Failed to get Thermostat cluster"));

    /*
     * Matter Thermostat temperatures use units of 0.01°C:
     * 1600 = 16.00°C
     * 3000 = 30.00°C
     */
    cluster::thermostat::attribute::create_abs_min_cool_setpoint_limit(
        thermostat_cluster, 1600);
    cluster::thermostat::attribute::create_abs_max_cool_setpoint_limit(
        thermostat_cluster, 3000);
    cluster::thermostat::attribute::create_min_cool_setpoint_limit(
        thermostat_cluster, 1600);
    cluster::thermostat::attribute::create_max_cool_setpoint_limit(
        thermostat_cluster, 3000);

    cluster::thermostat::attribute::create_abs_min_heat_setpoint_limit(
        thermostat_cluster, 1600);
    cluster::thermostat::attribute::create_abs_max_heat_setpoint_limit(
        thermostat_cluster, 3000);
    cluster::thermostat::attribute::create_min_heat_setpoint_limit(
        thermostat_cluster, 1600);
    cluster::thermostat::attribute::create_max_heat_setpoint_limit(
        thermostat_cluster, 3000);
    cluster::thermostat::attribute::create_min_setpoint_dead_band(
        thermostat_cluster, 100); // 1.00°C
}

static uint32_t app_count_active_subscriptions_locked()
{
    auto *engine =
        chip::app::InteractionModelEngine::GetInstance();

    if (engine == nullptr) {
        return 0;
    }

    return engine->GetNumActiveReadHandlers(
        chip::app::ReadHandler::
            InteractionType::Subscribe
    );
}

class AppSubscriptionCallback
    : public chip::app::ReadHandler::ApplicationCallback
{
public:
    CHIP_ERROR OnSubscriptionRequested(
        chip::app::ReadHandler &handler,
        chip::Transport::SecureSession &session) override
    {
        ESP_LOGI(TAG, "Matter subscription requested");

        return CHIP_NO_ERROR;
    }

    void OnSubscriptionEstablished(
        chip::app::ReadHandler &handler) override
    {
        ESP_LOGI( TAG, "Matter subscription established");
        /*
         * The Matter state can now be checked again.
         * It should transition to SUBSCRIBED.
         */
    	const uint32_t count =
    	    app_count_active_subscriptions_locked();

    	ESP_LOGI(
    	    TAG,
    	    "OnSubscriptionEstablished: active=%lu",
    	    static_cast<unsigned long>(count)
    	);

    	app_driver_set_subscription_active(
    	    count > 0
    	);
		app_driver_update_led_states();
    }

    void OnSubscriptionTerminated(
        chip::app::ReadHandler &handler) override
    {
        ESP_LOGI(TAG, "Matter subscription terminated");
        /*
         * This callback runs just before the subscription is removed.
         * The current handler may still appear in the active list, so recount
         * after the current CHIP operation completes. That keeps IR parsing
         * and LED state aligned with the real subscription count.
         */
        chip::DeviceLayer::SystemLayer().ScheduleLambda(
            []() {
                const uint32_t count =
                    app_count_active_subscriptions_locked();

                ESP_LOGI(
                    TAG,
                    "OnSubscriptionTerminated: active=%lu",
                    static_cast<unsigned long>(count)
                );

                app_driver_set_subscription_active(
                    count > 0
                );
                app_driver_update_led_states();
            }
        );
    }
};

static AppSubscriptionCallback s_subscription_callback;

static void app_event_cb(const ChipDeviceEvent *event, intptr_t arg)
{
    switch (event->Type) {
    case chip::DeviceLayer::DeviceEventType::kInterfaceIpAddressChanged:
        ESP_LOGI(TAG, "Interface IP Address changed");
		app_driver_update_led_states();
        break;

    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEConnectionEstablished:
        ESP_LOGI(TAG, "CHIPoBLE connection established");
        s_chipoble_connected = true;
        /*
         * Subscribe arrives just before PBKDF/PASE. Replace QR with "配对中..."
         * then free LVGL heap; the panel keeps that text frame on-screen.
         */
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            app_suspend_display_for_pase();
        }
        break;

    case chip::DeviceLayer::DeviceEventType::kCHIPoBLEConnectionClosed:
        ESP_LOGI(TAG, "CHIPoBLE connection closed");
        s_chipoble_connected = false;
        app_schedule_work([](intptr_t /*arg*/) {
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0 &&
                !s_commissioning_in_progress) {
                app_start_pairing_display();
            } else {
                /* BLE heap is back; safe to bring up the normal UI. */
                app_try_start_pending_post_commission_ui();
            }
        });
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
		s_commissioning_in_progress = false;
		app_driver_update_led_states();
        /* Restore normal UI (not pairing QR) after BLE releases heap. */
        app_schedule_work([](intptr_t /*arg*/) { app_request_post_commission_ui(); });
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        s_commissioning_in_progress = false;
        app_schedule_work([](intptr_t /*arg*/) {
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
                app_start_pairing_display();
            }
        });
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
		s_commissioning_in_progress = true;
		app_driver_update_led_states();
        /* Backup: free display if BLE-subscribe suspend did not run. */
        app_suspend_display_for_pase();
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
        s_commissioning_in_progress = false;
        /* Failed session → pairing QR; success → resume deferred normal UI. */
        app_schedule_work([](intptr_t /*arg*/) {
            if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
                app_start_pairing_display();
            } else {
                app_try_start_pending_post_commission_ui();
            }
        });
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowOpened:
        ESP_LOGI(TAG, "Commissioning window opened");
		app_driver_update_led_states();
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningWindowClosed:
        ESP_LOGI(TAG, "Commissioning window closed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricRemoved: {
        ESP_LOGI(TAG, "Fabric removed successfully");
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            chip::CommissioningWindowManager  &commissionMgr = chip::Server::GetInstance().GetCommissioningWindowManager();
            constexpr auto kTimeoutSeconds = chip::System::Clock::Seconds16(k_timeout_seconds);
            if (!commissionMgr.IsCommissioningWindowOpen()) {
                /* After removing last fabric, this example does not remove the Wi-Fi credentials
                 * and still has IP connectivity so, only advertising on DNS-SD.
                 */
                CHIP_ERROR err = commissionMgr.OpenBasicCommissioningWindow(kTimeoutSeconds,
                                                                            chip::CommissioningWindowAdvertisement::kDnssdOnly);
                if (err != CHIP_NO_ERROR) {
                    ESP_LOGE(TAG, "Failed to open commissioning window, err:%" CHIP_ERROR_FORMAT, err.Format());
                }
            }
        }
		app_driver_update_led_states();
        break;
    }

    case chip::DeviceLayer::DeviceEventType::kFabricWillBeRemoved:
        ESP_LOGI(TAG, "Fabric will be removed");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricUpdated:
        ESP_LOGI(TAG, "Fabric is updated");
        break;

    case chip::DeviceLayer::DeviceEventType::kFabricCommitted:
        ESP_LOGI(TAG, "Fabric is committed");
        break;
    default:
        break;
    }
}

// This callback is invoked when clients interact with the Identify Cluster.
// In the callback implementation, an endpoint can identify itself. (e.g., by flashing an LED or light).
static esp_err_t app_identification_cb(identification::callback_type_t type, uint16_t endpoint_id, uint8_t effect_id,
                                       uint8_t effect_variant, void *priv_data)
{
    ESP_LOGI(TAG, "Identification callback: type: %u, effect: %u, variant: %u", type, effect_id, effect_variant);
    return ESP_OK;
}

// This callback is called for every attribute update. The callback implementation shall
// handle the desired attributes and return an appropriate error code. If the attribute
// is not of your interest, please do not return an error code and strictly return ESP_OK.
static esp_err_t app_attribute_update_cb(attribute::callback_type_t type, uint16_t endpoint_id, uint32_t cluster_id,
                                         uint32_t attribute_id, esp_matter_attr_val_t *val, void *priv_data)
{
    esp_err_t err = ESP_OK;

    if (type == PRE_UPDATE) {
        /* Driver update */
        app_driver_handle_t driver_handle = (app_driver_handle_t)priv_data;
        err = app_driver_attribute_update(driver_handle, endpoint_id, cluster_id, attribute_id, val);
    }

    return err;
}

app_matter_state_t app_get_matter_state_locked()
{
    /*
     * The commissioning state has the highest priority.
     *
     * Even if the device already has a fabric, return COMMISSIONING
     * while a second fabric is being added.
     */
    if (s_commissioning_in_progress) {
        return app_matter_state_t::COMMISSIONING;
    }

    const size_t fabric_count =
        chip::Server::GetInstance()
            .GetFabricTable()
            .FabricCount();

    /*
     * No fabric means Matter commissioning has not been completed.
     */
    if (fabric_count == 0) {
        return app_matter_state_t::NOT_COMMISSIONED;
    }

    /*
     * A fabric exists, but Wi-Fi is currently disconnected.
     */
    const bool wifi_connected =
        chip::DeviceLayer::ConnectivityMgr()
            .IsWiFiStationConnected();

    if (!wifi_connected) {
        return app_matter_state_t::COMMISSIONED_OFFLINE;
    }

    /*
     * Wi-Fi is connected. Now check whether the device has any active subscriptions.
     */
    auto *interaction_engine =
        chip::app::InteractionModelEngine::GetInstance();

    uint32_t subscription_count = 0;

    if (interaction_engine != nullptr) {
        subscription_count =
            interaction_engine->GetNumActiveReadHandlers(
                chip::app::ReadHandler::
                    InteractionType::Subscribe
            );
    }

    if (subscription_count > 0) {
        return app_matter_state_t::SUBSCRIBED;
    }

    return app_matter_state_t::CONNECTED_NO_SUBSCRIPTION;
}

/*
 * Matter Temperature / Humidity units:
 *   temperature = °C × 100
 *   humidity    = %RH × 100
 */
static void sht30_temperature_notification(uint16_t /*endpoint_id*/, float temp_c,
                                           void * /*user_data*/)
{
    ws2812_temp_light_set_temperature_c(temp_c);

    const int16_t temp_x100 = static_cast<int16_t>(temp_c * 100.0f);
    chip::DeviceLayer::SystemLayer().ScheduleLambda([temp_x100]() {
        attribute_t *attribute = attribute::get(
            room_air_conditioner_endpoint_id,
            Thermostat::Id,
            Thermostat::Attributes::LocalTemperature::Id);
        if (attribute == nullptr) {
            return;
        }

        esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
        attribute::get_val(attribute, &val);
        val.type = ESP_MATTER_VAL_TYPE_NULLABLE_INT16;
        val.val.i16 = temp_x100;
        attribute::update(
            room_air_conditioner_endpoint_id,
            Thermostat::Id,
            Thermostat::Attributes::LocalTemperature::Id,
            &val);
    });
}

static void sht30_humidity_notification(uint16_t endpoint_id, float humidity_pct,
                                        void * /*user_data*/)
{
    if (endpoint_id == 0) {
        return;
    }

    float clamped = humidity_pct;
    if (clamped < 0.0f) {
        clamped = 0.0f;
    } else if (clamped > 100.0f) {
        clamped = 100.0f;
    }
    const uint16_t humidity_x100 = static_cast<uint16_t>(clamped * 100.0f);

    chip::DeviceLayer::SystemLayer().ScheduleLambda([endpoint_id, humidity_x100]() {
        attribute_t *attribute = attribute::get(
            endpoint_id,
            RelativeHumidityMeasurement::Id,
            RelativeHumidityMeasurement::Attributes::MeasuredValue::Id);
        if (attribute == nullptr) {
            return;
        }

        esp_matter_attr_val_t val = esp_matter_invalid(nullptr);
        attribute::get_val(attribute, &val);
        val.type = ESP_MATTER_VAL_TYPE_NULLABLE_UINT16;
        val.val.u16 = humidity_x100;
        attribute::update(
            endpoint_id,
            RelativeHumidityMeasurement::Id,
            RelativeHumidityMeasurement::Attributes::MeasuredValue::Id,
            &val);
    });
}

/*
 * Bring up GC9A01 + LVGL + UI. Screen choice is decided inside ui_init()
 * (pairing QR only when FabricCount == 0; otherwise LEARN/AC).
 */
static void app_start_display_ui(void)
{
    if (s_display_ui_started) {
        return;
    }

    ESP_LOGI(TAG, "Starting display UI (free heap=%u)",
             static_cast<unsigned>(esp_get_free_heap_size()));

    esp_err_t err = board_i2c_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Board I2C init failed: %s", esp_err_to_name(err));
    }

    err = display_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Display init failed: %s", esp_err_to_name(err));
        return;
    }

    err = ui_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "UI init failed: %s", esp_err_to_name(err));
        display_suspend_lvgl();
        return;
    }

    s_display_ui_started = true;
}

/*
 * Show Matter QR / manual code on the round LCD before first commission.
 * SHT30 stays deferred — it is not needed for pairing and costs heap/I2C.
 */
static void app_start_pairing_display(void)
{
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
        ESP_LOGW(TAG, "Skip pairing display: fabric already present");
        return;
    }

    ESP_LOGI(TAG, "Starting pairing display (free heap=%u)",
             static_cast<unsigned>(esp_get_free_heap_size()));
    app_start_display_ui();
}

static void app_suspend_display_for_pase(void)
{
    if (!s_display_ui_started && !display_is_ready()) {
        return;
    }

    ESP_LOGI(TAG, "Switching to pairing-busy frame, then freeing LVGL heap");
    /* Draw "配对中..." first; panel keeps that frame after LVGL is suspended. */
    (void)ui_show_commissioning_busy();
    ui_deinit();
    display_suspend_lvgl();
    s_display_ui_started = false;
}

static void app_start_sht30(void)
{
    if (s_sht30_started) {
        return;
    }
    s_sht30_started = true;

    static sht30_sensor_config_t sht30_config = {
        .temperature = {
            .cb = sht30_temperature_notification,
            .endpoint_id = room_air_conditioner_endpoint_id,
        },
        .humidity = {
            .cb = sht30_humidity_notification,
            .endpoint_id = humidity_sensor_endpoint_id,
        },
        .user_data = nullptr,
        .interval_ms = 5000,
    };
    esp_err_t err = sht30_sensor_init(&sht30_config);
    if (err == ESP_OK) {
        app_driver_set_ambient_sensor_active(true);
        ESP_LOGI(TAG, "SHT30 ambient sensor active");
    } else {
        app_driver_set_ambient_sensor_active(false);
        ESP_LOGW(TAG,
                 "SHT30 not available (%s); LocalTemperature will mirror setpoints",
                 esp_err_to_name(err));
    }
}

/* Full post-commission bring-up: normal LCD/UI (again if suspended) + SHT30. */
static void app_start_ui_peripherals(void)
{
    ESP_LOGI(TAG, "Starting LCD/SHT30 peripherals (free heap=%u)",
             static_cast<unsigned>(esp_get_free_heap_size()));
    app_start_display_ui();
    app_start_sht30();
}

static void app_post_commission_ui_fallback(chip::System::Layer * /*layer*/,
                                            void * /*appState*/)
{
    app_schedule_work([](intptr_t /*arg*/) {
        if (!s_pending_post_commission_ui || s_commissioning_in_progress) {
            return;
        }
        if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
            return;
        }
        /*
         * BLE sometimes stays up after the first fabric (second admin / lingering
         * CHIPoBLE). Don't block the LEARN/AC UI forever — LVGL will pick a
         * smaller draw buffer when heap is tight.
         */
        ESP_LOGW(TAG,
                 "Post-commission UI fallback restore (ble=%d heap=%u)",
                 s_chipoble_connected ? 1 : 0,
                 static_cast<unsigned>(esp_get_free_heap_size()));
        s_pending_post_commission_ui = false;
        app_start_ui_peripherals();
    });
}

static void app_try_start_pending_post_commission_ui(void)
{
    if (!s_pending_post_commission_ui) {
        return;
    }
    if (chip::Server::GetInstance().GetFabricTable().FabricCount() == 0) {
        return;
    }
    if (s_chipoble_connected || s_commissioning_in_progress) {
        ESP_LOGI(TAG,
                 "Post-commission UI still deferred (ble=%d commissioning=%d heap=%u)",
                 s_chipoble_connected ? 1 : 0,
                 s_commissioning_in_progress ? 1 : 0,
                 static_cast<unsigned>(esp_get_free_heap_size()));
        return;
    }

    s_pending_post_commission_ui = false;
    (void)chip::DeviceLayer::SystemLayer().CancelTimer(app_post_commission_ui_fallback,
                                                       nullptr);
    ESP_LOGI(TAG, "Restoring UI after commissioning (free heap=%u)",
             static_cast<unsigned>(esp_get_free_heap_size()));
    app_start_ui_peripherals();
}

static void app_request_post_commission_ui(void)
{
    s_pending_post_commission_ui = true;
    if (s_chipoble_connected || s_commissioning_in_progress) {
        ESP_LOGI(TAG,
                 "Deferring LCD restore until CHIPoBLE closes (free heap=%u)",
                 static_cast<unsigned>(esp_get_free_heap_size()));
        (void)chip::DeviceLayer::SystemLayer().CancelTimer(
            app_post_commission_ui_fallback, nullptr);
        (void)chip::DeviceLayer::SystemLayer().StartTimer(
            chip::System::Clock::Seconds16(20), app_post_commission_ui_fallback,
            nullptr);
        return;
    }
    (void)chip::DeviceLayer::SystemLayer().CancelTimer(app_post_commission_ui_fallback,
                                                       nullptr);
    app_try_start_pending_post_commission_ui();
}

static void app_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    (void)arg;
    if (event_base != WIFI_EVENT) {
        return;
    }

    if (event_id == WIFI_EVENT_STA_DISCONNECTED) {
        const auto *ev =
            static_cast<const wifi_event_sta_disconnected_t *>(event_data);
        ESP_LOGW(TAG,
                 "WIFI_EVENT_STA_DISCONNECTED reason=%u ssid=%.*s rssi=%d",
                 static_cast<unsigned>(ev->reason),
                 ev->ssid_len, reinterpret_cast<const char *>(ev->ssid),
                 static_cast<int>(ev->rssi));
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE || CONFIG_SW_COEXIST_ENABLE
        /* Prefer Wi-Fi while Matter still holds CHIPoBLE open for ConnectNetwork. */
        esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
#endif
    } else if (event_id == WIFI_EVENT_STA_START) {
#if CONFIG_ESP_COEX_SW_COEXIST_ENABLE || CONFIG_SW_COEXIST_ENABLE
        esp_coex_preference_set(ESP_COEX_PREFER_WIFI);
#endif
    } else if (event_id == WIFI_EVENT_STA_CONNECTED) {
        const auto *ev =
            static_cast<const wifi_event_sta_connected_t *>(event_data);
        ESP_LOGI(TAG, "WIFI_EVENT_STA_CONNECTED ssid=%.*s channel=%u auth=%u",
                 ev->ssid_len, reinterpret_cast<const char *>(ev->ssid),
                 static_cast<unsigned>(ev->channel),
                 static_cast<unsigned>(ev->authmode));
    }
}

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    nvs_flash_init();

    /* Persist SerialNumber before Matter reads Basic Information */
    app_ensure_serial_number();

    /* Initialize driver */
    app_driver_handle_t room_air_conditioner_handle = app_driver_room_air_conditioner_init();
    /* Button callbacks are registered inside init; handle is not needed here. */
    (void)app_driver_button_init();

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

    app_set_default_node_label(node);

    /*
     * Home display mode (NVS, default COMBINED):
     *   COMBINED — flat siblings on one node (one Home accessory)
     *   SEPARATE — Aggregator + Bridged Nodes (independent Home tiles)
     *
     * No Matter Fan / FanControl endpoint; IR fan is always Auto.
     * Change mode on the device Settings page (reboot + re-pair required).
     */
    const bool separate_home_tiles =
        app_settings_home_display_is_separate();
    ESP_LOGI(TAG, "Home display mode: %s",
             separate_home_tiles ? "separate (bridged)" : "combined (flat)");

    endpoint_t *aggregator = nullptr;
    if (separate_home_tiles) {
        aggregator::config_t aggregator_config;
        aggregator = endpoint::aggregator::create(
            node,
            &aggregator_config,
            ENDPOINT_FLAG_NONE,
            nullptr);
        ABORT_APP_ON_FAILURE(
            aggregator != nullptr,
            ESP_LOGE(TAG, "Failed to create aggregator endpoint"));
        ESP_LOGI(TAG, "Aggregator created with endpoint_id %d",
                 endpoint::get_id(aggregator));
    }

    room_air_conditioner::config_t room_air_conditioner_config;
    room_air_conditioner_config.on_off.on_off = DEFAULT_POWER;
	auto &thermostat = room_air_conditioner_config.thermostat;
	/*
	 * The Thermostat cluster must declare the supported features.
	 * This air conditioner supports both cooling and heating,
	 * so enable the cooling and heating features.
	 */
	thermostat.feature_flags =
	    cluster::thermostat::feature::cooling::get_id() |
	    cluster::thermostat::feature::heating::get_id() ;
	
	
	/*
	 * 0x04 = Cooling and Heating
	 * system_mode: 0=Off, 1=Auto, 3=Cool, 4=Heat, 7=FanOnly, 8=Dry
	 */
	thermostat.control_sequence_of_operation = 0x04;
	thermostat.system_mode =
	    static_cast<uint8_t>(
	        Thermostat::SystemModeEnum::kOff
	    );

	static constexpr int16_t DEFAULT_TARGET_TEMP_X100 = 2500;
	room_air_conditioner_config.thermostat.local_temperature =
	    DEFAULT_TARGET_TEMP_X100;
	 room_air_conditioner_config
	    .thermostat
	    .features
	    .cooling
	    .occupied_cooling_setpoint =
	        DEFAULT_TARGET_TEMP_X100;
	
	room_air_conditioner_config
	    .thermostat
	    .features
	    .heating
	    .occupied_heating_setpoint =
	        DEFAULT_TARGET_TEMP_X100;
	
	room_air_conditioner_config
	    .thermostat
	    .local_temperature =
	        DEFAULT_TARGET_TEMP_X100;

	endpoint_t *endpoint = nullptr;
	if (separate_home_tiles) {
	    endpoint = app_create_bridged_endpoint(
	        node,
	        aggregator,
	        "Air Conditioner",
	        "Air Conditioner",
	        room_air_conditioner_handle);
	    ABORT_APP_ON_FAILURE(
	        room_air_conditioner::add(
	            endpoint,
	            &room_air_conditioner_config) == ESP_OK,
	        ESP_LOGE(TAG, "Failed to add Room Air Conditioner device type"));
	} else {
	    endpoint = room_air_conditioner::create(
	        node,
	        &room_air_conditioner_config,
	        ENDPOINT_FLAG_NONE,
	        room_air_conditioner_handle);
	    ABORT_APP_ON_FAILURE(
	        endpoint != nullptr,
	        ESP_LOGE(TAG, "Failed to create room air conditioner endpoint"));
	}

    room_air_conditioner_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Room Air Conditioner endpoint_id %d (%s)",
             room_air_conditioner_endpoint_id,
             separate_home_tiles ? "bridged" : "flat");
	app_configure_thermostat_limits(endpoint);

	/*
	 * Optional SHT30 ambient humidity endpoint.
	 * Temperature is reported via Thermostat LocalTemperature on the RAC
	 * endpoint so Apple Home / Google Home show room temp on the AC tile.
	 */
	humidity_sensor::config_t humidity_sensor_config;
	/* MeasuredValue stays null until the first successful SHT30 sample. */
	humidity_sensor_config.relative_humidity_measurement.min_measured_value =
	    nullable<uint16_t>(0);
	humidity_sensor_config.relative_humidity_measurement.max_measured_value =
	    nullable<uint16_t>(10000);

	endpoint_t *humidity_endpoint = nullptr;
	if (separate_home_tiles) {
	    humidity_endpoint = app_create_bridged_endpoint(
	        node,
	        aggregator,
	        "Humidity",
	        "Humidity Sensor",
	        nullptr);
	    ABORT_APP_ON_FAILURE(
	        humidity_sensor::add(
	            humidity_endpoint,
	            &humidity_sensor_config) == ESP_OK,
	        ESP_LOGE(TAG, "Failed to add Humidity Sensor device type"));
	} else {
	    humidity_endpoint = humidity_sensor::create(
	        node,
	        &humidity_sensor_config,
	        ENDPOINT_FLAG_NONE,
	        nullptr);
	    ABORT_APP_ON_FAILURE(
	        humidity_endpoint != nullptr,
	        ESP_LOGE(TAG, "Failed to create humidity sensor endpoint"));
	}

	humidity_sensor_endpoint_id = endpoint::get_id(humidity_endpoint);
	ESP_LOGI(TAG, "Humidity sensor endpoint_id %d (%s)",
	         humidity_sensor_endpoint_id,
	         separate_home_tiles ? "bridged" : "flat");

	/*
	 * WS2812 ambient light as Matter Dimmable Light:
	 * On/Off + LevelControl only. Rainbow / breath / solid modes are
	 * selected on the local screen (option A).
	 */
	dimmable_light::config_t temp_light_config;
	temp_light_config.on_off.on_off = true;
	temp_light_config.level_control.current_level = 180;
	endpoint_t *temp_light_endpoint = nullptr;
	if (separate_home_tiles) {
	    temp_light_endpoint = app_create_bridged_endpoint(
	        node,
	        aggregator,
	        "Ambient Light",
	        "Ambient Light",
	        nullptr);
	    ABORT_APP_ON_FAILURE(
	        dimmable_light::add(
	            temp_light_endpoint,
	            &temp_light_config) == ESP_OK,
	        ESP_LOGE(TAG, "Failed to add Dimmable Light device type"));
	} else {
	    temp_light_endpoint = dimmable_light::create(
	        node,
	        &temp_light_config,
	        ENDPOINT_FLAG_NONE,
	        nullptr);
	    ABORT_APP_ON_FAILURE(
	        temp_light_endpoint != nullptr,
	        ESP_LOGE(TAG, "Failed to create dimmable light endpoint"));
	}

	temp_light_endpoint_id = endpoint::get_id(temp_light_endpoint);
	ESP_LOGI(TAG, "Ambient dimmable light endpoint_id %d (%s)",
	         temp_light_endpoint_id,
	         separate_home_tiles ? "bridged" : "flat");

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
    /* Set OpenThread platform config */
    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };
    set_openthread_platform_config(&config);
#endif


    /* Matter start */
    err = esp_matter::start(app_event_cb);
    ABORT_APP_ON_FAILURE(err == ESP_OK, ESP_LOGE(TAG, "Failed to start Matter, err:%d", err));

    /*
     * Register after Matter start: the default esp_event loop is created by
     * the platform/Wi-Fi stack during start. Registering earlier returns
     * ESP_ERR_INVALID_STATE and aborts into a boot loop.
     *
     * Log Wi-Fi disconnect reasons during Matter ConnectNetwork. BLE+WiFi
     * coexistence often fails here; reason codes distinguish auth/password
     * problems from RF coexist timeouts.
     */
    {
        esp_err_t wifi_evt_err = esp_event_handler_register(
            WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, nullptr);
        if (wifi_evt_err != ESP_OK) {
            ESP_LOGW(TAG, "Wi-Fi event handler register failed: %s",
                     esp_err_to_name(wifi_evt_err));
        }
    }

	auto *interaction_engine =
	    chip::app::InteractionModelEngine::GetInstance();
	
	if (interaction_engine != nullptr) {
	    interaction_engine->RegisterReadHandlerAppCallback(
	        &s_subscription_callback
	    );
	
	    ESP_LOGI(
	        TAG,
	        "Matter subscription callback registered"
	    );
	} else {
	    ESP_LOGE(
	        TAG,
	        "InteractionModelEngine is unavailable"
	    );
	}
    /* Starting driver with default values */
    app_driver_room_air_conditioner_set_defaults(room_air_conditioner_endpoint_id);

	/*
	 * WS2812 temperature indicator (independent of SHT30 presence so On/Off
	 * still works; color follows sensor readings when available).
	 */
	err = ws2812_temp_light_init();
	if (err == ESP_OK) {
	    ws2812_temp_light_set_enabled(temp_light_config.on_off.on_off);
	    ws2812_temp_light_set_brightness(180);
	    ws2812_temp_light_set_mode(WS2812_MODE_TEMP_BREATH);
	} else {
	    ESP_LOGW(TAG, "WS2812 indicator not available (%s)", esp_err_to_name(err));
	}

	/*
	 * Uncommissioned: show pairing QR on LCD now. LVGL is suspended when the
	 * commissioning session starts so BLE PASE still has enough heap.
	 * SHT30 waits until a fabric exists.
	 */
	if (chip::Server::GetInstance().GetFabricTable().FabricCount() > 0) {
	    app_start_ui_peripherals();
	} else {
	    app_start_pairing_display();
	}

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#endif
}
