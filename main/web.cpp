#include "app.hpp"
#include "build_version.h"
#include "esp_https_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ota.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

extern const unsigned char html_start[] asm("_binary_index_html_start");
extern const unsigned char html_end[] asm("_binary_index_html_end");
extern const unsigned char js_start[] asm("_binary_app_js_start");
extern const unsigned char js_end[] asm("_binary_app_js_end");
extern const unsigned char css_start[] asm("_binary_style_css_start");
extern const unsigned char css_end[] asm("_binary_style_css_end");
extern const unsigned char cert_start[] asm("_binary_https_cert_pem_start");
extern const unsigned char cert_end[] asm("_binary_https_cert_pem_end");
extern const unsigned char key_start[] asm("_binary_https_key_pem_start");
extern const unsigned char key_end[] asm("_binary_https_key_pem_end");

namespace app {
struct Session {
    std::string token, user;
    int64_t expiry = 0;
};
static std::array<Session, 4> sessions;
static std::mutex session_mutex;
static httpd_handle_t server;
void invalidate_sessions() {
    std::lock_guard lock(session_mutex);
    for (auto& s : sessions)
        s = {};
}
static void headers(httpd_req_t* r) {
    httpd_resp_set_hdr(r, "Cache-Control", "no-store");
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(r, "Content-Security-Policy",
                       "default-src 'self'; script-src 'self'; style-src 'self'; object-src 'none'; "
                       "frame-ancestors 'none'; base-uri 'none'");
}
static esp_err_t json_response(httpd_req_t* r, cJSON* j, const char* status = "200 OK") {
    Json owned(j);
    auto s = json_text(j);
    headers(r);
    httpd_resp_set_type(r, "application/json");
    httpd_resp_set_status(r, status);
    return httpd_resp_send(r, s.c_str(), s.size());
}
static esp_err_t message(httpd_req_t* r, const std::string& text, const char* status = "200 OK") {
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "message", text.c_str());
    return json_response(r, j, status);
}
static bool authorized(httpd_req_t* r) {
    char header[96];
    if (httpd_req_get_hdr_value_str(r, "Authorization", header, sizeof(header)) != ESP_OK)
        return false;
    if (strncmp(header, "Bearer ", 7) || strlen(header + 7) != 64)
        return false;
    std::string user;
    {
        std::lock_guard lock(session_mutex);
        auto now = esp_timer_get_time();
        for (auto& s : sessions) {
            if (s.token.size() != 64 || now > s.expiry)
                continue;
            unsigned diff = 0;
            for (size_t i = 0; i < 64; ++i)
                diff |= unsigned(s.token[i] ^ header[i + 7]);
            if (!diff) {
                user = s.user;
                s.expiry = now + 1800000000LL;
                break;
            }
        }
    }
    return !user.empty() && user_enabled(user);
}
static bool read_body(httpd_req_t* r, std::string& out, size_t limit = 32768) {
    if (r->content_len > limit)
        return false;
    out.resize(r->content_len);
    size_t received = 0;
    int64_t deadline = esp_timer_get_time() + 15000000;
    while (received < out.size()) {
        int n = httpd_req_recv(r, out.data() + received, out.size() - received);
        if (n <= 0 || esp_timer_get_time() > deadline)
            return false;
        received += n;
    }
    return out.find('\0') == std::string::npos;
}
static esp_err_t login(httpd_req_t* r) {
    if (!auth_allowed())
        return message(r, "Try again later", "429 Too Many Requests");
    std::string body;
    if (!read_body(r, body, 1024))
        return message(r, "Invalid body", "400 Bad Request");
    Json j(cJSON_Parse(body.c_str()));
    auto name = field(j.p, "name"), password = field(j.p, "password");
    bool ok = authenticate(name, password);
    std::fill(password.begin(), password.end(), '\0');
    std::fill(body.begin(), body.end(), '\0');
    if (!ok)
        return message(r, "Authentication failed", "401 Unauthorized");
    std::string token = random_hex(32);
    {
        std::lock_guard lock(session_mutex);
        auto it = std::min_element(sessions.begin(), sessions.end(),
                                   [](auto& a, auto& b) { return a.expiry < b.expiry; });
        *it = {token, name, esp_timer_get_time() + 1800000000LL};
    }
    auto response = cJSON_CreateObject();
    cJSON_AddStringToObject(response, "token", token.c_str());
    return json_response(r, response);
}
static esp_err_t status(httpd_req_t* r) {
    auto j = cJSON_CreateObject();
    auto c = running;
    cJSON_AddStringToObject(j, "version", KNIFE_BUILD_VERSION);
    cJSON_AddStringToObject(j, "boot_id", boot_id.c_str());
    cJSON_AddStringToObject(j, "hostname", c.hostname.c_str());
    cJSON_AddStringToObject(j, "ip", ip_address().c_str());
    cJSON_AddBoolToObject(j, "network", network_up);
    cJSON_AddBoolToObject(j, "microSD", sd_ready);
    cJSON_AddNumberToObject(j, "uptime", esp_timer_get_time() / 1000000);
    cJSON_AddNumberToObject(j, "free_heap", esp_get_free_heap_size());
    cJSON_AddNumberToObject(j, "resources", gate.state());
    cJSON_AddStringToObject(j, "transfer", transfer_result().c_str());
    cJSON_AddNumberToObject(j, "sent", transfer_done);
    cJSON_AddNumberToObject(j, "size", transfer_size);
    cJSON_AddStringToObject(j, "storage", storage_result().c_str());
    cJSON_AddBoolToObject(j, "ssh", ssh_ready);
    cJSON_AddBoolToObject(j, "telnet", telnet_ready);
    cJSON_AddNumberToObject(j, "web_port", c.web_port);
    cJSON_AddBoolToObject(j, "web_tls", c.web_tls);
    cJSON_AddNumberToObject(j, "ssh_port", c.ssh_port);
    cJSON_AddNumberToObject(j, "telnet_port", c.telnet_port);
    cJSON_AddStringToObject(j, "transfer_protocols", "XMODEM, YMODEM");
    return json_response(r, j);
}
static std::string file_name(httpd_req_t* r) {
    char query[160]{}, name[80]{};
    if (httpd_req_get_url_query_str(r, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK)
        return {};
    return knife::safe_filename(name) ? std::string(name) : std::string();
}
static esp_err_t upload(httpd_req_t* r) {
    auto name = file_name(r);
    if (name.empty() || r->content_len > 64 * 1024 * 1024)
        return message(r, "Invalid filename or file larger than 64 MiB", "400 Bad Request");
    knife::Lease lease(gate, knife::Card);
    if (!lease)
        return message(r, "Card busy", "409 Conflict");
    if (!sd_ready)
        return message(r, "microSD unavailable", "503 Service Unavailable");
    std::string path = "/sd/" + name;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0)
        return message(r, "File exists; delete it explicitly first", "409 Conflict");
    const char* temporary = "/sd/.upload.tmp";
    FILE* f = fopen(temporary, "wb");
    if (!f)
        return message(r, "Cannot create file", "500 Internal Server Error");
    char buffer[4096];
    size_t received = 0;
    bool ok = true;
    auto started = esp_timer_get_time();
    while (received < r->content_len) {
        int n = httpd_req_recv(r, buffer, std::min(sizeof(buffer), r->content_len - received));
        if (n <= 0 || esp_timer_get_time() - started > 600000000LL || fwrite(buffer, 1, n, f) != size_t(n)) {
            ok = false;
            break;
        }
        received += n;
    }
    if (fflush(f) != 0 || fsync(fileno(f)) != 0)
        ok = false;
    if (fclose(f) != 0)
        ok = false;
    if (ok && rename(temporary, path.c_str()) != 0)
        ok = false;
    if (!ok)
        remove(temporary);
    return message(r, ok ? "Uploaded" : "Upload failed", ok ? "201 Created" : "500 Internal Server Error");
}
static esp_err_t get_handler(httpd_req_t* r) {
    std::string path = r->uri;
    if (path == "/api/health") {
        auto j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "boot_id", boot_id.c_str());
        cJSON_AddStringToObject(j, "version", KNIFE_BUILD_VERSION);
        cJSON_AddStringToObject(j, "image_id", firmware_image_id().c_str());
        cJSON_AddStringToObject(j, "ota_state", firmware_state());
        cJSON_AddStringToObject(j, "ota_partition", esp_ota_get_running_partition()->label);
        return json_response(r, j);
    }
    if (path.rfind("/api/", 0) != 0) {
        headers(r);
        const unsigned char* start = nullptr;
        const unsigned char* end = nullptr;
        if (path == "/") {
            start = html_start;
            end = html_end;
            httpd_resp_set_type(r, "text/html; charset=utf-8");
        } else if (path == "/app.js") {
            start = js_start;
            end = js_end;
            httpd_resp_set_type(r, "application/javascript; charset=utf-8");
        } else if (path == "/style.css") {
            start = css_start;
            end = css_end;
            httpd_resp_set_type(r, "text/css; charset=utf-8");
        } else
            return message(r, "Not found", "404 Not Found");
        return httpd_resp_send(r, reinterpret_cast<const char*>(start), end - start - 1);
    }
    if (!authorized(r))
        return message(r, "Authentication required", "401 Unauthorized");
    if (path == "/api/status")
        return status(r);
    if (path == "/api/settings")
        return json_response(r, settings_json());
    if (path == "/api/users")
        return json_response(r, users_json());
    if (path == "/api/files") {
        knife::Lease lease(gate, knife::Card);
        if (!lease)
            return message(r, "Card busy", "409 Conflict");
        if (!sd_ready)
            return message(r, "microSD unavailable", "503 Service Unavailable");
        return json_response(r, files_json());
    }
    return message(r, "Not found", "404 Not Found");
}
static esp_err_t update_firmware(httpd_req_t* r) {
    auto reject = [&](const std::string& error, const char* status) {
        // Close after a rejection, since a partially read binary body may remain.
        message(r, error, status);
        return ESP_FAIL;
    };
    knife::Lease lease(gate, knife::Update);
    if (!lease)
        return reject("Устройство занято. Закройте serial-сессию и завершите файловые операции",
                      "409 Conflict");
    FirmwareUpdate update;
    std::string error;
    if (!firmware_receive(
            r->content_len,
            [r](uint8_t* data, size_t size) {
                return httpd_req_recv(r, reinterpret_cast<char*>(data), size);
            },
            update, error))
        return reject(error, "400 Bad Request");
    if (esp_ota_set_boot_partition(update.partition) != ESP_OK)
        return reject("Не удалось выбрать новую прошивку для запуска", "500 Internal Server Error");
    if (!gate.finish_update())
        abort(); // Update lease must be exclusive; never release it before reboot.
    lease.held = false;
    if (!schedule_restart()) {
        if (esp_ota_set_boot_partition(esp_ota_get_running_partition()) != ESP_OK)
            esp_restart(); // Do not report a cancelled update with a new boot selection left active.
        gate.release(knife::Shutdown);
        return reject("Не удалось запланировать перезагрузку", "500 Internal Server Error");
    }
    auto j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "message", "Подпись проверена. Перезагрузка для установки обновления");
    cJSON_AddStringToObject(j, "image_id", update.image_id.c_str());
    cJSON_AddStringToObject(j, "ota_partition", update.partition->label);
    return json_response(r, j, "202 Accepted");
}
static esp_err_t post_handler(httpd_req_t* r) {
    std::string path = r->uri;
    if (path == "/api/login")
        return login(r);
    if (!authorized(r)) {
        auto sent = message(r, "Authentication required", "401 Unauthorized");
        return path == "/api/firmware" ? ESP_FAIL : sent;
    }
    if (gate.state() & knife::Shutdown)
        return message(r, "Restart in progress", "503 Service Unavailable");
    if (path == "/api/firmware")
        return update_firmware(r);
    if (path.rfind("/api/upload?", 0) == 0)
        return upload(r);
    std::string body;
    if (!read_body(r, body))
        return message(r, "Body too large or incomplete", "400 Bad Request");
    Json j(cJSON_Parse(body.c_str()));
    if (!cJSON_IsObject(j.p))
        return message(r, "Expected JSON object", "400 Bad Request");
    std::string error;
    bool ok = false;
    if (path == "/api/settings") {
        ok = save_settings(j.p, error);
        if (ok)
            return message(r, "Saved. Restart to apply settings.");
    } else if (path == "/api/users") {
        ok = save_user(j.p, false, error);
        if (ok)
            return message(r, "Saved. Sign in again.");
    } else if (path == "/api/users/delete") {
        ok = save_user(j.p, true, error);
        if (ok)
            return message(r, "Deleted. Sign in again.");
    } else if (path == "/api/logout") {
        invalidate_sessions();
        return message(r, "Signed out on all browsers");
    } else if (path == "/api/transfer") {
        auto protocol = field(j.p, "protocol");
        if (protocol != "xmodem" && protocol != "ymodem")
            return message(r, "Supported: xmodem, ymodem", "400 Bad Request");
        ok = transfer_start(field(j.p, "name"), protocol == "ymodem", error);
        if (ok)
            return message(r, "Transfer started", "202 Accepted");
    } else if (path == "/api/transfer/cancel") {
        transfer_cancel = true;
        return message(r, "Cancellation requested");
    } else if (path == "/api/files/delete") {
        auto name = field(j.p, "name");
        if (!knife::safe_filename(name))
            return message(r, "Invalid filename", "400 Bad Request");
        knife::Lease lease(gate, knife::Card);
        if (!lease)
            return message(r, "Card busy", "409 Conflict");
        if (!sd_ready)
            return message(r, "microSD unavailable", "503 Service Unavailable");
        ok = remove(("/sd/" + name).c_str()) == 0;
        if (ok)
            return message(r, "Deleted");
        error = "Cannot delete file";
    } else if (path == "/api/format") {
        if (field(j.p, "confirm") != "FORMAT")
            return message(r, "Explicit FORMAT confirmation required", "400 Bad Request");
        ok = format_start(error);
        if (ok)
            return message(r, "Formatting started", "202 Accepted");
    } else if (path == "/api/reboot") {
        if (field(j.p, "confirm") != "REBOOT")
            return message(r, "Explicit REBOOT confirmation required", "400 Bad Request");
        if (!gate.shutdown())
            return message(r, "Device busy; finish active write/transfer/card operation", "409 Conflict");
        auto sent = message(r, "Restart scheduled; all connections will close", "202 Accepted");
        if (sent != ESP_OK || !schedule_restart())
            gate.release(knife::Shutdown);
        return sent;
    } else
        return message(r, "Not found", "404 Not Found");
    return message(r, error, "409 Conflict");
}
void web_start() {
    httpd_ssl_config_t cfg = HTTPD_SSL_CONFIG_DEFAULT();
    cfg.transport_mode = running.web_tls ? HTTPD_SSL_TRANSPORT_SECURE : HTTPD_SSL_TRANSPORT_INSECURE;
    cfg.port_secure = running.web_port;
    cfg.port_insecure = running.web_port;
    cfg.httpd.stack_size = 16384;
    cfg.httpd.max_uri_handlers = 3;
    cfg.httpd.max_open_sockets = 4;
    cfg.httpd.lru_purge_enable = true;
    cfg.httpd.recv_wait_timeout = 5;
    cfg.httpd.send_wait_timeout = 5;
    cfg.httpd.uri_match_fn = httpd_uri_match_wildcard;
    cfg.servercert = cert_start;
    cfg.servercert_len = cert_end - cert_start;
    cfg.prvtkey_pem = key_start;
    cfg.prvtkey_len = key_end - key_start;
    ESP_ERROR_CHECK(httpd_ssl_start(&server, &cfg));
    httpd_uri_t get{};
    get.uri = "/*";
    get.method = HTTP_GET;
    get.handler = get_handler;
    httpd_uri_t post{};
    post.uri = "/*";
    post.method = HTTP_POST;
    post.handler = post_handler;
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &get));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &post));
    web_ready = true;
}
} // namespace app
