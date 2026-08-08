#include "ws2812_temp_light.h"

#include "board_pins.h"

#include <atomic>
#include <cmath>
#include <math.h>

#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <led_strip.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const char *TAG = "ws2812_temp";

static constexpr float kTempGreenC = 18.0f;
static constexpr float kTempOrangeC = 30.0f;
static constexpr float kBreathPeriodSec = 2.5f;
static constexpr uint32_t kTickMs = 30;
static constexpr float kBreathMin = 0.12f;
static constexpr float kBreathMax = 0.85f;

static led_strip_handle_t s_strip = nullptr;
static TaskHandle_t s_task = nullptr;
static std::atomic<bool> s_ready{false};
static std::atomic<bool> s_enabled{true};
static std::atomic<bool> s_learn{false};
static std::atomic<float> s_temp_c{22.0f};
static std::atomic<uint8_t> s_level{180}; /* Matter 1..254 */
static std::atomic<ws2812_light_mode_t> s_mode{WS2812_MODE_TEMP_BREATH};
static std::atomic<float> s_hue{0.0f};

static void temp_to_rgb(float temp_c, uint8_t *r, uint8_t *g, uint8_t *b)
{
    float t = temp_c;
    if (t < kTempGreenC) {
        t = kTempGreenC;
    } else if (t > kTempOrangeC) {
        t = kTempOrangeC;
    }

    const float u = (t - kTempGreenC) / (kTempOrangeC - kTempGreenC);
    float rf;
    float gf;
    float bf = 0.0f;

    if (u <= 0.5f) {
        const float v = u / 0.5f;
        rf = 255.0f * v;
        gf = 255.0f;
    } else {
        const float v = (u - 0.5f) / 0.5f;
        rf = 255.0f;
        gf = 255.0f + (80.0f - 255.0f) * v;
    }

    *r = static_cast<uint8_t>(rf + 0.5f);
    *g = static_cast<uint8_t>(gf + 0.5f);
    *b = static_cast<uint8_t>(bf + 0.5f);
}

static void hsv_to_rgb(float h, float s, float v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    while (h < 0.0f) {
        h += 360.0f;
    }
    while (h >= 360.0f) {
        h -= 360.0f;
    }
    const float c = v * s;
    const float x = c * (1.0f - fabsf(fmodf(h / 60.0f, 2.0f) - 1.0f));
    const float m = v - c;
    float rf = 0, gf = 0, bf = 0;
    if (h < 60) {
        rf = c;
        gf = x;
    } else if (h < 120) {
        rf = x;
        gf = c;
    } else if (h < 180) {
        gf = c;
        bf = x;
    } else if (h < 240) {
        gf = x;
        bf = c;
    } else if (h < 300) {
        rf = x;
        bf = c;
    } else {
        rf = c;
        bf = x;
    }
    *r = static_cast<uint8_t>((rf + m) * 255.0f + 0.5f);
    *g = static_cast<uint8_t>((gf + m) * 255.0f + 0.5f);
    *b = static_cast<uint8_t>((bf + m) * 255.0f + 0.5f);
}

static void apply_level(uint8_t r, uint8_t g, uint8_t b, float scale,
                        uint8_t *out_r, uint8_t *out_g, uint8_t *out_b)
{
    if (scale < 0.0f) {
        scale = 0.0f;
    } else if (scale > 1.0f) {
        scale = 1.0f;
    }
    *out_r = static_cast<uint8_t>(r * scale + 0.5f);
    *out_g = static_cast<uint8_t>(g * scale + 0.5f);
    *out_b = static_cast<uint8_t>(b * scale + 0.5f);
}

static float breath_wave(int64_t now_us)
{
    const float phase =
        static_cast<float>(fmod(static_cast<double>(now_us) / 1000000.0,
                                static_cast<double>(kBreathPeriodSec))) /
        kBreathPeriodSec;
    const float wave = 0.5f + 0.5f * sinf(2.0f * static_cast<float>(M_PI) * phase);
    return kBreathMin + (kBreathMax - kBreathMin) * wave;
}

