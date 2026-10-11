#pragma once
#include "cJSON.h"
#include "core.hpp"
#include "esp_err.h"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace app {
struct Config {
    std::string hostname = "serial-dongle", ip = "192.168.1.50", mask = "255.255.255.0",
                gateway = "192.168.1.1", dns = "192.168.1.1";
    bool dhcp = true, telnet = false, ssh = true;
    int vlan = 0, web_port = 443, ssh_port = 22, telnet_port = 23;
    int baud = 115200, data_bits = 8, stop_bits = 1;
    std::string parity = "none";
    bool smtp = false;
    std::string smtp_host, mail_from, mail_to;
    int smtp_port = 25;
};
struct User {
    std::string name, salt, hash, public_key;
    bool enabled = true;
};
struct Json {
    cJSON* p;
    explicit Json(cJSON* v = nullptr) : p(v) {}
    ~Json() {
        cJSON_Delete(p);
    }
    Json(const Json&) = delete;
};
extern knife::Gate gate;
extern std::atomic<bool> network_up, sd_ready, transfer_cancel, web_ready, ssh_ready, telnet_ready;
extern std::atomic<size_t> transfer_done, transfer_size;
extern std::string boot_id;
extern Config running;
std::string random_hex(size_t bytes);
std::string json_text(cJSON* json);
std::string field(cJSON* json, const char* key, const std::string& fallback = "");
Config config();
void config_init();
cJSON* settings_json();
bool save_settings(cJSON*, std::string& error);
cJSON* users_json();
bool save_user(cJSON*, bool remove, std::string& error);
bool authenticate(const std::string& name, const std::string& password);
bool authenticate_key(const std::string& name, std::span<const uint8_t> key);
bool user_enabled(const std::string& name);
void invalidate_sessions();
bool auth_allowed();
void auth_failed();
std::string secret_read(const char* key);
bool secret_write(const char* key, const std::string& value);
bool tls_import(const std::string& cert, const std::string& key, std::string& error);
void identity_init();
bool factory_reset();
void network_start();
std::string ip_address();
void serial_start();
void console_start();
void storage_start();
bool storage_format(std::string& error); // caller owns Card
cJSON* files_json();                     // caller owns Card
bool transfer_start(const std::string& name, bool ymodem, std::string& error);
std::string transfer_result();
bool format_start(std::string& error);
std::string storage_result();
void web_start();
void system_start();
bool schedule_restart();
} // namespace app
