#include "ui.h"
#include "ui_fonts.h"
#include "ui_i18n.h"
#include "qrcode.h"

#include "board_pins.h"
#include "display_gc9a01.h"
#include "ws2812_temp_light.h"
#include "app_settings.h"

#include <app_priv.h>

#include <esp_log.h>
#include <esp_system.h>
#include <esp_lvgl_port.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <setup_payload/OnboardingCodesUtil.h>
#include <app/server/Server.h>

#include <atomic>
#include <cstdio>
#include <cstring>

static const char *TAG = "ui";

enum class ui_screen_t : uint8_t {
    PAIRING = 0,
    PAIRING_BUSY,
    PAIRING_FAIL,
    LEARN,
    AC,
    LIGHT,
    SETTINGS,
};

static std::atomic<bool> s_english{false};
static std::atomic<bool> s_ready{false};
static std::atomic<bool> s_stop_task{false};
static std::atomic<bool> s_pairing_busy{false};
static std::atomic<bool> s_pairing_fail{false};
static bool s_ui_backlight_hold = false;
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
static lv_obj_t *s_btn_home_mode = nullptr;
static lv_obj_t *s_btn_home_mode_label = nullptr;
static lv_obj_t *s_btn_settings_apply = nullptr;
static lv_obj_t *s_btn_settings_apply_label = nullptr;

/* Pending selection on Settings; applied on confirm + reboot. */
static app_home_display_mode_t s_home_mode_pending =
    APP_HOME_DISPLAY_COMBINED;
static std::atomic<bool> s_settings_rebooting{false};
static TickType_t s_reboot_at_tick = 0;

/*
 * Matter MT: payloads fit QR v4 (33 modules). Scale 3 + 2-module quiet zone
 * → 111 px: fills the mid-screen “green box” band while keeping title / hint /
 * centered pairing code fully visible on the 1.28" round panel.
 */
static constexpr int kQrVersion = 4;
static constexpr int kQrScale = 3;
static constexpr int kQrQuiet = 2; /* white modules around the symbol */
static constexpr int kQrMaxPx = (4 * kQrVersion + 17 + 2 * kQrQuiet) * kQrScale;
static constexpr int kQrStrideBytes = (kQrMaxPx + 7) / 8;
/* 2 x lv_color32_t palette + 1bpp bitmap */
static constexpr int kQrImgBytes = 8 + kQrStrideBytes * kQrMaxPx;

static uint8_t s_qr_img_data[kQrImgBytes];
#if LVGL_VERSION_MAJOR >= 9
static lv_image_dsc_t s_qr_dsc = {};
#else
static lv_img_dsc_t s_qr_dsc = {};
#endif
static uint8_t s_qr_modules[256];
static char s_qr_text[160] = {};
static char s_manual_code[32] = {};

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

/* Shared palette for the round UI (matches user-manual mock figures). */
static constexpr uint32_t kColBg = 0x05070A;
static constexpr uint32_t kColTitle = 0xF2F7FA;
static constexpr uint32_t kColMuted = 0x8FA3B3;
static constexpr uint32_t kColBody = 0xD5E2EC;
static constexpr uint32_t kColAccent = 0x2F6FED;
static constexpr uint32_t kColOk = 0x1F8A5F;
static constexpr uint32_t kColCool = 0x2B4C7E;
static constexpr uint32_t kColHeat = 0x8B3A3A;
static constexpr uint32_t kColChip = 0x243447;
#if 0 /* English toggle disabled */
static constexpr uint32_t kColLang = 0x3A4A58;
#endif

