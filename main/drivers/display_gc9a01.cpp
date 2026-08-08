#include "display_gc9a01.h"

#include "board_pins.h"
#include "it7259.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_idf_version.h>
#include <esp_lcd_gc9a01.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lvgl_port.h>
#include <esp_log.h>

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io = nullptr;
static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_disp_t *s_disp = nullptr;
static bool s_hw_ready = false;
static bool s_ready = false;
static bool s_touch_indev_added = false;
static bool s_backlight_inited = false;

static void backlight_init(void)
{
    if (s_backlight_inited) {
        return;
    }

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
}

void display_set_backlight(bool on)
{
    if (!s_backlight_inited) {
        return;
    }
    const uint32_t duty = on ? 200 : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
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
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->point.x = point.x;
    data->point.y = point.y;
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
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(s_panel, false, false));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(s_panel, true));

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
    /* Default 7168 is too heavy alongside Matter + CHIPoBLE on S3 without PSRAM. */
    lvgl_cfg.task_stack = 4096;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    /*
     * Prefer a single smaller draw buffer. Fall back to fewer lines and to
     * non-DMA memory if internal DMA RAM is exhausted.
     */
    static const uint16_t k_buf_lines[] = {20, 10};
    const bool dma_opts[] = {use_dma, false};
    for (bool dma : dma_opts) {
        for (uint16_t lines : k_buf_lines) {
            lvgl_port_display_cfg_t disp_cfg = {};
            disp_cfg.io_handle = s_io;
            disp_cfg.panel_handle = s_panel;
            disp_cfg.buffer_size = BOARD_LCD_H_RES * lines;
            disp_cfg.double_buffer = false;
            disp_cfg.hres = BOARD_LCD_H_RES;
            disp_cfg.vres = BOARD_LCD_V_RES;
            disp_cfg.monochrome = false;
            disp_cfg.rotation.swap_xy = false;
            disp_cfg.rotation.mirror_x = false;
            disp_cfg.rotation.mirror_y = false;
            disp_cfg.flags.buff_dma = dma;

            s_disp = lvgl_port_add_disp(&disp_cfg);
            if (s_disp != nullptr) {
                ESP_LOGI(TAG, "LVGL display buffer: %u lines, dma=%d", lines,
                         dma ? 1 : 0);
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

    display_set_backlight(true);
    s_ready = true;
    ESP_LOGI(TAG, "GC9A01 + LVGL ready (%dx%d)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
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
        display_set_backlight(false);
        return;
    }

    ESP_LOGI(TAG, "Suspending LVGL to free heap for Matter PASE (free heap=%u)",
             static_cast<unsigned>(esp_get_free_heap_size()));

    display_set_backlight(false);

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
