set(BC7215_LIB_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../deps/bc7215_ac_lib"
)

idf_component_register(
    SRCS
        "${BC7215_LIB_DIR}/examples/ESP-IDF/components/bc7215/bc7215.cpp"
        "${BC7215_LIB_DIR}/examples/ESP-IDF/components/bc7215ac/bc7215ac.cpp"

        "${BC7215_LIB_DIR}/bc7215_ac_lib/bc7215_lib.c"
        "${BC7215_LIB_DIR}/bc7215_ac_lib/bc7215_ac_lib.c"

    INCLUDE_DIRS
        "${BC7215_LIB_DIR}/examples/ESP-IDF/components/bc7215/include"
        "${BC7215_LIB_DIR}/examples/ESP-IDF/components/bc7215ac/include"
        "${BC7215_LIB_DIR}/bc7215_ac_lib"
        "${BC7215_LIB_DIR}/examples/ESP-IDF/main/include"

    REQUIRES
        driver
        esp_timer
        nvs_flash
)
