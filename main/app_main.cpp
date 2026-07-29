/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include <esp_err.h>
#include <esp_log.h>
#include <nvs_flash.h>

#include <esp_matter.h>
#include <esp_matter_console.h>
#include <esp_matter_ota.h>

#include <common_macros.h>
#include <app_priv.h>
//#include <app_reset.h>
#include <atomic>

#include <platform/CHIPDeviceLayer.h>
#include <app/InteractionModelEngine.h>
#include <app/ReadHandler.h>

#if CHIP_DEVICE_CONFIG_ENABLE_THREAD
#include <platform/ESP32/OpenthreadLauncher.h>
#endif

#include <app/server/CommissioningWindowManager.h>
#include <app/server/Server.h>

static const char *TAG = "app_main";
uint16_t room_air_conditioner_endpoint_id = 0;
uint16_t fan_endpoint_id = 0;

using namespace esp_matter;
using namespace esp_matter::attribute;
using namespace esp_matter::endpoint;
using namespace chip::app::Clusters;

constexpr auto k_timeout_seconds = 300;
static bool s_commissioning_in_progress = false;

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
        ESP_LOGI( TAG, "Matter subscription terminated");
        /*
         * This callback is invoked just before the subscription is removed.
         * The current handler may still be present in the active handler list,
         * so recount the subscriptions after the current CHIP operation completes.
         */
        //chip::DeviceLayer::SystemLayer().ScheduleLambda(
        //    []() {
        //        app_update_matter_state_locked();
        //    }
        //);
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

    case chip::DeviceLayer::DeviceEventType::kCommissioningComplete:
        ESP_LOGI(TAG, "Commissioning complete");
		s_commissioning_in_progress = false;
		app_driver_update_led_states();
        break;

    case chip::DeviceLayer::DeviceEventType::kFailSafeTimerExpired:
        ESP_LOGI(TAG, "Commissioning failed, fail safe timer expired");
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStarted:
        ESP_LOGI(TAG, "Commissioning session started");
		s_commissioning_in_progress = true;
		app_driver_update_led_states();
        break;

    case chip::DeviceLayer::DeviceEventType::kCommissioningSessionStopped:
        ESP_LOGI(TAG, "Commissioning session stopped");
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

extern "C" void app_main()
{
    esp_err_t err = ESP_OK;

    /* Initialize the ESP NVS layer */
    nvs_flash_init();

    /* Initialize driver */
    app_driver_handle_t room_air_conditioner_handle = app_driver_room_air_conditioner_init();
    app_driver_handle_t button_handle = app_driver_button_init();
    //app_reset_button_register(button_handle);

    /* Create a Matter node and add the mandatory Root Node device type on endpoint 0 */
    node::config_t node_config;
    node_t *node = node::create(&node_config, app_attribute_update_cb, app_identification_cb);
    ABORT_APP_ON_FAILURE(node != nullptr, ESP_LOGE(TAG, "Failed to create Matter node"));

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

	endpoint_t *endpoint = room_air_conditioner::create(node, &room_air_conditioner_config, ENDPOINT_FLAG_NONE, room_air_conditioner_handle);
    ABORT_APP_ON_FAILURE(endpoint != nullptr, ESP_LOGE(TAG, "Failed to create room air conditioner endpoint"));

    room_air_conditioner_endpoint_id = endpoint::get_id(endpoint);
    ESP_LOGI(TAG, "Room Air Conditioner created with endpoint_id %d", room_air_conditioner_endpoint_id);
	
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

	/*
	 * Create a separate Fan endpoint.
	 *
	 * This is only for testing whether Apple / Google will show fan speed
	 * when FanControl is exposed as a standard Fan device endpoint instead
	 * of being attached to the Room Air Conditioner endpoint.
	 */
	fan::config_t fan_config = {};
	
	/*
	 * Matter FanMode:
	 * 0 = Off
	 * 1 = Low
	 * 2 = Medium
	 * 3 = High
	 * 4 = On
	 * 5 = Auto
	 * 6 = Smart
	 */
	fan_config.fan_control.fan_mode =
	    static_cast<uint8_t>(
	        FanControl::FanModeEnum::kOff
	    );
	
	fan_config.fan_control.fan_mode_sequence =
	    static_cast<uint8_t>(
	        FanControl::FanModeSequenceEnum::kOffLowMedHighAuto
	    );
	
	fan_config.fan_control.percent_setting =
	    nullable<uint8_t>(0);
	
	fan_config.fan_control.percent_current = 0;
	/*
	 * FanModeSequence:
	 * The target sequence is Off / Low / Medium / High / Auto.
	 * Use the enum values defined in the local fan_control.h file.
	 */
	//fan_config.fan_control.fan_mode_sequence = FanControl::FanModeSequenceEnum::kOffLowMedHighAuto;
	
	/*
	 * Keep PercentSetting and PercentCurrent so Apple Home and
	 * Google Home are more likely to expose fan-speed controls.
	 */
	//fan_config.fan_control.percent_setting = nullable<uint8_t>(50);
	//fan_config.fan_control.percent_current = 50;
	
	endpoint_t *fan_endpoint = fan::create(
	    node,
	    &fan_config,
	    ENDPOINT_FLAG_NONE,
	    room_air_conditioner_handle
	);
	
	ABORT_APP_ON_FAILURE(fan_endpoint != nullptr,
	    ESP_LOGE(TAG, "Failed to create fan endpoint"));
	
	fan_endpoint_id = endpoint::get_id(fan_endpoint);
	ESP_LOGI(TAG, "Fan endpoint created with endpoint_id %d", fan_endpoint_id);

	cluster_t *fan_cluster =
	    cluster::get(fan_endpoint, FanControl::Id);
	
	ABORT_APP_ON_FAILURE(
	    fan_cluster != nullptr,
	    ESP_LOGE(TAG, "Failed to get Fan Control cluster")
	);

	cluster::fan_control::feature::fan_auto::add(fan_cluster);



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

#if CONFIG_ENABLE_CHIP_SHELL
    esp_matter::console::diagnostics_register_commands();
    esp_matter::console::wifi_register_commands();
    esp_matter::console::factoryreset_register_commands();
    esp_matter::console::init();
#endif
}
