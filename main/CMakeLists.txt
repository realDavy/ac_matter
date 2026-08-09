# CONFIGURE_DEPENDS: re-scan when sources are added/removed (SRC_DIRS globs
# are easy to miss after git pull of new files like app_settings.cpp).
file(GLOB MAIN_SRCS CONFIGURE_DEPENDS "*.c" "*.cpp")
file(GLOB DRIVER_SRCS CONFIGURE_DEPENDS "drivers/*.c" "drivers/*.cpp")
file(GLOB UI_SRCS CONFIGURE_DEPENDS "ui/*.c" "ui/*.cpp")

idf_component_register(SRCS ${MAIN_SRCS} ${DRIVER_SRCS} ${UI_SRCS}
                       PRIV_INCLUDE_DIRS  "." "drivers" "ui" "${ESP_MATTER_PATH}/examples/common/utils"
)

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")
target_compile_definitions(${COMPONENT_LIB} PRIVATE LV_LVGL_H_INCLUDE_SIMPLE=1)

# ricmoo/QRCode uses Apple #pragma mark; silence under -Werror.
set_source_files_properties(ui/qrcode.c PROPERTIES COMPILE_FLAGS
    "-Wno-error=unknown-pragmas -Wno-type-limits")
