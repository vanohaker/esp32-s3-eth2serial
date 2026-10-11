#include "app.hpp"
#include "board.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/tcp.h"
#include "wolfssh/error.h"
#include "wolfssh/ssh.h"
#include <algorithm>
#include <cstring>
#include <fcntl.h>

namespace app {
void serial_start() {
    auto c = config();
    uart_config_t uc{};
    uc.baud_rate = c.baud;
    uc.data_bits = static_cast<uart_word_length_t>(c.data_bits - 5);
    uc.parity = c.parity == "even"  ? UART_PARITY_EVEN
                : c.parity == "odd" ? UART_PARITY_ODD
                                    : UART_PARITY_DISABLE;
    uc.stop_bits = c.stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
    uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uc.source_clk = UART_SCLK_DEFAULT;
    ESP_ERROR_CHECK(uart_param_config(board::uart, &uc));
    ESP_ERROR_CHECK(
        uart_set_pin(board::uart, board::uart_tx, board::uart_rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(board::uart, 8192, 0, 0, nullptr, 0));
}
static bool stopping() {
    return gate.state() & knife::Shutdown;
}
static void prepare_socket(int fd) {
    fcntl(fd, F_SETFL, O_NONBLOCK);
    int enabled = 1, idle = 60, interval = 10, count = 3;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &enabled, sizeof(enabled));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof(interval));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof(count));
}
static void reject_waiting(int server) {
    // Do not queue a second terminal that could unexpectedly attach later.
    for (unsigned i = 0; i < 4; ++i) {
        int fd = accept(server, nullptr, nullptr);
        if (fd < 0)
            return;
        close(fd);
    }
}
static bool socket_send(int fd, std::span<const uint8_t> bytes) {
    size_t offset = 0;
    int64_t deadline = esp_timer_get_time() + 5000000;
    while (offset < bytes.size() && !stopping()) {
        int n = send(fd, bytes.data() + offset, bytes.size() - offset, 0);
        if (n > 0) {
            offset += n;
            continue;
        }
        if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
            return false;
        if (esp_timer_get_time() > deadline)
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return offset == bytes.size();
}
static bool send_text(int fd, const char* s) {
    return socket_send(fd, {reinterpret_cast<const uint8_t*>(s), strlen(s)});
}
static int listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (fd < 0)
        return -1;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) || listen(fd, 1)) {
        close(fd);
        return -1;
    }
    fcntl(fd, F_SETFL, O_NONBLOCK);
    return fd;
}
static bool login_line(int fd, int server, knife::Telnet& parser, std::string& line, bool echo,
                       bool& skip_lf) {
    int64_t deadline = esp_timer_get_time() + 30000000;
    bool done = false, ok = true;
    while (!done && ok && !stopping() && network_up && esp_timer_get_time() < deadline) {
        reject_waiting(server);
        uint8_t b;
        int n = recv(fd, &b, 1, 0);
        if (n == 0)
            return false;
        if (n < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
                return false;
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        parser.feed(
            b,
            [&](uint8_t ch) {
                if (skip_lf && (ch == '\n' || ch == 0)) {
                    skip_lf = false;
                    return;
                }
                skip_lf = false;
                if (ch == '\r' || ch == '\n') {
                    done = true;
                    skip_lf = ch == '\r';
                    return;
                }
                if (ch == 8 || ch == 127) {
                    if (!line.empty()) {
                        line.pop_back();
                        if (echo)
                            send_text(fd, "\b \b");
                    }
                    return;
                }
                if (ch >= 32 && ch < 127) {
                    if (line.size() >= 128) {
                        ok = false;
                        return;
                    }
                    line += char(ch);
                    if (echo)
                        socket_send(fd, {&ch, 1});
                }
            },
            [&](auto reply) { ok = socket_send(fd, reply); });
    }
    send_text(fd, "\r\n");
    return done && ok;
}
static void telnet_session(int fd, int server) {
    if (!auth_allowed()) {
        send_text(fd, "Try again later.\r\n");
        return;
    }
    knife::Telnet parser;
    bool skip_lf = false;
    std::string name, password;
    const uint8_t options[] = {255, 251, 1, 255, 251, 3, 255, 253, 3, 255, 251, 0, 255, 253, 0};
    if (!socket_send(fd, options) || !send_text(fd, "Login: ") ||
        !login_line(fd, server, parser, name, true, skip_lf) || !send_text(fd, "Password: ") ||
        !login_line(fd, server, parser, password, false, skip_lf))
        return;
    bool valid = authenticate(name, password);
    std::fill(password.begin(), password.end(), '\0');
    if (!valid) {
        send_text(fd, "Authentication failed.\r\n");
        return;
    }
    knife::Lease lease(gate, knife::Uart);
    if (!lease) {
        send_text(fd, "Serial busy. Disconnect the other session or wait for transfer.\r\n");
        return;
    }
    uart_flush_input(board::uart);
    send_text(fd, "Connected. Close this connection to release serial.\r\n");
    uint8_t buffer[512];
    bool ok = true;
    int64_t checked = 0;
    while (ok && !stopping() && network_up) {
        reject_waiting(server);
        auto now = esp_timer_get_time();
        if (now - checked > 1000000) {
            checked = now;
            if (!user_enabled(name))
                break;
        }
        int n = recv(fd, buffer, sizeof(buffer), 0);
        if (n == 0)
            break;
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
            break;
        std::vector<uint8_t> decoded;
        for (int i = 0; i < n; ++i)
            parser.feed(
                buffer[i],
                [&](uint8_t b) {
                    if (skip_lf && (b == '\n' || b == 0)) {
                        skip_lf = false;
                        return;
                    }
                    skip_lf = false;
                    decoded.push_back(b);
                },
                [&](auto r) { ok = socket_send(fd, r); });
        if (!decoded.empty() &&
            uart_write_bytes(board::uart, decoded.data(), decoded.size()) != int(decoded.size()))
            break;
        n = uart_read_bytes(board::uart, buffer, sizeof(buffer), pdMS_TO_TICKS(10));
        if (n > 0) {
            auto encoded = knife::telnet_encode({buffer, size_t(n)});
            ok = socket_send(fd, encoded);
        }
    }
}
struct AuthContext {
    std::string name;
    unsigned attempts = 0;
    bool channel_open = false;
};
static int channel_open(WOLFSSH_CHANNEL* channel, void* context) {
    auto state = static_cast<AuthContext*>(context);
    auto type = wolfSSH_ChannelGetType(channel);
    if (!state || state->channel_open || !type || strcmp(type, "session"))
        return WS_CHANOPEN_FAILED;
    state->channel_open = true;
    return WS_SUCCESS;
}
static int channel_request(WOLFSSH_CHANNEL*, const byte* type, word32 size, const byte*, word32, int, void*) {
    std::string_view name(reinterpret_cast<const char*>(type), size);
    return name == "shell" || name == "pty-req" || name == "window-change" ? WOLFSSH_REQ_UNHANDLED
                                                                           : WOLFSSH_REQ_REJECT;
}
static int ssh_auth(byte type, WS_UserAuthData* auth, void* context) {
    auto state = static_cast<AuthContext*>(context);
    if (++state->attempts > 6 || !auth_allowed() || auth->usernameSz > 32)
        return WOLFSSH_USERAUTH_REJECTED;
    std::string name(reinterpret_cast<const char*>(auth->username), auth->usernameSz);
    bool valid = false;
    if (type == WOLFSSH_USERAUTH_PASSWORD && auth->sf.password.passwordSz <= 128) {
        std::string password(reinterpret_cast<const char*>(auth->sf.password.password),
                             auth->sf.password.passwordSz);
        valid = authenticate(name, password);
        std::fill(password.begin(), password.end(), '\0');
    } else if (type == WOLFSSH_USERAUTH_PUBLICKEY) {
        valid = authenticate_key(name, {auth->sf.publicKey.publicKey, auth->sf.publicKey.publicKeySz});
        // Libraries verify possession of the private key after this authorization check.
    }
    if (valid) {
        state->name = name;
        return WOLFSSH_USERAUTH_SUCCESS;
    }
    if (type == WOLFSSH_USERAUTH_PASSWORD)
        auth_failed();
    return WOLFSSH_USERAUTH_FAILURE;
}
static bool ssh_retry(WOLFSSH* ssh, int result) {
    int e = wolfSSH_get_error(ssh);
    return result == WS_WANT_READ || result == WS_WANT_WRITE || result == WS_REKEYING || e == WS_WANT_READ ||
           e == WS_WANT_WRITE || e == WS_REKEYING;
}
static bool ssh_send(WOLFSSH* ssh, std::span<uint8_t> data) {
    size_t offset = 0;
    int64_t deadline = esp_timer_get_time() + 10000000;
    while (offset < data.size() && !stopping()) {
        int n = wolfSSH_stream_send(ssh, data.data() + offset, data.size() - offset);
        if (n > 0) {
            offset += n;
            continue;
        }
        if (!ssh_retry(ssh, n) || esp_timer_get_time() > deadline)
            return false;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return offset == data.size();
}
static void ssh_session(int fd, int server, WOLFSSH_CTX* ctx) {
    if (!auth_allowed())
        return;
    WOLFSSH* ssh = wolfSSH_new(ctx);
    if (!ssh)
        return;
    AuthContext auth;
    wolfSSH_SetUserAuthCtx(ssh, &auth);
    wolfSSH_SetChannelOpenCtx(ssh, &auth);
    wolfSSH_set_fd(ssh, fd);
    int64_t deadline = esp_timer_get_time() + 30000000;
    int rc;
    do {
        reject_waiting(server);
        rc = wolfSSH_accept(ssh);
        if (rc == WS_SUCCESS)
            break;
        if (!ssh_retry(ssh, rc))
            break;
        vTaskDelay(pdMS_TO_TICKS(10));
    } while (!stopping() && network_up && esp_timer_get_time() < deadline);
    if (rc == WS_SUCCESS && !auth.name.empty() && wolfSSH_GetSessionType(ssh) == WOLFSSH_SESSION_SHELL) {
        knife::Lease lease(gate, knife::Uart);
        if (!lease) {
            uint8_t msg[] = "Serial busy.\r\n";
            ssh_send(ssh, {msg, sizeof(msg) - 1});
        } else {
            uart_flush_input(board::uart);
            uint8_t buf[512];
            int64_t checked = 0;
            while (!stopping() && network_up) {
                reject_waiting(server);
                auto now = esp_timer_get_time();
                if (now - checked > 1000000) {
                    checked = now;
                    if (!user_enabled(auth.name))
                        break;
                }
                int n = wolfSSH_stream_read(ssh, buf, sizeof(buf));
                if (n > 0) {
                    if (uart_write_bytes(board::uart, buf, n) != n)
                        break;
                } else if (n == 0 || !ssh_retry(ssh, n))
                    break;
                n = uart_read_bytes(board::uart, buf, sizeof(buf), pdMS_TO_TICKS(10));
                if (n > 0 && !ssh_send(ssh, {buf, size_t(n)}))
                    break;
            }
        }
    }
    wolfSSH_shutdown(ssh);
    wolfSSH_free(ssh);
}
static void telnet_task(void*) {
    int server = listen_on(config().telnet_port);
    telnet_ready = server >= 0;
    if (server >= 0)
        while (!stopping()) {
            int fd = accept(server, nullptr, nullptr);
            if (fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            prepare_socket(fd);
            telnet_session(fd, server);
            close(fd);
            reject_waiting(server);
        }
    if (server >= 0)
        close(server);
    telnet_ready = false;
    vTaskDelete(nullptr);
}
static void ssh_run() {
    WOLFSSH_CTX* ctx = nullptr;
    int server = -1;
    if (wolfSSH_Init() == WS_SUCCESS)
        ctx = wolfSSH_CTX_new(WOLFSSH_ENDPOINT_SERVER, nullptr);
    auto key = secret_read("ssh_key");
    if (ctx && wolfSSH_CTX_UsePrivateKey_buffer(ctx, reinterpret_cast<const byte*>(key.c_str()), key.size(),
                                                WOLFSSH_FORMAT_PEM) == WS_SUCCESS) {
        wolfSSH_SetUserAuth(ctx, ssh_auth);
        wolfSSH_CTX_SetChannelOpenCb(ctx, channel_open);
        wolfSSH_CTX_SetChannelReqAnyCb(ctx, channel_request);
        server = listen_on(config().ssh_port);
        ssh_ready = server >= 0;
        while (server >= 0 && !stopping()) {
            int fd = accept(server, nullptr, nullptr);
            if (fd < 0) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            prepare_socket(fd);
            ssh_session(fd, server, ctx);
            close(fd);
            reject_waiting(server);
        }
    } else
        ESP_LOGE("ssh", "SSH initialization failed");
    if (server >= 0)
        close(server);
    if (ctx)
        wolfSSH_CTX_free(ctx);
    wolfSSH_Cleanup();
    ssh_ready = false;
}
static void ssh_task(void*) {
    ssh_run();
    vTaskDelete(nullptr);
}
void console_start() {
    auto c = config();
    if (c.ssh)
        ESP_ERROR_CHECK(xTaskCreate(ssh_task, "ssh", 20480, nullptr, 5, nullptr) == pdPASS ? ESP_OK
                                                                                           : ESP_ERR_NO_MEM);
    if (c.telnet)
        ESP_ERROR_CHECK(xTaskCreate(telnet_task, "telnet", 6144, nullptr, 5, nullptr) == pdPASS
                            ? ESP_OK
                            : ESP_ERR_NO_MEM);
}
} // namespace app
