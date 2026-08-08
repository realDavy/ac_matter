set(IRREMOTE_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../deps/IRremoteESP8266/src")
set(IRREMOTE_TEST_DIR "${CMAKE_CURRENT_LIST_DIR}/../../deps/IRremoteESP8266/test")

set(IRREMOTE_SRCS "")
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/ir_sources.list" IRREMOTE_SRC_NAMES)
foreach(src_name ${IRREMOTE_SRC_NAMES})
    list(APPEND IRREMOTE_SRCS "${IRREMOTE_SRC_DIR}/${src_name}")
endforeach()

idf_component_register(
    SRCS
        "rmt_ir.cpp"
        "ir_ac_controller.cpp"
        ${IRREMOTE_SRCS}
    INCLUDE_DIRS
        "include"
        "arduino_compat"
        "${IRREMOTE_SRC_DIR}"
        # UNIT_TEST builds need IRsend_test.h (and friends) from the library test/
        "${IRREMOTE_TEST_DIR}"
    REQUIRES
        driver
        esp_timer
        esp_rom
)

# Build IRremoteESP8266 in host/unit-test mode so encode/decode work without
# Arduino GPIO interrupts. SWIGLIB redirects mark/space into timingList for RMT.
#
# Only enable protocols we actually compile (see ir_protocol_enables.h +
# ir_sources.list). Leaving _IR_ENABLE_DEFAULT_=true pulls in TV/AV protocols
# whose .cpp files are not in the build and causes link failures.
target_compile_definitions(${COMPONENT_LIB} PUBLIC
    UNIT_TEST
    SWIGLIB
)

target_compile_options(${COMPONENT_LIB} PUBLIC
    "-include${CMAKE_CURRENT_LIST_DIR}/ir_protocol_enables.h"
)

target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=switch
    -Wno-switch
    -Wno-unused-function
    -Wno-unused-variable
    -Wno-unused-but-set-variable
    -Wno-missing-field-initializers
    -Wno-format
)

# The vendored library is C++ and expects C++17 features already enabled by
# the Matter project. Keep warnings from flooding the Matter build.
set_source_files_properties(${IRREMOTE_SRCS} PROPERTIES COMPILE_FLAGS
    "-Wno-error -Wno-switch -Wno-unused-parameter"
)
