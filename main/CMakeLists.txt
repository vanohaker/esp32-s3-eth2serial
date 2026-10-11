idf_component_register(SRCS "app.cpp" "config.cpp" "network.cpp" "storage.cpp"
    "console.cpp" "web.cpp" "system.cpp"
    INCLUDE_DIRS "."
    REQUIRES knife_core wolfssh nvs_flash esp_eth esp_netif esp_event esp_driver_gpio
    esp_driver_uart esp_driver_spi fatfs sdmmc esp_https_server json mbedtls
    esp_timer bootloader_support esp_hw_support
    EMBED_TXTFILES "web/index.html" "web/app.js" "web/style.css")
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wall -Wextra -Wno-error=cpp)