static void style_circle_screen(lv_obj_t *obj)
{
    lv_obj_set_size(obj, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    /* Near-black base: high QR contrast and consistent across all pages. */
    lv_obj_set_style_bg_color(obj, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_grad_dir(obj, LV_GRAD_DIR_NONE, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_clip_corner(obj, true, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

static void style_btn(lv_obj_t *btn, uint32_t color)
{
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_pad_hor(btn, 8, 0);
    lv_obj_set_style_pad_ver(btn, 4, 0);
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
    const int px = (size + 2 * kQrQuiet) * kQrScale;
    if (px <= 0 || px > kQrMaxPx) {
        ESP_LOGW(TAG, "QR too large for buffer: %d (max %d)", px, kQrMaxPx);
        return;
    }

    const int stride = (px + 7) / 8;
    uint8_t *palette = s_qr_img_data;
    uint8_t *bitmap = s_qr_img_data + 8;

    /* LVGL indexed palette: lv_color32_t {B,G,R,A}. Index 0 = white. */
    palette[0] = 0xFF;
    palette[1] = 0xFF;
    palette[2] = 0xFF;
    palette[3] = 0xFF;
    palette[4] = 0x00;
    palette[5] = 0x00;
    palette[6] = 0x00;
    palette[7] = 0xFF;

    std::memset(bitmap, 0x00, static_cast<size_t>(stride * px)); /* all white */

    auto set_black = [&](int x, int y) {
        const int byte_index = y * stride + (x >> 3);
        bitmap[byte_index] |= static_cast<uint8_t>(0x80 >> (x & 7));
    };

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!qrcode_getModule(&qrcode, static_cast<uint8_t>(x),
                                  static_cast<uint8_t>(y))) {
                continue;
            }
            const int x0 = (x + kQrQuiet) * kQrScale;
            const int y0 = (y + kQrQuiet) * kQrScale;
            for (int dy = 0; dy < kQrScale; ++dy) {
                for (int dx = 0; dx < kQrScale; ++dx) {
                    set_black(x0 + dx, y0 + dy);
                }
            }
        }
    }

#if LVGL_VERSION_MAJOR >= 9
    s_qr_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_qr_dsc.header.cf = LV_COLOR_FORMAT_I1;
    s_qr_dsc.header.w = px;
    s_qr_dsc.header.h = px;
    s_qr_dsc.header.stride = stride;
    s_qr_dsc.data_size = static_cast<uint32_t>(8 + stride * px);
    s_qr_dsc.data = s_qr_img_data;
#else
    s_qr_dsc.header.always_zero = 0;
    s_qr_dsc.header.w = px;
    s_qr_dsc.header.h = px;
    s_qr_dsc.header.cf = LV_IMG_CF_INDEXED_1BIT;
    s_qr_dsc.data_size = 8 + stride * px;
    s_qr_dsc.data = s_qr_img_data;
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
    hide(s_btn_home_mode);
    hide(s_btn_settings_apply);
}

static void place_lang_btn(void)
{
    if (!s_lang_btn) {
        return;
    }
    /* English toggle temporarily disabled — Chinese-only UI. */
#if 0
    /*
     * Round 240px: inset from the rim so EN/中文 is not clipped by clip_corner.
     * Pairing page overrides to TOP_LEFT so it does not cover 「网」.
     */
    const bool english = s_english.load();
    lv_obj_set_size(s_lang_btn, english ? 44 : 34, 24);
    lv_obj_align(s_lang_btn, LV_ALIGN_TOP_RIGHT, -30, 18);
    lv_obj_clear_flag(s_lang_btn, LV_OBJ_FLAG_HIDDEN);
#else
    lv_obj_add_flag(s_lang_btn, LV_OBJ_FLAG_HIDDEN);
#endif
}

static void show_pairing(void)
{
    const ui_strings_t *s = ui_strings(s_english.load());
    hide_all_controls();
    refresh_onboarding_codes();
    draw_qr(s_qr_text);

    /*
     * Annotated photo fixes:
     *  - EN was clipping 「网」 on the top-right → put EN top-left (inset).
     *  - Title / 配对码 stay centered.
     *  - QR enlarged to 111px to fill the mid-screen band (green box).
     */
    lv_obj_set_style_text_font(s_title, &ui_font_cn_18, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(kColTitle), 0);
    lv_obj_set_width(s_title, 130);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_title, s->pairing_title);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 8, 14);

    lv_obj_clear_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_subtitle, &ui_font_cn_18, 0);
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(kColMuted), 0);
    lv_obj_set_width(s_subtitle, 168);
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_subtitle, s->pairing_hint);
    lv_obj_align(s_subtitle, LV_ALIGN_TOP_MID, 0, 38);

    /* Nudge QR up a bit to leave room for the 2.4G Wi-Fi tip below. */
    lv_obj_align(s_qr_img, LV_ALIGN_CENTER, 0, -8);
    lv_obj_clear_flag(s_qr_img, LV_OBJ_FLAG_HIDDEN);

    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_hint, &ui_font_cn_18, 0);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(kColBody), 0);
    lv_obj_set_width(s_hint, 180);
    lv_label_set_long_mode(s_hint, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_hint, s->pairing_wifi_hint);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -52);

    lv_obj_set_style_text_font(s_code_label, &ui_font_cn_18, 0);
    lv_obj_set_style_text_color(s_code_label, lv_color_hex(kColBody), 0);
    lv_obj_set_width(s_code_label, 180);
    lv_label_set_long_mode(s_code_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_code_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_code_label, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_clear_flag(s_code_label, LV_OBJ_FLAG_HIDDEN);

#if 0 /* English toggle temporarily disabled */
    /* Top-left inset: avoids fighting 「网」 and round-edge clip on the right. */
    if (s_lang_btn) {
        const bool english = s_english.load();
        lv_obj_set_size(s_lang_btn, english ? 44 : 34, 24);
        lv_obj_align(s_lang_btn, LV_ALIGN_TOP_LEFT, 26, 14);
        lv_obj_clear_flag(s_lang_btn, LV_OBJ_FLAG_HIDDEN);
    }
#else
    if (s_lang_btn) {
        lv_obj_add_flag(s_lang_btn, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    char line[64];
    std::snprintf(line, sizeof(line), "%s\n%s", s->manual_code, s_manual_code);
    lv_label_set_text(s_code_label, line);
}

static void show_pairing_busy(void)
{
    /* Use the language selected on the pairing screen (EN / 中文). */
    const bool english = s_english.load();
    const ui_strings_t *s = ui_strings(english);
    hide_all_controls();
    if (s_lang_btn) {
        lv_obj_add_flag(s_lang_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_font(s_title, &ui_font_cn_22, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(kColTitle), 0);
    lv_obj_set_width(s_title, 180);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -18);
    lv_label_set_text(s_title, s->pairing_busy_title);
    lv_obj_clear_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_subtitle, &ui_font_cn_18, 0);
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(kColMuted), 0);
    lv_obj_set_width(s_subtitle, 180);
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_subtitle, LV_ALIGN_CENTER, 0, 18);
    lv_label_set_text(s_subtitle, s->pairing_busy_hint);
    if (s_qr_img) {
#if LVGL_VERSION_MAJOR >= 9
        lv_image_set_src(s_qr_img, nullptr);
#else
        lv_img_set_src(s_qr_img, nullptr);
#endif
    }
}

