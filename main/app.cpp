#include "app.hpp"
#include "bootloader_random.h"
#include "esp_log.h"
#include "esp_random.h"

namespace app {
knife::Gate gate;
std::atomic<bool> network_up{false}, sd_ready{false}, transfer_cancel{false}, web_ready{false},
    ssh_ready{false}, telnet_ready{false};
std::atomic<size_t> transfer_done{0}, transfer_size{0};
std::string boot_id;
Config running;
std::string random_hex(size_t bytes) {
    std::vector<uint8_t> b(bytes);
    esp_fill_random(b.data(), b.size());
    std::string out;
    out.reserve(bytes * 2);
    for (auto c : b) {
        out += "0123456789abcdef"[c >> 4];
        out += "0123456789abcdef"[c & 15];
    }
    return out;
}
std::string json_text(cJSON* j) {
    char* s = cJSON_PrintUnformatted(j);
    std::string r = s ? s : "{}";
    cJSON_free(s);
    return r;
}
std::string field(cJSON* j, const char* k, const std::string& fallback) {
    auto v = cJSON_GetObjectItemCaseSensitive(j, k);
    return cJSON_IsString(v) ? std::string(v->valuestring) : fallback;
}
} // namespace app
extern "C" void app_main() {
    // No radio/ADC/I2S is used: retain the internal entropy source for TLS/SSH.
    bootloader_random_enable();
    app::boot_id = app::random_hex(8);
    app::config_init();
    app::running = app::config();
    app::identity_init();
    app::serial_start();
    app::storage_start();
    app::network_start();
    app::web_start();
    app::console_start();
    app::system_start();
    ESP_LOGI("app", "Prototype started; hardware validation required");
}
