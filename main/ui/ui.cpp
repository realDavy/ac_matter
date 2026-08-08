#include "ui.h"
#include "ui_fonts.h"
#include "ui_i18n.h"
#include "qrcode.h"

#include "board_pins.h"
#include "display_gc9a01.h"
#include "ws2812_temp_light.h"

#include <app_priv.h>

#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <setup_payload/OnboardingCodesUtil.h>

#include <atomic>
#include <cstdio>
#include <cstring>

static const char *TAG = "ui";

enum class ui_screen_t : uint8_t {
    PAIRING = 0,
    LEARN,
    AC,
    LIGHT,
};

static std::atomic<bool> s_english{false};
static std::atomic<bool> s_ready{false};
static std::atomic<bool> s_stop_task{false};
static TaskHandle_t s_task = nullptr;
static ui_screen_t s_screen = ui_screen_t::PAIRING;

static lv_obj_t *s_root = nullptr;
static lv_obj_t *s_title = nullptr;
static lv_obj_t *s_subtitle = nullptr;
static lv_obj_t *s_qr_img = nullptr;
static lv_obj_t *s_code_label = nullptr;
static lv_obj_t *s_btn_primary = nullptr;
static lv_obj_t *s_btn_primary_label = nullptr;
static lv_obj_t *s_btn_power = nullptr;
static lv_obj_t *s_btn_power_label = nullptr;
static lv_obj_t *s_btn_down = nullptr;
static lv_obj_t *s_btn_up = nullptr;
static lv_obj_t *s_temp_label = nullptr;
static lv_obj_t *s_mode_list = nullptr;
static lv_obj_t *s_brightness = nullptr;
static lv_obj_t *s_lang_btn = nullptr;
static lv_obj_t *s_hint = nullptr;

static uint16_t s_qr_pixels[80 * 80];
#if LVGL_VERSION_MAJOR >= 9
static lv_image_dsc_t s_qr_dsc = {};
#else
static lv_img_dsc_t s_qr_dsc = {};
#endif
static uint8_t s_qr_modules[256];
static char s_qr_text[160] = {};
static char s_manual_code[32] = {};

static constexpr int kQrVersion = 4;
static constexpr int kQrScale = 2;

#if LVGL_VERSION_MAJOR >= 9
#define UI_BTN_CREATE(parent) lv_button_create(parent)
#define UI_SCREEN_ACTIVE() lv_screen_active()
#define UI_IMAGE_CREATE(parent) lv_image_create(parent)
#define UI_IMAGE_SET_SRC(obj, src) lv_image_set_src((obj), (src))
#else
#define UI_BTN_CREATE(parent) lv_btn_create(parent)
#define UI_SCREEN_ACTIVE() lv_scr_act()
#define UI_IMAGE_CREATE(parent) lv_img_create(parent)
#define UI_IMAGE_SET_SRC(obj, src) lv_img_set_src((obj), (src))
#endif

