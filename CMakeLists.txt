cmake_minimum_required(VERSION 3.16)
set(IDF_TARGET esp32s3)
set(COMPONENTS main)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_signing.py"
    RESULT_VARIABLE signing_result)
if(NOT signing_result EQUAL 0)
    message(FATAL_ERROR "Cannot validate the project OTA signing key")
endif()
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_tls.py"
    RESULT_VARIABLE tls_result)
if(NOT tls_result EQUAL 0)
    message(FATAL_ERROR "Cannot prepare the fixed HTTPS identity")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_tls.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/.tls/https_cert.pem"
    "${CMAKE_CURRENT_SOURCE_DIR}/.tls/https_key.pem")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_version.py"
    --idf-version OUTPUT_VARIABLE PROJECT_VER OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE version_result)
if(NOT version_result EQUAL 0)
    message(FATAL_ERROR "Cannot determine build version from Git")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(eth2serial)
if(NOT CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT OR NOT CONFIG_SECURE_SIGNED_APPS_RSA_SCHEME
    OR NOT CONFIG_SECURE_BOOT_BUILD_SIGNED_BINARIES OR NOT CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE)
    message(FATAL_ERROR "OTA requires RSA app signing, signature verification and bootloader rollback; apply sdkconfig.defaults")
endif()
if(CONFIG_SECURE_BOOT OR CONFIG_SECURE_FLASH_ENC_ENABLED OR CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK)
    message(FATAL_ERROR "This project does not provision irreversible eFuses")
endif()
if(NOT CONFIG_SECURE_BOOT_SIGNING_KEY STREQUAL ".signing/ota-key.pem")
    message(FATAL_ERROR "Use the validated project OTA signing key .signing/ota-key.pem")
endif()
add_custom_target(knife_signing_check
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/prepare_signing.py"
    VERBATIM)

# Git refs/tags can change without a CMake reconfigure. Refresh the identity on
# every build; only rewrite the header when the identity actually changes.
set(version_header "${CMAKE_BINARY_DIR}/generated/build_version.h")
add_custom_target(knife_build_version
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/build_version.py"
        --header "${version_header}"
    BYPRODUCTS "${version_header}"
    VERBATIM)
idf_component_get_property(main_lib main COMPONENT_LIB)
target_include_directories(${main_lib} PRIVATE "${CMAKE_BINARY_DIR}/generated")
add_dependencies(${main_lib} knife_build_version)
add_dependencies(${main_lib} knife_signing_check)
# Keep the image's app descriptor in sync with the full version shown in the UI.
idf_component_get_property(app_format_lib esp_app_format COMPONENT_LIB)
target_compile_definitions(${app_format_lib} PRIVATE KNIFE_OVERRIDE_APP_VERSION)
target_compile_options(${app_format_lib} PRIVATE "-include" "${version_header}")
add_dependencies(${app_format_lib} knife_build_version)
