#include "app.hpp"
#include "esp_random.h"
#include "esp_timer.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "mbedtls/md.h"
#include "mbedtls/pk.h"
#include "mbedtls/pkcs5.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace app {
static std::mutex mutex;
static Config saved;
static std::vector<User> users;
static nvs_handle_t nvs;
static std::atomic<int64_t> blocked_until{0};
static constexpr int iterations = 30000;
static std::string password_hash(const std::string& password, const std::string& salt) {
    uint8_t out[32];
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(
        MBEDTLS_MD_SHA256, reinterpret_cast<const unsigned char*>(password.data()), password.size(),
        reinterpret_cast<const unsigned char*>(salt.data()), salt.size(), iterations, sizeof(out), out);
    if (rc)
        return {};
    std::string result;
    for (auto b : out) {
        result += "0123456789abcdef"[b >> 4];
        result += "0123456789abcdef"[b & 15];
    }
    return result;
}
static bool equal(std::string_view a, std::string_view b) {
    if (a.size() != b.size())
        return false;
    volatile uint8_t difference = 0;
    for (size_t i = 0; i < a.size(); ++i)
        difference = uint8_t(difference | (a[i] ^ b[i]));
    return difference == 0;
}
bool auth_allowed() {
    return esp_timer_get_time() >= blocked_until.load();
}
void auth_failed() {
    blocked_until.store(esp_timer_get_time() + 2000000);
}
std::string secret_read(const char* key) {
    size_t size = 0;
    auto rc = nvs_get_blob(nvs, key, nullptr, &size);
    if (rc == ESP_ERR_NVS_NOT_FOUND)
        return {};
    ESP_ERROR_CHECK(rc);
    ESP_ERROR_CHECK(size && size <= 65536 ? ESP_OK : ESP_ERR_INVALID_SIZE);
    std::string out(size, '\0');
    ESP_ERROR_CHECK(nvs_get_blob(nvs, key, out.data(), &size));
    out.resize(size);
    return out;
}
bool secret_write(const char* key, const std::string& value) {
    return nvs_set_blob(nvs, key, value.data(), value.size()) == ESP_OK && nvs_commit(nvs) == ESP_OK;
}
static cJSON* config_json(const Config& c) {
    auto j = cJSON_CreateObject();
#define S(k) cJSON_AddStringToObject(j, #k, c.k.c_str())
#define N(k) cJSON_AddNumberToObject(j, #k, c.k)
#define B(k) cJSON_AddBoolToObject(j, #k, c.k)
    S(hostname);
    S(ip);
    S(mask);
    S(gateway);
    S(dns);
    B(dhcp);
    B(telnet);
    B(ssh);
    B(web_tls);
    N(vlan);
    N(web_port);
    N(ssh_port);
    N(telnet_port);
    N(baud);
    N(data_bits);
    N(stop_bits);
    S(parity);
    B(smtp);
    S(smtp_host);
    S(mail_from);
    S(mail_to);
    N(smtp_port);
#undef S
#undef N
#undef B
    return j;
}
static bool read_config(cJSON* j, Config& c, std::string& error) {
    if (!cJSON_IsObject(j)) {
        error = "Expected settings object";
        return false;
    }
    bool valid = true;
    auto str = [&](const char* k, std::string& v, size_t max) {
        auto o = cJSON_GetObjectItemCaseSensitive(j, k);
        if (o) {
            if (!cJSON_IsString(o) || strlen(o->valuestring) > max)
                valid = false;
            else
                v = o->valuestring;
        }
    };
    auto num = [&](const char* k, int& v, int low, int high) {
        auto o = cJSON_GetObjectItemCaseSensitive(j, k);
        if (o) {
            if (!cJSON_IsNumber(o) || !std::isfinite(o->valuedouble) || o->valuedouble < low ||
                o->valuedouble > high || floor(o->valuedouble) != o->valuedouble)
                valid = false;
            else
                v = o->valueint;
        }
    };
    auto boolean = [&](const char* k, bool& v) {
        auto o = cJSON_GetObjectItemCaseSensitive(j, k);
        if (o) {
            if (!cJSON_IsBool(o))
                valid = false;
            else
                v = cJSON_IsTrue(o);
        }
    };
    str("hostname", c.hostname, 63);
    str("ip", c.ip, 15);
    str("mask", c.mask, 15);
    str("gateway", c.gateway, 15);
    str("dns", c.dns, 15);
    boolean("dhcp", c.dhcp);
    boolean("telnet", c.telnet);
    boolean("ssh", c.ssh);
    boolean("web_tls", c.web_tls);
    num("vlan", c.vlan, 0, 4094);
    num("web_port", c.web_port, 1, 65535);
    num("ssh_port", c.ssh_port, 1, 65535);
    num("telnet_port", c.telnet_port, 1, 65535);
    num("baud", c.baud, 1200, 115200);
    num("data_bits", c.data_bits, 5, 8);
    num("stop_bits", c.stop_bits, 1, 2);
    str("parity", c.parity, 5);
    boolean("smtp", c.smtp);
    str("smtp_host", c.smtp_host, 253);
    str("mail_from", c.mail_from, 128);
    str("mail_to", c.mail_to, 128);
    num("smtp_port", c.smtp_port, 1, 65535);
    in_addr addr{}, mask{}, gw{}, dns{};
    valid = valid && knife::hostname_valid(c.hostname) && inet_pton(AF_INET, c.ip.c_str(), &addr) == 1 &&
            inet_pton(AF_INET, c.mask.c_str(), &mask) == 1 &&
            inet_pton(AF_INET, c.gateway.c_str(), &gw) == 1 && inet_pton(AF_INET, c.dns.c_str(), &dns) == 1;
    uint32_t m = ntohl(mask.s_addr), a = ntohl(addr.s_addr), g = ntohl(gw.s_addr), inv = ~m;
    valid = valid && m != 0 && m != 0xffffffff && (inv & (inv + 1)) == 0 && (a & inv) != 0 &&
            (a & inv) != inv && ((g & m) == (a & m) || g == 0);
    valid = valid && (c.parity == "none" || c.parity == "even" || c.parity == "odd") &&
            c.web_port != c.ssh_port && c.web_port != c.telnet_port && c.ssh_port != c.telnet_port;
    if (c.smtp)
        valid = valid && !c.smtp_host.empty() &&
                std::all_of(c.smtp_host.begin(), c.smtp_host.end(),
                            [](unsigned char b) { return b > 32 && b < 127 && b != '/' && b != ':'; }) &&
                knife::email_valid(c.mail_from) && knife::email_valid(c.mail_to);
    if (!valid)
        error = "Invalid settings, IP/netmask or duplicate service ports";
    return valid;
}
static bool persist(const Config& c, const std::vector<User>& list) {
    Json root(cJSON_CreateObject());
    cJSON_AddNumberToObject(root.p, "schema", 1);
    cJSON_AddItemToObject(root.p, "settings", config_json(c));
    auto a = cJSON_AddArrayToObject(root.p, "users");
    for (const auto& u : list) {
        auto j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", u.name.c_str());
        cJSON_AddStringToObject(j, "salt", u.salt.c_str());
        cJSON_AddStringToObject(j, "hash", u.hash.c_str());
        cJSON_AddStringToObject(j, "public_key", u.public_key.c_str());
        cJSON_AddBoolToObject(j, "enabled", u.enabled);
        cJSON_AddItemToArray(a, j);
    }
    return secret_write("config", json_text(root.p));
}
Config config() {
    std::lock_guard lock(mutex);
    return saved;
}
cJSON* settings_json() {
    std::lock_guard lock(mutex);
    return config_json(saved);
}
void config_init() {
    // A corrupt/full NVS is a diagnostic failure, never an implicit factory reset.
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(nvs_open("knife", NVS_READWRITE, &nvs));
    auto raw = secret_read("config");
    if (raw.empty()) {
        User admin;
        admin.name = "admin";
        admin.salt = random_hex(16);
        admin.hash = password_hash("admin", admin.salt);
        users.push_back(admin);
        ESP_ERROR_CHECK(persist(saved, users) ? ESP_OK : ESP_FAIL);
        return;
    }
    Json root(cJSON_Parse(raw.c_str()));
    std::string error;
    auto schema = cJSON_GetObjectItemCaseSensitive(root.p, "schema");
    ESP_ERROR_CHECK(cJSON_IsNumber(schema) && schema->valueint == 1 &&
                            read_config(cJSON_GetObjectItemCaseSensitive(root.p, "settings"), saved, error)
                        ? ESP_OK
                        : ESP_FAIL);
    cJSON* item = nullptr;
    bool repaired_admin = false;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(root.p, "users")) {
        User u;
        u.name = field(item, "name");
        u.salt = field(item, "salt");
        u.hash = field(item, "hash");
        u.public_key = field(item, "public_key");
        u.enabled = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "enabled"));
        if (u.name == "admin" && !u.enabled) {
            u.enabled = true;
            repaired_admin = true;
        }
        users.push_back(u);
    }
    ESP_ERROR_CHECK(!users.empty() && users.size() <= 8 &&
                            std::any_of(users.begin(), users.end(), [](auto& u) { return u.enabled; })
                        ? ESP_OK
                        : ESP_FAIL);
    if (repaired_admin)
        ESP_ERROR_CHECK(persist(saved, users) ? ESP_OK : ESP_FAIL);
}
bool save_settings(cJSON* j, std::string& error) {
    knife::Lease lease(gate, knife::Write);
    if (!lease) {
        error = "Busy";
        return false;
    }
    std::lock_guard lock(mutex);
    auto next = saved;
    if (!read_config(j, next, error))
        return false;
    if (!persist(next, users)) {
        error = "NVS write failed";
        return false;
    }
    saved = next;
    return true;
}
cJSON* users_json() {
    std::lock_guard lock(mutex);
    auto a = cJSON_CreateArray();
    for (const auto& u : users) {
        auto j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", u.name.c_str());
        cJSON_AddBoolToObject(j, "enabled", u.enabled);
        cJSON_AddStringToObject(j, "public_key", u.public_key.c_str());
        cJSON_AddItemToArray(a, j);
    }
    return a;
}
static std::vector<uint8_t> decode_key(const std::string& key) {
    auto space = key.find(' ');
    if (space == std::string::npos)
        return {};
    auto kind = key.substr(0, space);
    if (kind != "ecdsa-sha2-nistp256" && kind != "ssh-ed25519" && kind != "ssh-rsa")
        return {};
    auto last = key.find_first_of(" \r\n", space + 1);
    auto b64 = key.substr(space + 1, last - space - 1);
    std::vector<uint8_t> result(2048);
    size_t size = 0;
    if (mbedtls_base64_decode(result.data(), result.size(), &size,
                              reinterpret_cast<const uint8_t*>(b64.data()), b64.size()))
        return {};
    result.resize(size);
    if (size < 4 + kind.size() || result[0] || result[1] || result[2] || result[3] != kind.size() ||
        memcmp(result.data() + 4, kind.data(), kind.size()))
        return {};
    return result;
}
bool save_user(cJSON* j, bool remove, std::string& error) {
    knife::Lease lease(gate, knife::Write);
    if (!lease) {
        error = "Busy";
        return false;
    }
    std::lock_guard lock(mutex);
    auto list = users;
    auto name = field(j, "name");
    if (!knife::hostname_valid(name) || name.size() > 32) {
        error = "Invalid username";
        return false;
    }
    auto it = std::find_if(list.begin(), list.end(), [&](auto& u) { return u.name == name; });
    // A username is the immutable identity. Creating and updating are explicit,
    // so changing a form field cannot silently create another account.
    if (cJSON_HasObjectItem(j, "new_name") || cJSON_HasObjectItem(j, "old_name")) {
        error = "Renaming users is not supported";
        return false;
    }
    if (name == "admin" && remove) {
        error = "Cannot delete admin";
        return false;
    }
    if (remove) {
        if (it == list.end()) {
            error = "Unknown user";
            return false;
        }
        list.erase(it);
    } else {
        auto operation = field(j, "operation");
        if (operation != "create" && operation != "update") {
            error = "Expected operation create or update";
            return false;
        }
        bool created = operation == "create";
        if (created == (it != list.end())) {
            error = created ? "User already exists" : "Unknown user";
            return false;
        }
        auto enabled = cJSON_GetObjectItemCaseSensitive(j, "enabled");
        if (enabled && !cJSON_IsBool(enabled)) {
            error = "Invalid enabled flag";
            return false;
        }
        if (name == "admin" && enabled && !cJSON_IsTrue(enabled)) {
            error = "Cannot disable admin";
            return false;
        }
        if (created) {
            if (list.size() >= 8) {
                error = "Maximum 8 users";
                return false;
            }
            list.push_back(User{});
            it = list.end() - 1;
            it->name = name;
        }
        auto password = field(j, "password");
        if (created || !password.empty()) {
            if (password.size() < 12 || password.size() > 128) {
                error = "Password must contain 12..128 bytes";
                return false;
            }
            it->salt = random_hex(16);
            it->hash = password_hash(password, it->salt);
            if (it->hash.empty()) {
                error = "Hash failed";
                return false;
            }
        }
        if (enabled) {
            it->enabled = cJSON_IsTrue(enabled);
        }
        auto key = cJSON_GetObjectItemCaseSensitive(j, "public_key");
        if (key) {
            if (!cJSON_IsString(key)) {
                error = "Invalid public key";
                return false;
            }
            std::string value = key->valuestring;
            if (value.size() > 4096 || (!value.empty() && decode_key(value).empty())) {
                error = "Expected one OpenSSH RSA, Ed25519 or P256 public key";
                return false;
            }
            it->public_key = value;
        }
    }
    if (std::none_of(list.begin(), list.end(), [](auto& u) { return u.enabled; })) {
        error = "Keep at least one enabled administrator";
        return false;
    }
    if (!persist(saved, list)) {
        error = "NVS write failed";
        return false;
    }
    users = std::move(list);
    invalidate_sessions();
    return true;
}
bool user_enabled(const std::string& name) {
    std::lock_guard lock(mutex);
    for (auto& u : users)
        if (u.name == name)
            return u.enabled;
    return false;
}
bool authenticate(const std::string& name, const std::string& password) {
    if (!auth_allowed() || name.size() > 32 || password.size() > 128)
        return false;
    User found;
    {
        std::lock_guard lock(mutex);
        for (auto& u : users)
            if (u.name == name)
                found = u;
    }
    auto hash = password_hash(password, found.salt.empty() ? std::string(32, '0') : found.salt);
    bool ok = !found.name.empty() && found.enabled && !hash.empty() && equal(hash, found.hash);
    if (!ok)
        auth_failed();
    return ok;
}
bool authenticate_key(const std::string& name, std::span<const uint8_t> key) {
    std::lock_guard lock(mutex);
    for (auto& u : users)
        if (u.name == name && u.enabled) {
            auto decoded = decode_key(u.public_key);
            return !decoded.empty() &&
                   equal(std::string_view(reinterpret_cast<const char*>(key.data()), key.size()),
                         std::string_view(reinterpret_cast<const char*>(decoded.data()), decoded.size()));
        }
    return false;
}
static int rng(void*, unsigned char* out, size_t n) {
    esp_fill_random(out, n);
    return 0;
}
static std::string generate_key() {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    unsigned char pem[2048]{};
    int rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    if (!rc)
        rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), rng, nullptr);
    if (!rc)
        rc = mbedtls_pk_write_key_pem(&pk, pem, sizeof(pem));
    mbedtls_pk_free(&pk);
    ESP_ERROR_CHECK(rc == 0 ? ESP_OK : ESP_FAIL);
    return reinterpret_cast<char*>(pem);
}
void identity_init() {
    if (secret_read("ssh_key").empty())
        ESP_ERROR_CHECK(secret_write("ssh_key", generate_key()) ? ESP_OK : ESP_FAIL);
    // HTTPS uses the identity embedded in the application. Legacy NVS TLS data
    // is ignored and left intact for rollback to an older firmware.
}
bool factory_reset() {
    return nvs_erase_all(nvs) == ESP_OK && nvs_commit(nvs) == ESP_OK;
}
} // namespace app
