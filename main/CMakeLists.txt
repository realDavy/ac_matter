idf_component_register(SRC_DIRS          "." "drivers" "ui"
                       PRIV_INCLUDE_DIRS  "." "drivers" "ui" "${ESP_MATTER_PATH}/examples/common/utils"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

# Shorten Matter ScanNetworks (CHIP calls esp_wifi_scan_start(nullptr)).
target_link_libraries(${COMPONENT_LIB} INTERFACE "-Wl,--wrap=esp_wifi_scan_start")

# ricmoo/QRCode uses Apple #pragma mark; silence under -Werror.
set_source_files_properties(ui/qrcode.c PROPERTIES COMPILE_FLAGS
    "-Wno-error=unknown-pragmas -Wno-type-limits")
