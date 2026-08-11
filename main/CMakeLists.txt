# ESP-IDF ignores SRC_DIRS when SRCS is also set, so list every source here.
# New ui_font_*.c files must be added to this list (SRC_DIRS alone would miss
# them until a full CMake reconfigure after a plain git pull).
set(COMPONENT_SRCS
    "app_main.cpp"
    "app_driver.cpp"
    "app_settings.cpp"
    "board_i2c.cpp"
    "drivers/display_gc9a01.cpp"
    "drivers/it7259.cpp"
    "drivers/sht30.cpp"
    "drivers/ws2812_temp_light.cpp"
    "ui/ui.cpp"
    "ui/qrcode.c"
    "ui/ui_font_cn_16.c"
    "ui/ui_font_cn_20.c"
    "ui/ui_font_cn_28.c"
)

idf_component_register(SRCS              ${COMPONENT_SRCS}
                       PRIV_INCLUDE_DIRS  "." "drivers" "ui" "${ESP_MATTER_PATH}/examples/common/utils"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

# ricmoo/QRCode uses Apple #pragma mark; silence under -Werror.
set_source_files_properties(ui/qrcode.c PROPERTIES COMPILE_FLAGS
    "-Wno-error=unknown-pragmas -Wno-type-limits")