static void style_circle_screen(lv_obj_t *obj)
{
    lv_obj_set_size(obj, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(obj, lv_color_hex(0x101820), 0);
    lv_obj_set_style_bg_grad_color(obj, lv_color_hex(0x1c2e3a), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_VER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 8, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_btn(lv_obj_t *btn, uint32_t color)
{
    lv_obj_set_style_radius(btn, 18, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
}

static lv_obj_t *make_label(lv_obj_t *parent, const lv_font_t *font, uint32_t color)
{
    lv_obj_t *lbl = lv_label_create(parent);
    lv_obj_set_style_text_font(lbl, font, 0);
    lv_obj_set_style_text_color(lbl, lv_color_hex(color), 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    return lbl;
}

static void refresh_onboarding_codes(void)
{
    chip::MutableCharSpan qr(s_qr_text);
    CHIP_ERROR err = GetQRCode(
        qr, chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    if (err != CHIP_NO_ERROR) {
        std::snprintf(s_qr_text, sizeof(s_qr_text), "MT:unavailable");
        ESP_LOGW(TAG, "GetQRCode failed: %" CHIP_ERROR_FORMAT, err.Format());
    } else if (qr.size() < sizeof(s_qr_text)) {
        s_qr_text[qr.size()] = '\0';
    }

    chip::MutableCharSpan manual(s_manual_code);
    err = GetManualPairingCode(
        manual, chip::RendezvousInformationFlags(chip::RendezvousInformationFlag::kBLE));
    if (err != CHIP_NO_ERROR) {
        std::snprintf(s_manual_code, sizeof(s_manual_code), "--------");
    } else if (manual.size() < sizeof(s_manual_code)) {
        s_manual_code[manual.size()] = '\0';
    }

    ESP_LOGI(TAG, "UI Matter QR: %s", s_qr_text);
    ESP_LOGI(TAG, "UI Matter code: %s", s_manual_code);
}

static void draw_qr(const char *text)
{
    if (s_qr_img == nullptr || text == nullptr) {
        return;
    }

    QRCode qrcode;
    if (qrcode_initText(&qrcode, s_qr_modules, kQrVersion, ECC_LOW, text) != 0) {
        ESP_LOGW(TAG, "QR encode failed");
        return;
    }

    const int size = qrcode.size;
    const int px = size * kQrScale;
    if (px > 80) {
        ESP_LOGW(TAG, "QR too large for buffer: %d", px);
        return;
    }

    const uint16_t white = 0xFFFF;
    const uint16_t black = 0x0000;
    for (int i = 0; i < px * px; ++i) {
        s_qr_pixels[i] = white;
    }

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!qrcode_getModule(&qrcode, static_cast<uint8_t>(x),
                                  static_cast<uint8_t>(y))) {
                continue;
            }
            for (int dy = 0; dy < kQrScale; ++dy) {
                for (int dx = 0; dx < kQrScale; ++dx) {
                    s_qr_pixels[(y * kQrScale + dy) * px + (x * kQrScale + dx)] = black;
                }
            }
        }
    }

#if LVGL_VERSION_MAJOR >= 9
    s_qr_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_qr_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_qr_dsc.header.w = px;
    s_qr_dsc.header.h = px;
    s_qr_dsc.header.stride = px * 2;
    s_qr_dsc.data_size = static_cast<uint32_t>(px * px * 2);
    s_qr_dsc.data = reinterpret_cast<const uint8_t *>(s_qr_pixels);
#else
    s_qr_dsc.header.always_zero = 0;
    s_qr_dsc.header.w = px;
    s_qr_dsc.header.h = px;
    s_qr_dsc.header.cf = LV_IMG_CF_TRUE_COLOR;
    s_qr_dsc.data_size = px * px * sizeof(uint16_t);
    s_qr_dsc.data = reinterpret_cast<const uint8_t *>(s_qr_pixels);
#endif
    UI_IMAGE_SET_SRC(s_qr_img, &s_qr_dsc);
}

static void hide_all_controls(void)
{
    auto hide = [](lv_obj_t *o) {
        if (o) {
            lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
        }
    };
    hide(s_qr_img);
    hide(s_code_label);
    hide(s_btn_primary);
    hide(s_btn_power);
    hide(s_btn_down);
    hide(s_btn_up);
    hide(s_temp_label);
    hide(s_mode_list);
    hide(s_brightness);
    hide(s_hint);
}

static void show_pairing(void)
{
    const ui_strings_t *s = ui_strings(s_english.load());
    hide_all_controls();
    refresh_onboarding_codes();
    draw_qr(s_qr_text);

    lv_label_set_text(s_title, s->pairing_title);
    lv_label_set_text(s_subtitle, s->pairing_hint);
    lv_obj_clear_flag(s_qr_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_code_label, LV_OBJ_FLAG_HIDDEN);

    char line[64];
    std::snprintf(line, sizeof(line), "%s\n%s", s->manual_code, s_manual_code);
    lv_label_set_text(s_code_label, line);
}

static void show_learn(void)
{
    const ui_strings_t *s = ui_strings(s_english.load());
    hide_all_controls();
    lv_label_set_text(s_title, s->learn_title);
    lv_label_set_text(s_subtitle, s->learn_hint);
    lv_obj_clear_flag(s_btn_primary, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_btn_primary_label,
                      app_driver_ir_is_pairing() ? "..." : s->learn_btn);
}

static void show_ac(void)
{
    const ui_strings_t *s = ui_strings(s_english.load());
    hide_all_controls();

    int temp = 25;
    bool power = false;
    app_driver_ui_get_ac_state(&temp, &power);

    lv_label_set_text(s_title, s->ac_title);
    lv_label_set_text(s_subtitle, s->swipe_hint);
    lv_obj_clear_flag(s_btn_power, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_btn_down, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_btn_up, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_temp_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);

    lv_label_set_text(s_btn_power_label, power ? s->power_off : s->power_on);
    char tbuf[16];
    std::snprintf(tbuf, sizeof(tbuf), "%d°", temp);
    lv_label_set_text(s_temp_label, tbuf);
    lv_label_set_text(s_hint, s->swipe_hint);

    lv_obj_t *down_lbl = lv_obj_get_child(s_btn_down, 0);
    lv_obj_t *up_lbl = lv_obj_get_child(s_btn_up, 0);
    if (down_lbl) {
        lv_label_set_text(down_lbl, s->cool_down);
    }
    if (up_lbl) {
        lv_label_set_text(up_lbl, s->heat_up);
    }
}

static void show_light(void)
{
    const ui_strings_t *s = ui_strings(s_english.load());
    hide_all_controls();
    lv_label_set_text(s_title, s->light_title);
    lv_label_set_text(s_subtitle, s->brightness);
    lv_obj_clear_flag(s_mode_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_brightness, LV_OBJ_FLAG_HIDDEN);

    const char *modes[] = {
        s->mode_night, s->mode_manual, s->mode_temp,
        s->mode_solid, s->mode_rainbow, s->mode_white,
    };
    const uint32_t count = lv_obj_get_child_cnt(s_mode_list);
    for (uint32_t i = 0; i < count && i < 6; ++i) {
        lv_obj_t *btn = lv_obj_get_child(s_mode_list, i);
        lv_obj_t *lbl = btn ? lv_obj_get_child(btn, 0) : nullptr;
        if (lbl) {
            lv_label_set_text(lbl, modes[i]);
        }
    }

    lv_slider_set_value(s_brightness, ws2812_temp_light_get_brightness(), LV_ANIM_OFF);
}

static void apply_screen(ui_screen_t screen)
{
    s_screen = screen;
    switch (screen) {
    case ui_screen_t::PAIRING:
        show_pairing();
        break;
    case ui_screen_t::LEARN:
        show_learn();
        break;
    case ui_screen_t::AC:
        show_ac();
        break;
    case ui_screen_t::LIGHT:
        show_light();
        break;
    }

    const ui_strings_t *s = ui_strings(s_english.load());
    if (s_lang_btn) {
        lv_obj_t *lbl = lv_obj_get_child(s_lang_btn, 0);
        if (lbl) {
            lv_label_set_text(lbl, s->lang_toggle);
        }
    }
}

static void on_lang(lv_event_t *e)
{
    (void)e;
    s_english.store(!s_english.load());
    apply_screen(s_screen);
}

static void on_learn(lv_event_t *e)
{
    (void)e;
    app_driver_ir_start_learn();
    ws2812_temp_light_set_learn_active(true);
    apply_screen(ui_screen_t::LEARN);
}

static void on_power(lv_event_t *e)
{
    (void)e;
    app_driver_ui_toggle_power();
    apply_screen(ui_screen_t::AC);
}

static void on_temp_down(lv_event_t *e)
{
    (void)e;
    app_driver_ui_adjust_temp(-1);
    apply_screen(ui_screen_t::AC);
}

static void on_temp_up(lv_event_t *e)
{
    (void)e;
    app_driver_ui_adjust_temp(1);
    apply_screen(ui_screen_t::AC);
}

static void on_mode(lv_event_t *e)
{
    const uintptr_t mode = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    ws2812_temp_light_set_mode(static_cast<ws2812_light_mode_t>(mode));
}

static void on_brightness(lv_event_t *e)
{
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const uint8_t level = static_cast<uint8_t>(lv_slider_get_value(slider));
    ws2812_temp_light_set_brightness(level);
    app_driver_ui_set_light_brightness(level);
}

static void on_gesture(lv_event_t *e)
{
    (void)e;
#if LVGL_VERSION_MAJOR >= 9
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
#else
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
#endif
    if (s_screen == ui_screen_t::AC && dir == LV_DIR_LEFT) {
        apply_screen(ui_screen_t::LIGHT);
    } else if (s_screen == ui_screen_t::LIGHT && dir == LV_DIR_RIGHT) {
        apply_screen(ui_screen_t::AC);
    }
}

static void build_ui(void)
{
    s_root = lv_obj_create(UI_SCREEN_ACTIVE());
    style_circle_screen(s_root);
    lv_obj_center(s_root);
    lv_obj_add_event_cb(s_root, on_gesture, LV_EVENT_GESTURE, nullptr);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_title = make_label(s_root, &ui_font_cn_20, 0xE8F1F8);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 10);

    s_subtitle = make_label(s_root, &ui_font_cn_16, 0x9BB4C4);
    lv_obj_set_width(s_subtitle, 200);
    lv_obj_align(s_subtitle, LV_ALIGN_TOP_MID, 0, 38);

    s_qr_img = UI_IMAGE_CREATE(s_root);
    lv_obj_align(s_qr_img, LV_ALIGN_CENTER, 0, -6);

    s_code_label = make_label(s_root, &ui_font_cn_16, 0xD0E4F0);
    lv_obj_set_width(s_code_label, 210);
    lv_obj_align(s_code_label, LV_ALIGN_BOTTOM_MID, 0, -28);

    s_btn_primary = UI_BTN_CREATE(s_root);
    style_btn(s_btn_primary, 0x2F6FED);
    lv_obj_set_size(s_btn_primary, 140, 44);
    lv_obj_align(s_btn_primary, LV_ALIGN_CENTER, 0, 40);
    s_btn_primary_label = make_label(s_btn_primary, &ui_font_cn_16, 0xFFFFFF);
    lv_obj_center(s_btn_primary_label);
    lv_obj_add_event_cb(s_btn_primary, on_learn, LV_EVENT_CLICKED, nullptr);

    s_btn_power = UI_BTN_CREATE(s_root);
    style_btn(s_btn_power, 0x1F8A5F);
    lv_obj_set_size(s_btn_power, 100, 42);
    lv_obj_align(s_btn_power, LV_ALIGN_CENTER, 0, -10);
    s_btn_power_label = make_label(s_btn_power, &ui_font_cn_16, 0xFFFFFF);
    lv_obj_center(s_btn_power_label);
    lv_obj_add_event_cb(s_btn_power, on_power, LV_EVENT_CLICKED, nullptr);

    s_btn_down = UI_BTN_CREATE(s_root);
    style_btn(s_btn_down, 0x2B4C7E);
    lv_obj_set_size(s_btn_down, 88, 40);
    lv_obj_align(s_btn_down, LV_ALIGN_CENTER, -55, 50);
    lv_obj_center(make_label(s_btn_down, &ui_font_cn_16, 0xFFFFFF));
    lv_obj_add_event_cb(s_btn_down, on_temp_down, LV_EVENT_CLICKED, nullptr);

    s_btn_up = UI_BTN_CREATE(s_root);
    style_btn(s_btn_up, 0x8B3A3A);
    lv_obj_set_size(s_btn_up, 88, 40);
    lv_obj_align(s_btn_up, LV_ALIGN_CENTER, 55, 50);
    lv_obj_center(make_label(s_btn_up, &ui_font_cn_16, 0xFFFFFF));
    lv_obj_add_event_cb(s_btn_up, on_temp_up, LV_EVENT_CLICKED, nullptr);

    s_temp_label = make_label(s_root, &ui_font_cn_20, 0xF2F7FA);
    lv_obj_align(s_temp_label, LV_ALIGN_CENTER, 0, -55);

    s_hint = make_label(s_root, &ui_font_cn_16, 0x7F97A8);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    s_mode_list = lv_obj_create(s_root);
    lv_obj_set_size(s_mode_list, 210, 120);
    lv_obj_align(s_mode_list, LV_ALIGN_CENTER, 0, -10);
    lv_obj_set_style_bg_opa(s_mode_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_mode_list, 0, 0);
    lv_obj_set_flex_flow(s_mode_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_all(s_mode_list, 2, 0);
    lv_obj_set_style_pad_row(s_mode_list, 4, 0);
    lv_obj_set_style_pad_column(s_mode_list, 4, 0);

    for (int i = 0; i < 6; ++i) {
        lv_obj_t *b = UI_BTN_CREATE(s_mode_list);
        style_btn(b, 0x243447);
        lv_obj_set_size(b, 98, 34);
        lv_obj_center(make_label(b, &ui_font_cn_16, 0xE6EEF5));
        lv_obj_add_event_cb(b, on_mode, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }

    s_brightness = lv_slider_create(s_root);
    lv_obj_set_width(s_brightness, 160);
    lv_slider_set_range(s_brightness, 1, 254);
    lv_obj_align(s_brightness, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_obj_add_event_cb(s_brightness, on_brightness, LV_EVENT_VALUE_CHANGED, nullptr);

    s_lang_btn = UI_BTN_CREATE(s_root);
    style_btn(s_lang_btn, 0x3A4A58);
    lv_obj_set_size(s_lang_btn, 44, 28);
    lv_obj_align(s_lang_btn, LV_ALIGN_TOP_RIGHT, -4, 4);
    lv_obj_center(make_label(s_lang_btn, &ui_font_cn_16, 0xFFFFFF));
    lv_obj_add_event_cb(s_lang_btn, on_lang, LV_EVENT_CLICKED, nullptr);
}

static ui_screen_t decide_screen(void)
{
    const app_matter_state_t matter = app_get_matter_state_locked();
    if (matter == app_matter_state_t::NOT_COMMISSIONED ||
        matter == app_matter_state_t::COMMISSIONING) {
        return ui_screen_t::PAIRING;
    }
    if (!app_driver_ir_is_paired()) {
        return ui_screen_t::LEARN;
    }
    if (s_screen == ui_screen_t::LIGHT) {
        return ui_screen_t::LIGHT;
    }
    return ui_screen_t::AC;
}

static void ui_task(void *arg)
{
    (void)arg;
    ui_screen_t last = static_cast<ui_screen_t>(0xFF);
    uint32_t ticks = 0;

    while (!s_stop_task.load()) {
        if (display_is_ready() && lvgl_port_lock(50)) {
            const ui_screen_t next = decide_screen();
            if (next != last) {
                if (next != ui_screen_t::LEARN) {
                    ws2812_temp_light_set_learn_active(false);
                }
                apply_screen(next);
                last = next;
                ticks = 0;
            } else if (next == ui_screen_t::AC || next == ui_screen_t::LEARN) {
                if (++ticks >= 2) {
                    apply_screen(next);
                    ticks = 0;
                }
            } else if (next == ui_screen_t::PAIRING) {
                if (++ticks >= 25) {
                    apply_screen(next);
                    ticks = 0;
                }
            }
            lvgl_port_unlock();
        }
        vTaskDelay(pdMS_TO_TICKS(400));
    }

    s_task = nullptr;
    vTaskDelete(nullptr);
}

esp_err_t ui_init(void)
{
    if (s_ready.load()) {
        return ESP_OK;
    }
    if (!display_is_ready()) {
        ESP_LOGE(TAG, "Display not ready");
        return ESP_ERR_INVALID_STATE;
    }

    s_root = nullptr;
    s_screen = ui_screen_t::PAIRING;

    if (lvgl_port_lock(1000)) {
        build_ui();
        apply_screen(decide_screen());
        lvgl_port_unlock();
    }

    s_stop_task.store(false);
    /* Pairing / AC refresh loops are light; keep stack small for PASE heap. */
    if (xTaskCreate(ui_task, "ui_task", 4096, nullptr, 4, &s_task) != pdPASS) {
        s_task = nullptr;
        return ESP_ERR_NO_MEM;
    }

    s_ready.store(true);
    ESP_LOGI(TAG, "UI ready (default language: Chinese)");
    return ESP_OK;
}

esp_err_t ui_show_commissioning_busy(void)
{
    if (!s_ready.load() || !display_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    const ui_strings_t *s = ui_strings(s_english.load());
    if (!lvgl_port_lock(200)) {
        return ESP_ERR_TIMEOUT;
    }

    hide_all_controls();
    if (s_lang_btn) {
        lv_obj_add_flag(s_lang_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_label_set_text(s_title, s->pairing_busy_title);
    lv_label_set_text(s_subtitle, s->pairing_busy_hint);
    /* Drop QR image association so the panel only keeps the text frame. */
    if (s_qr_img) {
#if LVGL_VERSION_MAJOR >= 9
        lv_image_set_src(s_qr_img, nullptr);
#else
        lv_img_set_src(s_qr_img, nullptr);
#endif
    }
    std::memset(s_qr_pixels, 0, sizeof(s_qr_pixels));
    std::memset(s_qr_text, 0, sizeof(s_qr_text));
    std::memset(s_manual_code, 0, sizeof(s_manual_code));

    if (s_root) {
        lv_obj_invalidate(s_root);
    }
    lv_refr_now(display_get_disp());
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Showing commissioning-busy screen");
    return ESP_OK;
}

void ui_deinit(void)
{
    if (!s_ready.load() && s_task == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Stopping UI task");
    s_stop_task.store(true);
    s_ready.store(false);

    /*
     * Must be safe on the CHIP event loop: do not block. Force-delete the UI
     * task; LVGL is torn down immediately afterwards by display_suspend_lvgl().
     */
    TaskHandle_t task = s_task;
    s_task = nullptr;
    if (task != nullptr) {
        vTaskDelete(task);
    }

    s_root = nullptr;
    s_title = nullptr;
    s_subtitle = nullptr;
    s_qr_img = nullptr;
    s_code_label = nullptr;
    s_btn_primary = nullptr;
    s_btn_primary_label = nullptr;
    s_btn_power = nullptr;
    s_btn_power_label = nullptr;
    s_btn_down = nullptr;
    s_btn_up = nullptr;
    s_temp_label = nullptr;
    s_mode_list = nullptr;
    s_brightness = nullptr;
    s_lang_btn = nullptr;
    s_hint = nullptr;
}

void ui_update(void) {}

void ui_set_language_english(bool english)
{
    s_english.store(english);
}

bool ui_is_language_english(void)
{
    return s_english.load();
}
