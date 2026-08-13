#include "display_gc9a01.h"

#include "board_pins.h"
#include "it7259.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_lcd_gc9a01.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lvgl_port.h>
#include <esp_log.h>

#include <atomic>

static const char *TAG = "display";

static constexpr uint64_t k_idle_timeout_us = 60ULL * 1000ULL * 1000ULL;

static esp_lcd_panel_io_handle_t s_io = nullptr;
static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_disp_t *s_disp = nullptr;
static bool s_hw_ready = false;
static bool s_ready = false;
static bool s_touch_indev_added = false;
static bool s_backlight_inited = false;
static std::atomic<bool> s_backlight_on{false};
static std::atomic<bool> s_idle_hold{false};
static esp_timer_handle_t s_idle_timer = nullptr;

static void display_idle_timer_cb(void *arg)
{
    (void)arg;
    if (s_idle_hold.load(std::memory_order_relaxed)) {
        return;
    }
    if (!s_backlight_on.load(std::memory_order_relaxed)) {
        return;
    }
    ESP_LOGI(TAG, "Idle timeout: turning backlight off");
    display_set_backlight(false);
}

static void display_idle_timer_ensure(void)
{
    if (s_idle_timer != nullptr) {
        return;
    }
    const esp_timer_create_args_t args = {
        .callback = &display_idle_timer_cb,
        .arg = nullptr,
        .dispatch_method = ESP_TIMER_TASK,
        .name = "lcd_idle",
        .skip_unhandled_events = true,
    };
    if (esp_timer_create(&args, &s_idle_timer) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to create LCD idle timer");
        s_idle_timer = nullptr;
    }
}

static void display_idle_timer_restart(void)
{
    display_idle_timer_ensure();
    if (s_idle_timer == nullptr) {
        return;
    }
    (void)esp_timer_stop(s_idle_timer);
    if (s_idle_hold.load(std::memory_order_relaxed)) {
        return;
    }
    if (esp_timer_start_once(s_idle_timer, k_idle_timeout_us) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to start LCD idle timer");
    }
}

void display_backlight_early_off(void)
{
    if (s_backlight_inited) {
        display_set_backlight(false);
        return;
    }

    /*
     * Hold BL dark before LEDC claims the pin. GC9A01 GRAM is random until the
     * first flush; a floating BL MOSFET gate can otherwise show snow at power-on.
     */
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << BOARD_LCD_BL_GPIO;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_up_en = GPIO_PULLUP_DISABLE;
    io.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&io) == ESP_OK) {
        gpio_set_level(BOARD_LCD_BL_GPIO, 0);
    }
}

static void backlight_init(void)
{
    if (s_backlight_inited) {
        return;
    }

    display_backlight_early_off();

    ledc_timer_config_t timer = {};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_8_BIT;
    timer.timer_num = LEDC_TIMER_0;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ledc_timer_config(&timer);

    ledc_channel_config_t channel = {};
    channel.gpio_num = BOARD_LCD_BL_GPIO;
    channel.speed_mode = LEDC_LOW_SPEED_MODE;
    channel.channel = LEDC_CHANNEL_0;
    channel.intr_type = LEDC_INTR_DISABLE;
    channel.timer_sel = LEDC_TIMER_0;
    channel.duty = 0;
    channel.hpoint = 0;
    ledc_channel_config(&channel);
    s_backlight_inited = true;
    s_backlight_on.store(false, std::memory_order_relaxed);
    display_idle_timer_ensure();
}

/*
 * Fill GRAM with black before DISPON / backlight so residual random pixels are
 * never visible when the backlight comes up.
 */
static esp_err_t display_clear_gram_black(void)
{
    if (s_panel == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }

    constexpr int k_lines = 20;
    const size_t bytes =
        static_cast<size_t>(BOARD_LCD_H_RES) * k_lines * sizeof(uint16_t);
    auto *buf = static_cast<uint16_t *>(
        heap_caps_calloc(1, bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL));
    if (buf == nullptr) {
        buf = static_cast<uint16_t *>(heap_caps_calloc(1, bytes, MALLOC_CAP_INTERNAL));
    }
    if (buf == nullptr) {
        ESP_LOGW(TAG, "GRAM clear skipped: no buffer");
        return ESP_ERR_NO_MEM;
    }

    for (int y = 0; y < BOARD_LCD_V_RES; y += k_lines) {
        const int y_end = (y + k_lines < BOARD_LCD_V_RES) ? (y + k_lines)
                                                          : BOARD_LCD_V_RES;
        esp_err_t err = esp_lcd_panel_draw_bitmap(s_panel, 0, y, BOARD_LCD_H_RES,
                                                  y_end, buf);
        if (err != ESP_OK) {
            heap_caps_free(buf);
            return err;
        }
    }
    heap_caps_free(buf);
    return ESP_OK;
}

