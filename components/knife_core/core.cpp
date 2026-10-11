#include "core.hpp"
#include <algorithm>
#include <cstring>

namespace knife {
bool Gate::acquire(unsigned bits) {
    auto value = state_.load();
    do {
        if (value & (bits | Shutdown))
            return false;
    } while (!state_.compare_exchange_weak(value, value | bits));
    return true;
}
bool Gate::shutdown() {
    auto value = state_.load();
    do {
        if (value & (Card | Write | Transfer | Shutdown))
            return false;
    } while (!state_.compare_exchange_weak(value, value | Shutdown));
    return true;
}
static bool alnum(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}
bool safe_filename(std::string_view s) {
    return !s.empty() && s.size() <= 60 && s[0] != '.' && std::all_of(s.begin(), s.end(), [](char c) {
        return alnum(c) || c == '.' || c == '_' || c == '-';
    });
}
bool hostname_valid(std::string_view s) {
    return !s.empty() && s.size() <= 63 && alnum(s.front()) && alnum(s.back()) &&
           std::all_of(s.begin(), s.end(), [](char c) { return alnum(c) || c == '-'; });
}
bool email_valid(std::string_view s) {
    return s.size() >= 3 && s.size() <= 128 && s.find('@') != std::string_view::npos &&
           std::all_of(s.begin(), s.end(), [](char c) {
               return alnum(c) || std::string_view("@._+-").find(c) != std::string_view::npos;
           });
}
uint16_t crc16(std::span<const uint8_t> bytes) {
    uint16_t crc = 0;
    for (auto b : bytes) {
        crc ^= uint16_t(b) << 8;
        for (int i = 0; i < 8; ++i)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : crc << 1;
    }
    return crc;
}
void Telnet::feed(uint8_t b, const std::function<void(uint8_t)>& data,
                  const std::function<void(std::span<const uint8_t>)>& reply) {
    switch (state_) {
    case Data:
        if (b == 255)
            state_ = Iac;
        else
            data(b);
        break;
    case Iac:
        if (b == 255) {
            data(b);
            state_ = Data;
        } else if (b == 250)
            state_ = Sub;
        else if (b >= 251 && b <= 254) {
            command_ = b;
            state_ = Option;
        } else
            state_ = Data;
        break;
    case Option: {
        // Accept binary, suppress-go-ahead and server echo. Refuse other options once.
        bool supported = b == 0 || b == 3 || (b == 1 && command_ == 253);
        if (!supported && (command_ == 251 || command_ == 253)) {
            uint8_t r[] = {255, uint8_t(command_ == 251 ? 254 : 252), b};
            reply(r);
        }
        state_ = Data;
        break;
    }
    case Sub:
        if (b == 255)
            state_ = SubIac;
        break;
    case SubIac:
        state_ = b == 240 ? Data : Sub;
        break;
    }
}
std::vector<uint8_t> telnet_encode(std::span<const uint8_t> bytes) {
    std::vector<uint8_t> result;
    result.reserve(bytes.size() * 2);
    for (auto b : bytes) {
        result.push_back(b);
        if (b == 255)
            result.push_back(b);
    }
    return result;
}
bool vlan_strip(std::span<uint8_t> frame, uint16_t vid, size_t& size) {
    if (frame.size() < 14)
        return false;
    bool tagged = frame[12] == 0x81 && frame[13] == 0;
    if (!vid) {
        size = frame.size();
        return !tagged;
    }
    if (!tagged || frame.size() < 18 || (((frame[14] & 15) << 8) | frame[15]) != vid)
        return false;
    // Refuse nested VLANs.
    if ((frame[16] == 0x81 && frame[17] == 0) || (frame[16] == 0x88 && frame[17] == 0xa8))
        return false;
    std::memmove(frame.data() + 12, frame.data() + 16, frame.size() - 16);
    size = frame.size() - 4;
    return true;
}
std::vector<uint8_t> vlan_tag(std::span<const uint8_t> frame, uint16_t vid) {
    if (frame.size() < 14 || vid > 4094 || frame.size() > (vid ? 1510u : 1514u))
        return {};
    if (!vid)
        return {frame.begin(), frame.end()};
    std::vector<uint8_t> result(frame.size() + 4);
    std::copy_n(frame.begin(), 12, result.begin());
    result[12] = 0x81;
    result[13] = 0;
    result[14] = vid >> 8;
    result[15] = vid;
    std::copy(frame.begin() + 12, frame.end(), result.begin() + 16);
    return result;
}
bool modem_send(ModemIO& io, bool y, std::string_view filename, size_t size, std::string& error) {
    constexpr uint8_t Ack = 6, Nak = 21, Can = 24, Eot = 4;
    auto fail = [&](const char* message) {
        const uint8_t cancel[] = {Can, Can};
        io.write(cancel);
        error = message;
        return false;
    };
    auto wait = [&](bool crc_only) {
        for (unsigned i = 0; i < 10; ++i) {
            int c = io.read_byte(10000);
            if (c == 'C' || (!crc_only && c == Nak))
                return c;
            if (c == Can || c == -2)
                return -2;
        }
        return -1;
    };
    int initial = wait(y);
    if (initial < 0)
        return fail("Receiver not ready or cancelled");
    bool crc = initial == 'C';
    auto packet = [&](uint8_t number, std::span<const uint8_t> payload) {
        std::vector<uint8_t> p = {uint8_t(payload.size() == 1024 ? 2 : 1), number, uint8_t(255 - number)};
        p.insert(p.end(), payload.begin(), payload.end());
        if (crc) {
            auto c = crc16(payload);
            p.push_back(c >> 8);
            p.push_back(c);
        } else {
            uint8_t sum = 0;
            for (auto b : payload)
                sum += b;
            p.push_back(sum);
        }
        for (unsigned retry = 0; retry < 10; ++retry) {
            if (!io.write(p))
                return false;
            // A leftover 'C' or line noise is not an ACK and must not lose the block.
            for (unsigned noise = 0; noise < 32; ++noise) {
                int c = io.read_byte(10000);
                if (c == Ack)
                    return true;
                if (c == Can || c == -2)
                    return false;
                if (c == Nak || c == -1)
                    break;
            }
        }
        return false;
    };
    std::array<uint8_t, 1024> data{};
    if (y) {
        if (!safe_filename(filename))
            return fail("Invalid filename");
        auto header = std::string(filename) + '\0' + std::to_string(size);
        std::copy(header.begin(), header.end(), data.begin());
        if (!packet(0, std::span(data).first(128)) || wait(true) < 0)
            return fail("YMODEM header rejected");
    }
    size_t sent = 0;
    uint8_t number = 1;
    while (sent < size) {
        size_t block = y ? 1024 : 128, count = std::min(block, size - sent);
        data.fill(0x1a);
        if (io.read_file(std::span(data).first(count)) != count)
            return fail("File read failed");
        if (!packet(number++, std::span(data).first(block)))
            return fail("Block rejected or cancelled");
        sent += count;
        io.progress(sent);
    }
    bool ended = false;
    for (unsigned retry = 0; retry < 10; ++retry) {
        if (!io.write(std::span(&Eot, 1)))
            break;
        int c = io.read_byte(10000);
        if (c == Ack) {
            ended = true;
            break;
        }
        if (c == Can || c == -2)
            break;
    }
    if (!ended)
        return fail("End of file not acknowledged");
    if (y) {
        if (wait(true) < 0)
            return fail("Final YMODEM request missing");
        data.fill(0);
        if (!packet(0, std::span(data).first(128)))
            return fail("Final YMODEM block rejected");
    }
    return true;
}
} // namespace knife
