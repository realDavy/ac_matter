#include "display_gc9a01.h"

#include "board_pins.h"
#include "it7259.h"

#include <driver/gpio.h>
#include <driver/ledc.h>
#include <driver/spi_master.h>
#include <esp_lcd_gc9a01.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lvgl_port.h>
#include <esp_log.h>

static const char *TAG = "display";

static esp_lcd_panel_io_handle_t s_io = nullptr;
static esp_lcd_panel_handle_t s_panel = nullptr;
static lv_display_t *s_disp = nullptr;
static bool s_ready = false;

static void backlight_init(void)
{
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
}

void display_set_backlight(bool on)
{
    const uint32_t duty = on ? 200 : 0;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
}

static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    it7259_point_t point = {};
    if (it7259_read(&point) != ESP_OK || !point.pressed) {
        data->state = LV_INDEV_STATE_RELEASED;
        return;
    }
    data->point.x = point.x;
    data->point.y = point.y;
    data->state = LV_INDEV_STATE_PRESSED;
}

esp_err_t display_init(void)
{
    if (s_ready) {
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
    bus_config.max_transfer_sz = BOARD_LCD_H_RES * 80 * sizeof(uint16_t);
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

    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_cfg));

    lvgl_port_display_cfg_t disp_cfg = {};
    disp_cfg.io_handle = s_io;
    disp_cfg.panel_handle = s_panel;
    disp_cfg.buffer_size = BOARD_LCD_H_RES * 40;
    disp_cfg.double_buffer = true;
    disp_cfg.hres = BOARD_LCD_H_RES;
    disp_cfg.vres = BOARD_LCD_V_RES;
    disp_cfg.monochrome = false;
    disp_cfg.rotation.swap_xy = false;
    disp_cfg.rotation.mirror_x = false;
    disp_cfg.rotation.mirror_y = false;
    disp_cfg.flags.buff_dma = true;
#if defined(LVGL_VERSION_MAJOR) && (LVGL_VERSION_MAJOR >= 9)
    disp_cfg.flags.swap_bytes = true;
#endif

    s_disp = lvgl_port_add_disp(&disp_cfg);
    if (s_disp == nullptr) {
        ESP_LOGE(TAG, "lvgl_port_add_disp failed");
        return ESP_FAIL;
    }

    esp_err_t touch_err = it7259_init();
    if (touch_err == ESP_OK) {
        lv_indev_t *indev = lv_indev_create();
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(indev, touch_read_cb);
        lv_indev_set_display(indev, s_disp);
    } else {
        ESP_LOGW(TAG, "Touch unavailable (%s); UI is display-only",
                 esp_err_to_name(touch_err));
    }

    display_set_backlight(true);
    s_ready = true;
    ESP_LOGI(TAG, "GC9A01 + LVGL ready (%dx%d)", BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    return ESP_OK;
}

lv_disp_t *display_get_disp(void)
{
    return s_disp;
}

bool display_is_ready(void)
{
    return s_ready;
}
)