static void show_pairing_fail(void)
{
    const bool english = s_english.load();
    const ui_strings_t *s = ui_strings(english);
    hide_all_controls();
    if (s_lang_btn) {
        lv_obj_add_flag(s_lang_btn, LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_set_style_text_font(s_title, &ui_font_cn_22, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(kColTitle), 0);
    lv_obj_set_width(s_title, 180);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, -28);
    lv_label_set_text(s_title, s->pairing_fail_title);

    lv_obj_clear_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_subtitle, &ui_font_cn_18, 0);
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(kColBody), 0);
    lv_obj_set_width(s_subtitle, 190);
    lv_label_set_long_mode(s_subtitle, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_subtitle, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_subtitle, LV_ALIGN_CENTER, 0, 12);
    lv_label_set_text(s_subtitle, s->pairing_fail_hint);
    /* QR already hidden by hide_all_controls(); do not clear img src. */
}

static void restore_default_chrome(void)
{
    lv_obj_set_style_text_font(s_title, &ui_font_cn_22, 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(kColTitle), 0);
    /* Keep titles inside the round safe chord; leave room for EN/中文. */
    lv_obj_set_width(s_title, 130);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_CLIP);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, -14, 18);
    lv_obj_clear_flag(s_subtitle, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_text_font(s_subtitle, &ui_font_cn_18, 0);
    lv_obj_set_style_text_color(s_subtitle, lv_color_hex(kColMuted), 0);
    lv_obj_set_width(s_subtitle, 168);
    lv_obj_align(s_subtitle, LV_ALIGN_TOP_MID, 0, 46);
    place_lang_btn();
    lv_obj_set_style_text_color(s_code_label, lv_color_hex(kColBody), 0);
    lv_obj_align(s_code_label, LV_ALIGN_BOTTOM_MID, 0, -16);
    lv_obj_align(s_qr_img, LV_ALIGN_CENTER, 0, 10);
}

