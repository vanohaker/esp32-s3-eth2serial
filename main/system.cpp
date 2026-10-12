#include "app.hpp"
#include "board.hpp"
#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include <cstring>
#include <fcntl.h>

namespace app {
static void restart_task(void*) {
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}
bool schedule_restart() {
    if (xTaskCreate(restart_task, "restart", 3072, nullptr, 3, nullptr) != pdPASS) {
        return false;
    }
    return true;
}
static void button_task(void*) {
    gpio_config_t io{};
    io.pin_bit_mask = 1ULL << board::boot;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    ESP_ERROR_CHECK(gpio_config(&io));
    int64_t since = 0;
    bool armed = false;
    while (true) {
        if (!gpio_get_level(static_cast<gpio_num_t>(board::boot))) {
            if (!since)
                since = esp_timer_get_time();
            if (!armed && esp_timer_get_time() - since >= 10000000) {
                armed = true;
                ESP_LOGW(
                    "reset",
                    "Factory reset armed; release BOOT to erase internal settings. microSD is preserved.");
            }
        } else {
            if (armed) {
                if (gate.shutdown()) {
                    if (factory_reset())
                        esp_restart();
                    else {
                        ESP_LOGE("reset", "NVS erase failed");
                        gate.release(knife::Shutdown);
                    }
                } else
                    ESP_LOGW("reset", "Device busy; reset refused");
            }
            since = 0;
            armed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}
static bool smtp_write(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        int n = send(fd, s.data() + off, s.size() - off, 0);
        if (n <= 0)
            return false;
        off += n;
    }
    return true;
}
static bool smtp_reply(int fd, int code) {
    auto deadline = esp_timer_get_time() + 10000000;
    for (int lines = 0; lines < 32; ++lines) {
        std::string line;
        char ch;
        while (line.size() < 512) {
            if (esp_timer_get_time() > deadline || recv(fd, &ch, 1, 0) != 1)
                return false;
            line += ch;
            if (ch == '\n')
                break;
        }
        if (line.size() < 5 || line.back() != '\n' || atoi(line.c_str()) != code)
            return false;
        if (line[3] == ' ')
            return true;
        if (line[3] != '-')
            return false;
    }
    return false;
}
static bool mail_once(const Config& c) {
    addrinfo hints{}, *list = nullptr;
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    auto port = std::to_string(c.smtp_port);
    if (getaddrinfo(c.smtp_host.c_str(), port.c_str(), &hints, &list))
        return false;
    int fd = -1;
    for (auto p = list; p; p = p->ai_next) {
        fd = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
        if (fd < 0)
            continue;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int rc = connect(fd, p->ai_addr, p->ai_addrlen);
        if (rc && errno == EINPROGRESS) {
            fd_set write;
            FD_ZERO(&write);
            FD_SET(fd, &write);
            timeval timeout{5, 0};
            rc = select(fd + 1, nullptr, &write, nullptr, &timeout);
            int error = 0;
            socklen_t size = sizeof(error);
            if (rc > 0 && getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) == 0 && error == 0)
                rc = 0;
            else
                rc = -1;
        }
        if (rc == 0)
            break;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(list);
    if (fd < 0)
        return false;
    fcntl(fd, F_SETFL, 0);
    timeval timeout{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    auto command = [&](const std::string& s, int code) {
        return smtp_write(fd, s + "\r\n") && smtp_reply(fd, code);
    };
    bool ok = smtp_reply(fd, 220) && command("EHLO " + c.hostname, 250) &&
              command("MAIL FROM:<" + c.mail_from + ">", 250) &&
              command("RCPT TO:<" + c.mail_to + ">", 250) && command("DATA", 354);
    if (ok) {
        std::string ip = ip_address();
        std::string body =
            "From: <" + c.mail_from + ">\r\nTo: <" + c.mail_to +
            ">\r\nSubject: Serial dongle started: " + c.hostname +
            "\r\nMIME-Version: 1.0\r\nContent-Type: text/plain; charset=us-ascii\r\n\r\nHostname: " +
            c.hostname + "\r\nIP: " + ip + "\r\nWeb: " + (c.web_tls ? "https://" : "http://") + ip + ":" +
            std::to_string(c.web_port) + "/\r\n";
        body +=
            "SSH: " +
            std::string(ssh_ready ? "enabled port " + std::to_string(c.ssh_port) : "disabled/unavailable") +
            "\r\n";
        body += "Telnet: " +
                std::string(telnet_ready ? "enabled port " + std::to_string(c.telnet_port)
                                         : "disabled/unavailable") +
                "\r\n";
        body += "VLAN: " + std::to_string(c.vlan) + "\r\nUART: " + std::to_string(c.baud) + " " +
                std::to_string(c.data_bits) + "/" + c.parity + "/" + std::to_string(c.stop_bits) +
                "\r\nFile transfer: XMODEM, YMODEM\r\n.\r\n";
        ok = smtp_write(fd, body) && smtp_reply(fd, 250);
        if (ok)
            command("QUIT", 221);
    }
    close(fd);
    return ok;
}
static void smtp_run() {
    auto c = running;
    unsigned retry = 0;
    while (true) {
        if (network_up && web_ready && (!c.ssh || ssh_ready) && (!c.telnet || telnet_ready)) {
            if (mail_once(c)) {
                ESP_LOGI("smtp", "Startup notification delivered");
                break;
            }
            ESP_LOGW("smtp", "Startup notification failed; retry scheduled");
            unsigned seconds = std::min(300u, 5u << std::min(retry++, 6u));
            for (unsigned i = 0; i < seconds; ++i)
                vTaskDelay(pdMS_TO_TICKS(1000));
        } else
            vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
static void smtp_task(void*) {
    smtp_run();
    vTaskDelete(nullptr);
}
void system_start() {
    ESP_ERROR_CHECK(xTaskCreate(button_task, "button", 4096, nullptr, 3, nullptr) == pdPASS ? ESP_OK
                                                                                            : ESP_ERR_NO_MEM);
    if (running.smtp)
        ESP_ERROR_CHECK(xTaskCreate(smtp_task, "smtp", 6144, nullptr, 2, nullptr) == pdPASS ? ESP_OK
                                                                                            : ESP_ERR_NO_MEM);
}
} // namespace app
