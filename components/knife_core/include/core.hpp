#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace knife {
enum Resource : unsigned { Uart = 1, Card = 2, Write = 4, Transfer = 8, Shutdown = 16, Update = 32 };
class Gate {
    std::atomic<unsigned> state_{0};

  public:
    bool acquire(unsigned bits);
    void release(unsigned bits) {
        state_.fetch_and(~bits);
    }
    unsigned state() const {
        return state_.load();
    }
    bool shutdown();      // An idle console may be disconnected; an operation may not.
    bool finish_update(); // Atomically keep all resources blocked until restart.
};
struct Lease {
    Gate& gate;
    unsigned bits;
    bool held;
    Lease(Gate& g, unsigned b) : gate(g), bits(b), held(g.acquire(b)) {}
    ~Lease() {
        if (held)
            gate.release(bits);
    }
    Lease(const Lease&) = delete;
    explicit operator bool() const {
        return held;
    }
};
bool safe_filename(std::string_view name);
bool hostname_valid(std::string_view name);
bool email_valid(std::string_view name);
uint16_t crc16(std::span<const uint8_t> bytes);
// Stateful Telnet decoder; strips negotiations/subnegotiations, preserves binary IAC.
class Telnet {
    enum State { Data, Iac, Option, Sub, SubIac } state_ = Data;
    uint8_t command_ = 0;

  public:
    void feed(uint8_t b, const std::function<void(uint8_t)>& data,
              const std::function<void(std::span<const uint8_t>)>& reply);
};
std::vector<uint8_t> telnet_encode(std::span<const uint8_t> bytes);
bool vlan_strip(std::span<uint8_t> frame, uint16_t vid, size_t& size);
std::vector<uint8_t> vlan_tag(std::span<const uint8_t> frame, uint16_t vid);
struct ModemIO {
    // read_byte returns -1 on timeout, -2 on cancellation/error.
    std::function<int(unsigned)> read_byte;
    std::function<bool(std::span<const uint8_t>)> write;
    std::function<size_t(std::span<uint8_t>)> read_file;
    std::function<void(size_t)> progress;
};
// XMODEM-CRC/checksum (128 bytes) and single-file YMODEM (1 KiB).
bool modem_send(ModemIO& io, bool ymodem, std::string_view filename, size_t size, std::string& error);
} // namespace knife
