set(ROOT "${CMAKE_CURRENT_LIST_DIR}/../../third_party/wolfssl")
file(GLOB SOURCES "${ROOT}/wolfcrypt/src/*.c")
list(FILTER SOURCES EXCLUDE REGEX "/(misc|evp|evp_pk|asn_orig|asn_tsp)\\.c$")
idf_component_register(SRCS ${SOURCES} "memory.c" INCLUDE_DIRS "${ROOT}" "include"
    "$ENV{IDF_PATH}/components/freertos/FreeRTOS-Kernel/include/freertos"
    REQUIRES freertos esp_hw_support esp_timer lwip)
target_compile_definitions(${COMPONENT_LIB} PUBLIC WOLFSSL_USER_SETTINGS)
# Third-party optional-algorithm translation units may be empty.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error)