static void show_learn(void)
{
    const ui_strings_t *s = ui_strings(s_english.load());
    hide_all_controls();
    restore_default_chrome();
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, -8, 48);
    lv_obj_align(s_subtitle, LV_ALIGN_TOP_MID, 0, 84);
    lv_label_set_text(s_title, s->learn_title);
    lv_label_set_text(s_subtitle, s->learn_hint);
    lv_obj_align(s_btn_primary, LV_ALIGN_CENTER, 0, 42);
    lv_obj_clear_flag(s_btn_primary, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_btn_primary_label,
                      app_driver_ir_is_pairing() ? "..." : s->learn_btn);
}

static void show_ac(void)
{
    const ui_strings_t *s = ui_strings(s_english.load());
    hide_all_controls();
    restore_default_chrome();

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
    restore_default_chrome();
    lv_label_set_text(s_title, s->light_title);
    lv_label_set_text(s_subtitle, s->swipe_hint_settings);
    lv_obj_clear_flag(s_mode_list, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_brightness, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_hint, s->brightness);

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

static void refresh_settings_mode_button(void)
{
    const ui_strings_t *s = ui_strings(s_english.load());
    if (!s_btn_home_mode_label) {
        return;
    }
    const bool separate =
        s_home_mode_pending == APP_HOME_DISPLAY_SEPARATE;
    lv_label_set_text(
        s_btn_home_mode_label,
        separate ? s->home_mode_separate : s->home_mode_combined);
    style_btn(s_btn_home_mode, separate ? 0x8B5A2B : kColAccent);
}

static void show_settings(void)
{
    const ui_strings_t *s = ui_strings(s_english.load());
    hide_all_controls();
    restore_default_chrome();

    if (!s_settings_rebooting.load()) {
        s_home_mode_pending = app_settings_get_home_display_mode();
    }

    lv_label_set_text(s_title, s->settings_title);
    lv_label_set_text(
        s_subtitle,
        s_settings_rebooting.load() ? s->settings_rebooting
                                    : s->settings_subtitle);
    lv_obj_clear_flag(s_btn_home_mode, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_btn_settings_apply, LV_OBJ_FLAG_HIDDEN);
    lv_obj_clear_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(
        s_hint,
        s_settings_rebooting.load() ? s->settings_rebooting : s->settings_hint);
    lv_label_set_text(s_btn_settings_apply_label, s->settings_apply);
    refresh_settings_mode_button();

    if (s_settings_rebooting.load()) {
        lv_obj_add_state(s_btn_home_mode, LV_STATE_DISABLED);
        lv_obj_add_state(s_btn_settings_apply, LV_STATE_DISABLED);
    } else {
        lv_obj_clear_state(s_btn_home_mode, LV_STATE_DISABLED);
        lv_obj_clear_state(s_btn_settings_apply, LV_STATE_DISABLED);
    }
}

static void sync_backlight_hold_for_screen(ui_screen_t screen)
{
    const bool want_hold = (screen == ui_screen_t::PAIRING ||
                            screen == ui_screen_t::PAIRING_BUSY ||
                            screen == ui_screen_t::PAIRING_FAIL ||
                            screen == ui_screen_t::LEARN);
    if (want_hold == s_ui_backlight_hold) {
        return;
    }
    display_set_idle_hold(want_hold);
    s_ui_backlight_hold = want_hold;
}

/* Force absolute hold state (used after ui_init / PASE restore). */
static void apply_backlight_hold_for_screen(ui_screen_t screen)
{
    const bool want_hold = (screen == ui_screen_t::PAIRING ||
                            screen == ui_screen_t::PAIRING_BUSY ||
                            screen == ui_screen_t::PAIRING_FAIL ||
                            screen == ui_screen_t::LEARN);
    display_set_idle_hold(want_hold);
    s_ui_backlight_hold = want_hold;
}

static void apply_screen(ui_screen_t screen)
{
    s_screen = screen;
    switch (screen) {
    case ui_screen_t::PAIRING:
        show_pairing();
        break;
    case ui_screen_t::PAIRING_BUSY:
        show_pairing_busy();
        break;
    case ui_screen_t::PAIRING_FAIL:
        show_pairing_fail();
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
    case ui_screen_t::SETTINGS:
        show_settings();
        break;
    }

    /*
     * During first ui_init(), defer backlight until after lv_refr_now() so the
     * GC9A01 never lights random GRAM (power-on snow). Later updates sync hold.
     */
    if (s_ready.load()) {
        sync_backlight_hold_for_screen(screen);
    }

    const ui_strings_t *s = ui_strings(s_english.load());
    if (s_lang_btn) {
        lv_obj_t *lbl = lv_obj_get_child(s_lang_btn, 0);
        if (lbl) {
            lv_label_set_text(lbl, s->lang_toggle);
        }
        place_lang_btn();
    }
}

#if 0 /* English toggle disabled */
static void on_lang(lv_event_t *e)
{
    (void)e;
    display_activity_notify();
    s_english.store(!s_english.load());
    apply_screen(s_screen);
}
#endif

static void on_learn(lv_event_t *e)
{
    (void)e;
    display_activity_notify();
    app_driver_ir_start_learn();
    ws2812_temp_light_set_learn_active(true);
    apply_screen(ui_screen_t::LEARN);
}

static void on_power(lv_event_t *e)
{
    (void)e;
    display_activity_notify();
    app_driver_ui_toggle_power();
    apply_screen(ui_screen_t::AC);
}

static void on_temp_down(lv_event_t *e)
{
    (void)e;
    display_activity_notify();
    app_driver_ui_adjust_temp(-1);
    apply_screen(ui_screen_t::AC);
}

static void on_temp_up(lv_event_t *e)
{
    (void)e;
    display_activity_notify();
    app_driver_ui_adjust_temp(1);
    apply_screen(ui_screen_t::AC);
}

static void on_mode(lv_event_t *e)
{
    display_activity_notify();
    const uintptr_t mode = reinterpret_cast<uintptr_t>(lv_event_get_user_data(e));
    ws2812_temp_light_set_mode(static_cast<ws2812_light_mode_t>(mode));
}

static void on_brightness(lv_event_t *e)
{
    display_activity_notify();
    lv_obj_t *slider = static_cast<lv_obj_t *>(lv_event_get_target(e));
    const uint8_t level = static_cast<uint8_t>(lv_slider_get_value(slider));
    ws2812_temp_light_set_brightness(level);
    app_driver_ui_set_light_brightness(level);
}

static void on_home_mode_toggle(lv_event_t *e)
{
    (void)e;
    if (s_settings_rebooting.load()) {
        return;
    }
    display_activity_notify();
    s_home_mode_pending =
        (s_home_mode_pending == APP_HOME_DISPLAY_SEPARATE)
            ? APP_HOME_DISPLAY_COMBINED
            : APP_HOME_DISPLAY_SEPARATE;
    refresh_settings_mode_button();
}

static void on_settings_apply(lv_event_t *e)
{
    (void)e;
    if (s_settings_rebooting.load()) {
        return;
    }
    display_activity_notify();

    const app_home_display_mode_t current =
        app_settings_get_home_display_mode();
    if (s_home_mode_pending == current) {
        apply_screen(ui_screen_t::LIGHT);
        return;
    }

    if (app_settings_set_home_display_mode(s_home_mode_pending) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save home display mode");
        return;
    }

    /* ui_task paints "Rebooting..." then calls esp_restart(). */
    s_reboot_at_tick = xTaskGetTickCount() + pdMS_TO_TICKS(800);
    s_settings_rebooting.store(true);
    apply_screen(ui_screen_t::SETTINGS);
}

static void on_gesture(lv_event_t *e)
{
    (void)e;
    if (s_settings_rebooting.load()) {
        return;
    }
    display_activity_notify();
#if LVGL_VERSION_MAJOR >= 9
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
#else
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_get_act());
#endif
    if (s_screen == ui_screen_t::AC && dir == LV_DIR_LEFT) {
        apply_screen(ui_screen_t::LIGHT);
    } else if (s_screen == ui_screen_t::LIGHT && dir == LV_DIR_LEFT) {
        apply_screen(ui_screen_t::SETTINGS);
    } else if (s_screen == ui_screen_t::LIGHT && dir == LV_DIR_RIGHT) {
        apply_screen(ui_screen_t::AC);
    } else if (s_screen == ui_screen_t::SETTINGS && dir == LV_DIR_RIGHT) {
        apply_screen(ui_screen_t::LIGHT);
    }
}