void display_set_backlight(bool on)
{
    if (!s_backlight_inited) {
        return;
    }
    const uint32_t duty = on ? 200 : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    s_backlight_on.store(on, std::memory_order_relaxed);
    if (!on && s_idle_timer != nullptr) {
        (void)esp_timer_stop(s_idle_timer);
    }
}

bool display_is_backlight_on(void)
{
    return s_backlight_on.load(std::memory_order_relaxed);
}

void display_activity_notify(void)
{
    if (!s_backlight_inited) {
        return;
    }
    if (!s_backlight_on.load(std::memory_order_relaxed)) {
        ESP_LOGI(TAG, "Activity: turning backlight on");
        display_set_backlight(true);
    }
    display_idle_timer_restart();
}

void display_set_idle_hold(bool hold)
{
    const bool prev = s_idle_hold.exchange(hold, std::memory_order_relaxed);
    if (hold) {
        if (!s_backlight_on.load(std::memory_order_relaxed)) {
            display_set_backlight(true);
        }
        if (s_idle_timer != nullptr) {
            (void)esp_timer_stop(s_idle_timer);
        }
        return;
    }
    /* Leaving hold: start the normal 60s idle countdown. */
    if (prev) {
        display_activity_notify();
    }
}

static void touch_map_point(lv_point_t *pt)
{
    /*
     * Independent of BOARD_LCD_MIRROR_*: LCD MADCTL flips panel scan only.
     * Remap touch here only when the digitizer origin disagrees with LVGL.
     */
#if BOARD_TOUCH_MIRROR_X
    pt->x = static_cast<lv_coord_t>(BOARD_LCD_H_RES - 1 - pt->x);
#endif
#if BOARD_TOUCH_MIRROR_Y
    pt->y = static_cast<lv_coord_t>(BOARD_LCD_V_RES - 1 - pt->y);
#endif
}

#if LVGL_VERSION_MAJOR >= 9
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
#else
static void touch_read_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    (void)drv;
#endif
    it7259_point_t point = {};
    if (it7259_read(&point) != ESP_OK || !point.pressed) {
        data->point.x = point.x;
        data->point.y = point.y;
        touch_map_point(&data->point);
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    /*
     * First press while the panel is dark only wakes the backlight — do not
     * deliver that press as a UI click (avoids accidental button hits).
     */
    if (!display_is_backlight_on()) {
        display_activity_notify();
        data->point.x = point.x;
        data->point.y = point.y;
        touch_map_point(&data->point);
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }

    display_activity_notify();
    data->point.x = point.x;
    data->point.y = point.y;
    touch_map_point(&data->point);
    data->state = LV_INDEV_STATE_PRESSED;
}

static esp_err_t display_init_hw(void)
{
    if (s_hw_ready) {
        return ESP_OK;
    }

    backlight_init();
    display_set_backlight(false);

    ESP_LOGI(TAG, "Init SPI bus for GC9A01");
    spi_bus_config_t bus_config = {};
    bus_config.mosi_io_num = BOARD_LCD_MOSI_GPIO;
    bus_config.miso_io_num = -1;
    bus_config.sclk_io_num = BOARD_LCD_SCLK_GPIO;
    bus_config.quadwp_io_num = -1;
    bus_config.quadhd_io_num = -1;
    /* Match the largest LVGL draw buffer we try (20 lines). */
    bus_config.max_transfer_sz = BOARD_LCD_H_RES * 20 * sizeof(uint16_t);
    ESP_ERROR_CHECK(spi_bus_initialize(BOARD_LCD_HOST, &bus_config, SPI_DMA_CH_AUTO));

    ESP_LOGI(TAG, "Install panel IO");
    esp_lcd_panel_io_spi_config_t io_config = {};
    io_config.cs_gpio_num = BOARD_LCD_CS_GPIO;
    io_config.dc_gpio_num = BOARD_LCD_DC_GPIO;
    io_config.spi_mode = 0;
    io_config.pclk_hz = BOARD_LCD_SPI_PIXEL_CLOCK_HZ;
    io_config.trans_queue_depth = 10;
    io_config.lcd_cmd_bits = 8;
    io_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(
        (esp_lcd_spi_bus_handle_t)BOARD_LCD_HOST, &io_config, &s_io));

    ESP_LOGI(TAG, "Install GC9A01 panel driver");
    esp_lcd_panel_dev_config_t panel_config = {};
    panel_config.reset_gpio_num = BOARD_LCD_RST_GPIO;
    panel_config.bits_per_pixel = 16;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_BGR;
