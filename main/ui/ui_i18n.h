#pragma once

#include <stdbool.h>

typedef struct {
    const char *pairing_title;
    const char *pairing_hint;
    const char *pairing_wifi_hint;
    const char *pairing_busy_title;
    const char *pairing_busy_hint;
    const char *manual_code;
    const char *learn_title;
    const char *learn_hint;
    const char *learn_btn;
    const char *ac_title;
    const char *power_on;
    const char *power_off;
    const char *cool_down;
    const char *heat_up;
    const char *light_page;
    const char *light_title;
    const char *mode_night;
    const char *mode_manual;
    const char *mode_temp;
    const char *mode_solid;
    const char *mode_rainbow;
    const char *mode_white;
    const char *brightness;
    const char *swipe_hint;
    const char *swipe_hint_settings;
    const char *settings_title;
    const char *settings_subtitle;
    const char *home_mode_combined;
    const char *home_mode_separate;
    const char *settings_apply;
    const char *settings_hint;
    const char *settings_rebooting;
    const char *lang_toggle;
} ui_strings_t;

static inline const ui_strings_t *ui_strings(bool english)
{
    static const ui_strings_t zh = {
        .pairing_title = "Matter 配网",
        .pairing_hint = "请扫码或输入配对码",
        .pairing_wifi_hint = "请用2.4G网",
        .pairing_busy_title = "配对中...",
        .pairing_busy_hint = "请用2.4G网完成添加",
        .manual_code = "配对码",
        .learn_title = "红外学习",
        .learn_hint = "对准遥控按任意键",
        .learn_btn = "开始学习",
        .ac_title = "空调",
        .power_on = "开启",
        .power_off = "关闭",
        .cool_down = "降温",
        .heat_up = "升温",
        .light_page = "灯光",
        .light_title = "氛围灯光",
        .mode_night = "夜间关闭",
        .mode_manual = "手动亮度",
        .mode_temp = "温感呼吸",
        .mode_solid = "纯色",
        .mode_rainbow = "彩虹",
        .mode_white = "呼吸白",
        .brightness = "亮度",
        .swipe_hint = "左滑灯光",
        .swipe_hint_settings = "左滑设置",
        .settings_title = "组件设置",
        .settings_subtitle = "Home显示方式",
        .home_mode_combined = "组合显示",
        .home_mode_separate = "分开显示",
        .settings_apply = "应用并重启",
        .settings_hint = "切换后重启并需重新配网",
        .settings_rebooting = "正在重启...",
        .lang_toggle = "EN",
    };

    static const ui_strings_t en = {
        .pairing_title = "Matter Setup",
        .pairing_hint = "Scan QR or enter code",
        .pairing_wifi_hint = "Use 2.4GHz Wi-Fi",
        .pairing_busy_title = "Pairing...",
        .pairing_busy_hint = "Use 2.4GHz Wi-Fi on phone",
        .manual_code = "Setup Code",
        .learn_title = "IR Learn",
        .learn_hint = "Aim remote, press any key",
        .learn_btn = "Start Learn",
        .ac_title = "Air Conditioner",
        .power_on = "On",
        .power_off = "Off",
        .cool_down = "Cooler",
        .heat_up = "Warmer",
        .light_page = "Light",
        .light_title = "Ambient Light",
        .mode_night = "Night Off",
        .mode_manual = "Manual",
        .mode_temp = "Temp Breath",
        .mode_solid = "Solid",
        .mode_rainbow = "Rainbow",
        .mode_white = "White Breath",
        .brightness = "Brightness",
        .swipe_hint = "Swipe for light",
        .swipe_hint_settings = "Swipe for settings",
        .settings_title = "Settings",
        .settings_subtitle = "Home display mode",
        .home_mode_combined = "Combined",
        .home_mode_separate = "Separate",
        .settings_apply = "Apply & Reboot",
        .settings_hint = "Reboots; re-add in Home",
        .settings_rebooting = "Rebooting...",
        .lang_toggle = "中文",
    };

    return english ? &en : &zh;
}