static void build_ui(void)
{
    lv_obj_t *scr = UI_SCREEN_ACTIVE();
    lv_obj_set_style_bg_color(scr, lv_color_hex(kColBg), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);

    s_root = lv_obj_create(scr);
    style_circle_screen(s_root);
    lv_obj_center(s_root);
    lv_obj_add_event_cb(s_root, on_gesture, LV_EVENT_GESTURE, nullptr);
    lv_obj_clear_flag(s_root, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_title = make_label(s_root, &ui_font_cn_22, kColTitle);
    lv_obj_set_width(s_title, 150);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, -8, 14);

    s_subtitle = make_label(s_root, &ui_font_cn_18, kColMuted);
    lv_obj_set_width(s_subtitle, 180);
    lv_obj_align(s_subtitle, LV_ALIGN_TOP_MID, 0, 40);

    s_qr_img = UI_IMAGE_CREATE(s_root);
    lv_obj_align(s_qr_img, LV_ALIGN_CENTER, 0, 10);

    s_code_label = make_label(s_root, &ui_font_cn_18, kColBody);
    lv_obj_set_width(s_code_label, 190);
    lv_label_set_long_mode(s_code_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_code_label, LV_ALIGN_BOTTOM_MID, 0, -14);

    s_btn_primary = UI_BTN_CREATE(s_root);
    style_btn(s_btn_primary, kColAccent);
    lv_obj_set_size(s_btn_primary, 148, 46);
    lv_obj_align(s_btn_primary, LV_ALIGN_CENTER, 0, 42);
    s_btn_primary_label = make_label(s_btn_primary, &ui_font_cn_18, 0xFFFFFF);
    lv_obj_center(s_btn_primary_label);
    lv_obj_add_event_cb(s_btn_primary, on_learn, LV_EVENT_CLICKED, nullptr);

    s_btn_power = UI_BTN_CREATE(s_root);
    style_btn(s_btn_power, kColOk);
    lv_obj_set_size(s_btn_power, 108, 44);
    lv_obj_align(s_btn_power, LV_ALIGN_CENTER, 0, 8);
    s_btn_power_label = make_label(s_btn_power, &ui_font_cn_18, 0xFFFFFF);
    lv_obj_center(s_btn_power_label);
    lv_obj_add_event_cb(s_btn_power, on_power, LV_EVENT_CLICKED, nullptr);

    s_btn_down = UI_BTN_CREATE(s_root);
    style_btn(s_btn_down, kColCool);
    lv_obj_set_size(s_btn_down, 86, 40);
    lv_obj_align(s_btn_down, LV_ALIGN_CENTER, -52, 58);
    lv_obj_center(make_label(s_btn_down, &ui_font_cn_18, 0xFFFFFF));
    lv_obj_add_event_cb(s_btn_down, on_temp_down, LV_EVENT_CLICKED, nullptr);

    s_btn_up = UI_BTN_CREATE(s_root);
    style_btn(s_btn_up, kColHeat);
    lv_obj_set_size(s_btn_up, 86, 40);
    lv_obj_align(s_btn_up, LV_ALIGN_CENTER, 52, 58);
    lv_obj_center(make_label(s_btn_up, &ui_font_cn_18, 0xFFFFFF));
    lv_obj_add_event_cb(s_btn_up, on_temp_up, LV_EVENT_CLICKED, nullptr);

    s_temp_label = make_label(s_root, &ui_font_cn_30, kColTitle);
    lv_obj_align(s_temp_label, LV_ALIGN_CENTER, 0, -42);

    s_hint = make_label(s_root, &ui_font_cn_18, kColMuted);
    lv_obj_align(s_hint, LV_ALIGN_BOTTOM_MID, 0, -12);

    s_mode_list = lv_obj_create(s_root);
    lv_obj_set_size(s_mode_list, 196, 118);
    lv_obj_align(s_mode_list, LV_ALIGN_CENTER, 0, 4);
    lv_obj_set_style_bg_opa(s_mode_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_mode_list, 0, 0);
    lv_obj_set_style_pad_all(s_mode_list, 0, 0);
    lv_obj_set_flex_flow(s_mode_list, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_row(s_mode_list, 6, 0);
    lv_obj_set_style_pad_column(s_mode_list, 6, 0);
    lv_obj_set_flex_align(s_mode_list, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_clear_flag(s_mode_list, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < 6; ++i) {
        lv_obj_t *b = UI_BTN_CREATE(s_mode_list);
        style_btn(b, kColChip);
        lv_obj_set_size(b, 92, 34);
        lv_obj_center(make_label(b, &ui_font_cn_18, kColBody));
        lv_obj_add_event_cb(b, on_mode, LV_EVENT_CLICKED,
                            reinterpret_cast<void *>(static_cast<uintptr_t>(i)));
    }

    s_brightness = lv_slider_create(s_root);
    lv_obj_set_width(s_brightness, 150);
    lv_slider_set_range(s_brightness, 1, 254);
    lv_obj_align(s_brightness, LV_ALIGN_BOTTOM_MID, 0, -34);
    lv_obj_set_style_bg_color(s_brightness, lv_color_hex(0x3A4A58), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_brightness, lv_color_hex(0x50A0C8), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_brightness, lv_color_hex(0xDCE6F0), LV_PART_KNOB);
    lv_obj_add_event_cb(s_brightness, on_brightness, LV_EVENT_VALUE_CHANGED, nullptr);

#if 0 /* English language toggle temporarily disabled — Chinese-only. */
    s_lang_btn = UI_BTN_CREATE(s_root);
    style_btn(s_lang_btn, kColLang);
    lv_obj_set_size(s_lang_btn, 36, 24);
    place_lang_btn();
    lv_obj_center(make_label(s_lang_btn, &ui_font_cn_18, 0xFFFFFF));
    lv_obj_add_event_cb(s_lang_btn, on_lang, LV_EVENT_CLICKED, nullptr);
#else
    s_lang_btn = nullptr;
    s_english.store(false);
#endif

    s_btn_home_mode = UI_BTN_CREATE(s_root);
    style_btn(s_btn_home_mode, kColAccent);
    lv_obj_set_size(s_btn_home_mode, 156, 46);
    lv_obj_align(s_btn_home_mode, LV_ALIGN_CENTER, 0, -14);
    s_btn_home_mode_label =
        make_label(s_btn_home_mode, &ui_font_cn_18, 0xFFFFFF);
    lv_obj_center(s_btn_home_mode_label);
    lv_obj_add_event_cb(
        s_btn_home_mode, on_home_mode_toggle, LV_EVENT_CLICKED, nullptr);

    s_btn_settings_apply = UI_BTN_CREATE(s_root);
    style_btn(s_btn_settings_apply, kColOk);
    lv_obj_set_size(s_btn_settings_apply, 156, 42);
    lv_obj_align(s_btn_settings_apply, LV_ALIGN_CENTER, 0, 46);
    s_btn_settings_apply_label =
        make_label(s_btn_settings_apply, &ui_font_cn_18, 0xFFFFFF);
    lv_obj_center(s_btn_settings_apply_label);
    lv_obj_add_event_cb(
        s_btn_settings_apply, on_settings_apply, LV_EVENT_CLICKED, nullptr);
}

static ui_screen_t decide_screen(void)
{
    if (s_pairing_busy.load()) {
        return ui_screen_t::PAIRING_BUSY;
    }
    if (s_pairing_fail.load()) {
        return ui_screen_t::PAIRING_FAIL;
    }

    /*
     * Pairing QR is only for the uncommissioned device. Once a fabric exists,
     * never return to PAIRING — even while a second fabric is being added
     * (app_get_matter_state_locked() reports COMMISSIONING in that case).
     */
    const size_t fabric_count =
        chip::Server::GetInstance().GetFabricTable().FabricCount();
    if (fabric_count == 0) {
        return ui_screen_t::PAIRING;
    }

    if (!app_driver_ir_is_paired()) {
        return ui_screen_t::LEARN;
    }
    if (s_screen == ui_screen_t::LIGHT ||
        s_screen == ui_screen_t::SETTINGS) {
        return s_screen;
    }
    return ui_screen_t::AC;
}

static void ui_task(void *arg)
{
    (void)arg;
    ui_screen_t last = static_cast<ui_screen_t>(0xFF);
    uint32_t ticks = 0;

    while (!s_stop_task.load()) {
        if (s_settings_rebooting.load() &&
            xTaskGetTickCount() >= s_reboot_at_tick) {
            esp_restart();
        }

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
    s_ui_backlight_hold = false;

    if (!lvgl_port_lock(1000)) {
        ESP_LOGE(TAG, "LVGL lock timeout during ui_init");
        return ESP_ERR_TIMEOUT;
    }

    build_ui();
    apply_screen(decide_screen());
    if (s_root) {
        lv_obj_invalidate(s_root);
    }
    /* Push the first real frame; backlight is already on (black GRAM). */
    lv_refr_now(display_get_disp());
    apply_backlight_hold_for_screen(s_screen);
    if (!display_is_backlight_on()) {
        display_set_backlight(true);
    }
    lvgl_port_unlock();

    s_stop_task.store(false);
    /* Pairing / AC refresh loops are light; keep stack modest for PASE heap. */
    if (xTaskCreate(ui_task, "ui_task", 6144, nullptr, 4, &s_task) != pdPASS) {
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

    /*
     * Freeze the language currently selected on the pairing page (via EN/中文)
     * so the busy frame matches what the user last chose.
     */
    s_pairing_fail.store(false);
    s_pairing_busy.store(true);
    const bool english = s_english.load();
    if (!lvgl_port_lock(200)) {
        s_pairing_busy.store(false);
        return ESP_ERR_TIMEOUT;
    }

    apply_screen(ui_screen_t::PAIRING_BUSY);
    std::memset(s_qr_img_data, 0, sizeof(s_qr_img_data));
    std::memset(s_qr_text, 0, sizeof(s_qr_text));
    std::memset(s_manual_code, 0, sizeof(s_manual_code));

    if (s_root) {
        lv_obj_invalidate(s_root);
    }
    lv_refr_now(display_get_disp());
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Showing commissioning-busy screen (lang=%s, title=%s)",
             english ? "en" : "zh",
             ui_strings(english)->pairing_busy_title);
    return ESP_OK;
}

esp_err_t ui_show_commissioning_failed(void)
{
    if (!s_ready.load() || !display_is_ready()) {
        return ESP_ERR_INVALID_STATE;
    }

    s_pairing_busy.store(false);
    s_pairing_fail.store(true);
    const bool english = s_english.load();
    if (!lvgl_port_lock(200)) {
        s_pairing_fail.store(false);
        return ESP_ERR_TIMEOUT;
    }

    apply_screen(ui_screen_t::PAIRING_FAIL);
    if (s_root) {
        lv_obj_invalidate(s_root);
    }
    lv_refr_now(display_get_disp());
    lvgl_port_unlock();

    ESP_LOGI(TAG, "Showing commissioning-fail tip (lang=%s, hint=%s)",
             english ? "en" : "zh",
             ui_strings(english)->pairing_fail_hint);
    return ESP_OK;
}

void ui_clear_commissioning_failed(void)
{
    if (!s_pairing_fail.exchange(false)) {
        return;
    }
    if (!s_ready.load() || !display_is_ready()) {
        return;
    }
    if (!lvgl_port_lock(200)) {
        return;
    }
    apply_screen(ui_screen_t::PAIRING);
    if (s_root) {
        lv_obj_invalidate(s_root);
    }
    lv_refr_now(display_get_disp());
    lvgl_port_unlock();
    ESP_LOGI(TAG, "Cleared commissioning-fail tip; back to pairing QR");
}

void ui_deinit(void)
{
    if (!s_ready.load() && s_task == nullptr) {
        return;
    }

    ESP_LOGI(TAG, "Stopping UI task");
    s_stop_task.store(true);
    s_pairing_busy.store(false);
    s_pairing_fail.store(false);
    s_ready.store(false);
    /*
     * Keep backlight held across PASE suspend (busy frame stays visible).
     * Cleared when the next ui_init()/apply_screen() runs after restore.
     */
    if (!s_ui_backlight_hold) {
        display_set_idle_hold(true);
        s_ui_backlight_hold = true;
    }

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