#endif
    ESP_ERROR_CHECK(esp_lcd_new_panel_gc9a01(s_io, &panel_config, &s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(s_panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(s_panel, true));
    /* Fix horizontally reversed glyphs/UI on this GC9A01 module (MADCTL MX). */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, BOARD_LCD_MIRROR_X != 0,
                                         BOARD_LCD_MIRROR_Y != 0));
    /*
     * Clear GRAM before DISPON so residual random pixels are never lit.
     * Enable backlight once the panel is solid black — do NOT wait for the
     * first LVGL frame. If UI init/paint fails, waiting forever left the
     * round LCD looking "dead" after flash.
     */
    (void)display_clear_gram_black();
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));
    display_set_backlight(true);

    s_hw_ready = true;
    return ESP_OK;
}

static esp_err_t display_start_lvgl(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    if (!s_hw_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    /*
     * Probe before lvgl_port_init(): that call starts an LVGL task even when
     * the later draw-buffer allocation fails, permanently wasting RAM.
     */
    constexpr uint16_t k_min_lines = 10;
    const size_t min_buf_bytes =
        static_cast<size_t>(BOARD_LCD_H_RES) * k_min_lines * sizeof(uint16_t);
    bool use_dma = true;
    void *probe = heap_caps_malloc(min_buf_bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    if (probe == nullptr) {
        use_dma = false;
        probe = heap_caps_malloc(min_buf_bytes, MALLOC_CAP_INTERNAL);
    }
    if (probe == nullptr) {
        ESP_LOGE(TAG, "Not enough heap for LVGL buffer (need %u bytes)",
                 static_cast<unsigned>(min_buf_bytes));
        return ESP_ERR_NO_MEM;
    }
    heap_caps_free(probe);

    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /*
     * Default 7168 is heavy with Matter on S3 without PSRAM, but 4096 was too
     * tight for bpp8 CJK + QR first paint (stack smash → blank forever).
     */
    lvgl_cfg.task_stack = 6144;
    esp_err_t lvgl_err = lvgl_port_init(&lvgl_cfg);
    if (lvgl_err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s (heap=%u) — UI deferred",
                 esp_err_to_name(lvgl_err),
                 static_cast<unsigned>(esp_get_free_heap_size()));
        return lvgl_err;
    }

    /*
     * Prefer a single smaller draw buffer. After Matter PASE free heap is often
     * ~20–30KB with fragmentation, so skip the 20-line attempt unless a large
     * enough contiguous internal block is available (avoids a failing alloc).
     */
    const size_t largest_dma =
        heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    const size_t largest_internal =
        heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    constexpr size_t k_bytes_20 =
        static_cast<size_t>(BOARD_LCD_H_RES) * 20U * sizeof(uint16_t);
    /* Leave headroom for LVGL port bookkeeping beyond the draw buffer itself. */
    const bool try_20 =
        (use_dma && largest_dma >= (k_bytes_20 + 4096)) ||
        (!use_dma && largest_internal >= (k_bytes_20 + 4096));

    uint16_t line_opts[2];
    size_t line_opt_count = 0;
    if (try_20) {
        line_opts[line_opt_count++] = 20;
    } else {
        ESP_LOGI(TAG,
                 "LVGL: skip 20-line buffer (largest dma=%u internal=%u heap=%u)",
                 static_cast<unsigned>(largest_dma),
                 static_cast<unsigned>(largest_internal),
                 static_cast<unsigned>(esp_get_free_heap_size()));
    }
    line_opts[line_opt_count++] = 10;

    const bool dma_opts[] = {use_dma, false};
    for (bool dma : dma_opts) {
        for (size_t i = 0; i < line_opt_count; ++i) {
            const uint16_t lines = line_opts[i];
            lvgl_port_display_cfg_t disp_cfg = {};
            disp_cfg.io_handle = s_io;
            disp_cfg.panel_handle = s_panel;
            disp_cfg.buffer_size = BOARD_LCD_H_RES * lines;
            disp_cfg.double_buffer = false;
            disp_cfg.hres = BOARD_LCD_H_RES;
            disp_cfg.vres = BOARD_LCD_V_RES;
            disp_cfg.monochrome = false;
            disp_cfg.rotation.swap_xy = false;
            /*
             * esp_lvgl_port applies these via esp_lcd_panel_mirror() after
             * add_disp and overwrites any earlier panel_mirror() call — so the
             * corrected scan direction MUST be set here, not only above.
             */
            disp_cfg.rotation.mirror_x = (BOARD_LCD_MIRROR_X != 0);
            disp_cfg.rotation.mirror_y = (BOARD_LCD_MIRROR_Y != 0);
            disp_cfg.flags.buff_dma = dma;

            s_disp = lvgl_port_add_disp(&disp_cfg);
            if (s_disp != nullptr) {
                /* Belt-and-suspenders: re-assert MADCTL after port init. */
                ESP_ERROR_CHECK(esp_lcd_panel_mirror(
                    s_panel, BOARD_LCD_MIRROR_X != 0, BOARD_LCD_MIRROR_Y != 0));
                ESP_LOGI(TAG,
                         "LVGL display buffer: %u lines, dma=%d, mirror_x=%d mirror_y=%d",
                         lines, dma ? 1 : 0, BOARD_LCD_MIRROR_X, BOARD_LCD_MIRROR_Y);
                break;
            }
            ESP_LOGW(TAG,
                     "lvgl_port_add_disp failed with %u-line dma=%d; retrying",
                     lines, dma ? 1 : 0);
        }
        if (s_disp != nullptr) {
            break;
        }
    }
    if (s_disp == nullptr) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        (void)lvgl_port_deinit();
        return ESP_FAIL;
    }

    if (!s_touch_indev_added) {
        esp_err_t touch_err = it7259_init();
        if (touch_err == ESP_OK) {
#if LVGL_VERSION_MAJOR >= 9
            lv_indev_t *indev = lv_indev_create();
            lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
            lv_indev_set_read_cb(indev, touch_read_cb);
            lv_indev_set_display(indev, s_disp);
#else
            static lv_indev_drv_t indev_drv;
            lv_indev_drv_init(&indev_drv);
            indev_drv.type = LV_INDEV_TYPE_POINTER;
            indev_drv.read_cb = touch_read_cb;
            indev_drv.disp = s_disp;
            lv_indev_drv_register(&indev_drv);
#endif
            s_touch_indev_added = true;
        } else {
            ESP_LOGW(TAG, "Touch unavailable (%s); UI is display-only",
                     esp_err_to_name(touch_err));
        }
    }

    /* Backlight already on after black GRAM clear in display_init_hw(). */
    s_ready = true;
    ESP_LOGI(TAG, "GC9A01 + LVGL ready (%dx%d), backlight on",
             BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

esp_err_t display_init(void)
{
    esp_err_t err = display_init_hw();
    if (err != ESP_OK) {
        return err;
    }
    return display_start_lvgl();
}

void display_suspend_lvgl(void)
{
    if (!s_ready && s_disp == nullptr) {
        return;
    }

    ESP_LOGI(TAG,
             "Suspending LVGL (keep panel frame + backlight) for Matter PASE "
             "(free heap=%u)",
             static_cast<unsigned>(esp_get_free_heap_size()));

    /*
     * Leave the GC9A01 GRAM and backlight alone so the last drawn frame
     * (e.g. "配对中...") stays visible while BLE PASE reclaims LVGL heap.
     */
    if (s_disp != nullptr) {
        (void)lvgl_port_remove_disp(s_disp);
        s_disp = nullptr;
    }
    (void)lvgl_port_deinit();
    s_ready = false;
    /* Touch indev belongs to LVGL; recreate after the next start. */
    s_touch_indev_added = false;

    ESP_LOGI(TAG, "LVGL suspended (free heap=%u)",
             static_cast<unsigned>(esp_get_free_heap_size()));
}

lv_disp_t *display_get_disp(void)
{
    return s_disp;
}

bool display_is_ready(void)
{
    return s_ready;
}