static float level_scale(void)
{
    const uint8_t level = s_level.load();
    return static_cast<float>(level) / 254.0f;
}

static void render_frame(void)
{
    if (!s_strip) {
        return;
    }

    if (!s_enabled.load() || s_mode.load() == WS2812_MODE_NIGHT_OFF) {
        if (!s_learn.load()) {
            led_strip_clear(s_strip);
            return;
        }
    }

    uint8_t r = 0, g = 0, b = 0;
    float scale = level_scale();
    const int64_t now = esp_timer_get_time();

    if (s_learn.load()) {
        /* Yellow breath while learning IR. */
        r = 255;
        g = 200;
        b = 0;
        scale = breath_wave(now);
    } else {
        switch (s_mode.load()) {
        case WS2812_MODE_NIGHT_OFF:
            led_strip_clear(s_strip);
            return;
        case WS2812_MODE_MANUAL:
            r = g = b = 255;
            break;
        case WS2812_MODE_TEMP_BREATH:
            temp_to_rgb(s_temp_c.load(), &r, &g, &b);
            scale *= breath_wave(now);
            break;
        case WS2812_MODE_SOLID:
            temp_to_rgb(s_temp_c.load(), &r, &g, &b);
            break;
        case WS2812_MODE_RAINBOW: {
            float hue = s_hue.load() + 2.0f;
            if (hue >= 360.0f) {
                hue -= 360.0f;
            }
            s_hue.store(hue);
            hsv_to_rgb(hue, 1.0f, 1.0f, &r, &g, &b);
            break;
        }
        case WS2812_MODE_WHITE_BREATH:
            r = g = b = 255;
            scale *= breath_wave(now);
            break;
        }
    }

    uint8_t out_r = 0, out_g = 0, out_b = 0;
    apply_level(r, g, b, scale, &out_r, &out_g, &out_b);
    led_strip_set_pixel(s_strip, 0, out_r, out_g, out_b);
    led_strip_refresh(s_strip);
}

static void light_task(void * /*arg*/)
{
    while (true) {
        render_frame();
        vTaskDelay(pdMS_TO_TICKS(kTickMs));
    }
}

esp_err_t ws2812_temp_light_init(void)
{
    if (s_ready.load()) {
        return ESP_OK;
    }

    led_strip_config_t strip_config = {};
    strip_config.strip_gpio_num = BOARD_WS2812_GPIO;
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
                 static_cast<int>(BOARD_WS2812_GPIO), esp_err_to_name(err));
        s_strip = nullptr;
        return err;
    }

    led_strip_clear(s_strip);

    if (xTaskCreate(light_task, "ws2812_light", 3072, nullptr, 4, &s_task) !=
        pdPASS) {
        led_strip_del(s_strip);
        s_strip = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_ready.store(true);
    ESP_LOGI(TAG, "WS2812 ready on GPIO%d (default mode: temp breath)",
             static_cast<int>(BOARD_WS2812_GPIO));
    return ESP_OK;
}

void ws2812_temp_light_set_enabled(bool enabled)
{
    s_enabled.store(enabled);
    ESP_LOGI(TAG, "Light power %s", enabled ? "ON" : "OFF");
}

bool ws2812_temp_light_is_enabled(void)
{
    return s_enabled.load();
}

void ws2812_temp_light_set_brightness(uint8_t level_1_254)
{
    if (level_1_254 < 1) {
        level_1_254 = 1;
    }
    s_level.store(level_1_254);
}

uint8_t ws2812_temp_light_get_brightness(void)
{
    return s_level.load();
}

void ws2812_temp_light_set_mode(ws2812_light_mode_t mode)
{
    s_mode.store(mode);
    if (mode == WS2812_MODE_NIGHT_OFF) {
        s_enabled.store(false);
    } else if (!s_enabled.load()) {
        s_enabled.store(true);
    }
    ESP_LOGI(TAG, "Light mode=%d", static_cast<int>(mode));
}

ws2812_light_mode_t ws2812_temp_light_get_mode(void)
{
    return s_mode.load();
}

void ws2812_temp_light_set_learn_active(bool active)
{
    s_learn.store(active);
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
)
