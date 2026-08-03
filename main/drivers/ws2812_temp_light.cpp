/*
   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/

#include "ws2812_temp_light.h"

#include <atomic>
#include <cmath>
#include <math.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <led_strip.h>

static const char *TAG = "ws2812_temp";

#if defined(CONFIG_WS2812_GPIO)
#define WS2812_GPIO_NUM CONFIG_WS2812_GPIO
#else
#define WS2812_GPIO_NUM 7
#endif

static constexpr float kTempGreenC = 18.0f;
static constexpr float kTempOrangeC = 30.0f;
static constexpr float kBreathPeriodSec = 2.5f;
static constexpr uint32_t kBreathTickMs = 30;
static constexpr float kBreathMinBrightness = 0.12f;
static constexpr float kBreathMaxBrightness = 0.85f;

static led_strip_handle_t s_strip = nullptr;
static TaskHandle_t s_task = nullptr;
static std::atomic<bool> s_ready{false};
static std::atomic<bool> s_enabled{true};
static std::atomic<float> s_temp_c{22.0f};

static void temp_to_rgb(float temp_c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float t = temp_c;
    if (t < kTempGreenC) {
        t = kTempGreenC;
    } else if (t > kTempOrangeC) {
        t = kTempOrangeC;
    }

    /*
     * Human comfort scale: green (cool) → yellow → orange (warm).
     * Two-segment lerp keeps mid-range readable as warm amber.
     */
    const float u = (t - kTempGreenC) / (kTempOrangeC - kTempGreenC);
    float rf;
    float gf;
    float bf = 0.0f;

    if (u <= 0.5f) {
        const float v = u / 0.5f; // 0..1 green → yellow
        rf = 255.0f * v;
        gf = 255.0f;
    } else {
        const float v = (u - 0.5f) / 0.5f; // 0..1 yellow → orange
        rf = 255.0f;
        gf = 255.0f + (80.0f - 255.0f) * v;
    }

    *r = static_cast<uint8_t>(rf + 0.5f);
    *g = static_cast<uint8_t>(gf + 0.5f);
    *b = static_cast<uint8_t>(bf + 0.5f);
}

static void apply_brightness(uint8_t r, uint8_t g, uint8_t b, float brightness,
                             uint8_t *out_r, uint8_t *out_g, uint8_t *out_b)
{
    if (brightness < 0.0f) {
        brightness = 0.0f;
    } else if (brightness > 1.0f) {
        brightness = 1.0f;
    }
    *out_r = static_cast<uint8_t>(r * brightness + 0.5f);
    *out_g = static_cast<uint8_t>(g * brightness + 0.5f);
    *out_b = static_cast<uint8_t>(b * brightness + 0.5f);
}

static float breath_brightness(int64_t now_us)
{
    const float phase =
        static_cast<float>(fmod(static_cast<double>(now_us) / 1000000.0,
                                static_cast<double>(kBreathPeriodSec))) /
        kBreathPeriodSec;
    const float wave = 0.5f + 0.5f * sinf(2.0f * static_cast<float>(M_PI) * phase);
    return kBreathMinBrightness +
           (kBreathMaxBrightness - kBreathMinBrightness) * wave;
}

static void breath_task(void * /*arg*/)
{
    bool was_enabled = false;

    while (true) {
        const bool enabled = s_enabled.load();
        if (!enabled) {
            if (was_enabled && s_strip) {
                led_strip_clear(s_strip);
            }
            was_enabled = false;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        was_enabled = true;

        uint8_t base_r = 0;
        uint8_t base_g = 0;
        uint8_t base_b = 0;
        temp_to_rgb(s_temp_c.load(), &base_r, &base_g, &base_b);

        const float brightness = breath_brightness(esp_timer_get_time());
        uint8_t r = 0;
        uint8_t g = 0;
        uint8_t b = 0;
        apply_brightness(base_r, base_g, base_b, brightness, &r, &g, &b);

        if (s_strip) {
            led_strip_set_pixel(s_strip, 0, r, g, b);
            led_strip_refresh(s_strip);
        }

        vTaskDelay(pdMS_TO_TICKS(kBreathTickMs));
    }
}

esp_err_t ws2812_temp_light_init(void)
{
    if (s_ready.load()) {
        return ESP_OK;
    }

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = WS2812_GPIO_NUM;
    strip_config.max_leds = 1;
    strip_config.led_model = LED_MODEL_WS2812;
    strip_config.color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB;
    strip_config.flags.invert_out = false;

    led_strip_rmt_config_t rmt_config = {};
    rmt_config.clk_src = RMT_CLK_SRC_DEFAULT;
    rmt_config.resolution_hz = 10 * 1000 * 1000;
    rmt_config.mem_block_symbols = 0;
    rmt_config.flags.with_dma = false;

    esp_err_t err = led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "led_strip_new_rmt_device failed on GPIO%d: %s",
                 WS2812_GPIO_NUM, esp_err_to_name(err));
        s_strip = nullptr;
        return err;
    }

    led_strip_clear(s_strip);

    BaseType_t created =
        xTaskCreate(breath_task, "ws2812_breath", 3072, nullptr, 4, &s_task);
    if (created != pdPASS) {
        ESP_LOGE(TAG, "Failed to create breath task");
        led_strip_del(s_strip);
        s_strip = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_ready.store(true);
    ESP_LOGI(TAG,
             "WS2812 temperature indicator ready on GPIO%d "
             "(green@%.0f°C → orange@%.0f°C, breath %.1fs)",
             WS2812_GPIO_NUM, kTempGreenC, kTempOrangeC, kBreathPeriodSec);
    return ESP_OK;
}

void ws2812_temp_light_set_enabled(bool enabled)
{
    s_enabled.store(enabled);
    if (!enabled && s_strip) {
        led_strip_clear(s_strip);
    }
    ESP_LOGI(TAG, "Temperature indicator %s", enabled ? "ON" : "OFF");
}

bool ws2812_temp_light_is_enabled(void)
{
    return s_enabled.load();
}

void ws2812_temp_light_set_temperature_c(float temp_c)
{
    if (!isfinite(temp_c)) {
        return;
    }
    s_temp_c.store(temp_c);
}

bool ws2812_temp_light_is_ready(void)
{
    return s_ready.load();
}
