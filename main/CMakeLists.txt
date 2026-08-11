# List UI sources explicitly. ESP-IDF SRC_DIRS only re-globs at CMake configure
# time, so a newly added ui_font_*.c is missed by incremental `idf.py build`
# and links fail with undefined references (e.g. ui_font_cn_28).
set(UI_SRCS
    "ui/ui.cpp"
    "ui/qrcode.c"
    "ui/ui_font_cn_16.c"
    "ui/ui_font_cn_20.c"
    "ui/ui_font_cn_28.c"
)

idf_component_register(SRC_DIRS          "." "drivers"
                       SRCS              ${UI_SRCS}
                       PRIV_INCLUDE_DIRS  "." "drivers" "ui" "${ESP_MATTER_PATH}/examples/common/utils"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

# ricmoo/QRCode uses Apple #pragma mark; silence under -Werror.
set_source_files_properties(ui/qrcode.c PROPERTIES COMPILE_FLAGS
    "-Wno-error=unknown-pragmas -Wno-type-limits")
