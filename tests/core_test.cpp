#include "core.hpp"
#include <algorithm>
#include <barrier>
#include <cassert>
#include <deque>
#include <iostream>
#include <thread>
using namespace knife;
static void gate_tests() {
    Gate gate;
    assert(gate.acquire(Uart));
    assert(!gate.acquire(Uart | Card | Transfer));
    assert(!(gate.state() & Card));
    assert(gate.acquire(Card));
    assert(!gate.shutdown());
    gate.release(Card);
    assert(gate.shutdown());
    assert(!gate.acquire(Write));
    gate.release(Uart);
    assert(gate.state() == Shutdown);
    Gate update;
    assert(update.acquire(Uart));
    assert(!update.acquire(Update));
    update.release(Uart);
    assert(update.acquire(Update));
    for (unsigned resource : {Uart, Card, Write, Transfer, Update})
        assert(!update.acquire(resource));
    assert(!update.shutdown());
    assert(update.finish_update());
    assert(update.state() == Shutdown);
    assert(!update.acquire(Card));
    Gate failed;
    assert(failed.acquire(Update));
    failed.release(Update);
    assert(failed.acquire(Write));
    assert(!failed.finish_update());
    Gate concurrent;
    std::barrier ready(9), hold(9);
    std::atomic<int> winners{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 8; ++i)
        threads.emplace_back([&] {
            ready.arrive_and_wait();
            bool held = concurrent.acquire(Uart);
            if (held)
                ++winners;
            hold.arrive_and_wait();
            if (held)
                concurrent.release(Uart);
        });
    ready.arrive_and_wait();
    hold.arrive_and_wait();
    for (auto& t : threads)
        t.join();
    assert(winners == 1);
}
static void telnet_tests() {
    Telnet parser;
    std::vector<uint8_t> out, replies;
    std::vector<uint8_t> input = {'a', 255, 251, 42,  255, 250, 24,  0,   'x',
                                  255, 255, 'y', 255, 240, 0,   255, 255, 'z'};
    for (auto b : input)
        parser.feed(
            b, [&](auto d) { out.push_back(d); },
            [&](auto r) { replies.insert(replies.end(), r.begin(), r.end()); });
    assert((out == std::vector<uint8_t>{'a', 0, 255, 'z'}));
    assert((replies == std::vector<uint8_t>{255, 254, 42}));
    assert((telnet_encode(out) == std::vector<uint8_t>{'a', 0, 255, 255, 'z'}));
}
static void vlan_tests() {
    std::vector<uint8_t> plain(1510, 0x23);
    plain[12] = 8;
    plain[13] = 0;
    auto frame = vlan_tag(plain, 4094);
    assert(frame.size() == 1514 && frame[12] == 0x81 && frame[14] == 15 && frame[15] == 254);
    size_t size = 0;
    assert(!vlan_strip(frame, 3, size));
    assert(vlan_strip(frame, 4094, size));
    frame.resize(size);
    assert(frame == plain);
    assert(!vlan_strip(plain, 2, size));
    assert(vlan_strip(plain, 0, size));
    assert(vlan_tag(std::vector<uint8_t>(1511), 2).empty());
    assert(vlan_tag(plain, 4095).empty());
    std::vector<uint8_t> short_frame(13);
    assert(!vlan_strip(short_frame, 0, size));
}
struct Receiver {
    std::deque<int> responses;
    std::vector<std::vector<uint8_t>> packets;
    std::vector<uint8_t> source;
    size_t offset = 0, progress = 0;
    bool disconnect = false;
    ModemIO io() {
        return {[&](unsigned timeout) {
                    assert(timeout == 10000);
                    if (responses.empty())
                        return -1;
                    int c = responses.front();
                    responses.pop_front();
                    return c;
                },
                [&](std::span<const uint8_t> data) {
                    packets.emplace_back(data.begin(), data.end());
                    return !disconnect;
                },
                [&](std::span<uint8_t> data) {
                    size_t n = std::min(data.size(), source.size() - offset);
                    std::copy_n(source.data() + offset, n, data.data());
                    offset += n;
                    return n;
                },
                [&](size_t n) { progress = n; }};
    }
};
static void modem_tests() {
    const std::string vector = "123456789";
    assert(crc16({reinterpret_cast<const uint8_t*>(vector.data()), vector.size()}) == 0x31c3);
    std::string error;
    Receiver x;
    x.responses = {'C', 21, 6, 6, 6};
    x.source = std::vector<uint8_t>(129, 0x42);
    auto io = x.io();
    assert(modem_send(io, false, "firmware.bin", 129, error));
    assert(x.progress == 129 && x.offset == 129);
    assert(x.packets.size() == 4 && x.packets[0] == x.packets[1]);
    assert(x.packets[0].size() == 133 && x.packets[0][1] == 1);
    assert(x.packets[2][1] == 2 && x.packets[2][3] == 0x42 && x.packets[2][4] == 0x1a);
    assert(crc16(std::span(x.packets[0]).subspan(3, 128)) == (x.packets[0][131] << 8 | x.packets[0][132]));
    Receiver checksum;
    checksum.responses = {21, 6, 6};
    checksum.source = {1, 2, 3};
    io = checksum.io();
    assert(modem_send(io, false, "f", 3, error));
    assert(checksum.packets[0].size() == 132);
    uint8_t sum = 0;
    for (int i = 3; i < 131; ++i)
        sum += checksum.packets[0][i];
    assert(checksum.packets[0][131] == sum);
    Receiver y;
    y.responses = {'C', 6, 'C', 6, 21, 6, 'C', 6};
    y.source = std::vector<uint8_t>(10, 0x55);
    io = y.io();
    assert(modem_send(io, true, "test.bin", 10, error));
    assert(y.packets.size() == 5 && y.packets[0][1] == 0 && y.packets[1].size() == 1029 &&
           y.packets[1][1] == 1);
    assert(y.packets[0][3 + 8] == 0 && y.packets[0][3 + 9] == '1');
    assert(y.packets[2][0] == 4 && y.packets[3][0] == 4);
    assert(std::all_of(y.packets[4].begin() + 3, y.packets[4].end() - 2, [](auto b) { return b == 0; }));
    Receiver cancel;
    cancel.responses = {'C', 24};
    cancel.source = {1};
    io = cancel.io();
    assert(!modem_send(io, false, "f", 1, error));
    assert(cancel.progress == 0 && cancel.packets.back() == std::vector<uint8_t>({24, 24}));
    Receiver timeout;
    io = timeout.io();
    assert(!modem_send(io, false, "f", 0, error));
    assert(timeout.packets.size() == 1);
    Receiver shortfile;
    shortfile.responses = {'C'};
    shortfile.source = {1};
    io = shortfile.io();
    assert(!modem_send(io, false, "f", 2, error));
    assert(error == "File read failed");
    Receiver wrap;
    wrap.responses.push_back('C');
    wrap.source = std::vector<uint8_t>(128 * 257, 0x11);
    for (int i = 0; i < 258; ++i)
        wrap.responses.push_back(6);
    io = wrap.io();
    assert(modem_send(io, false, "f", wrap.source.size(), error));
    assert(wrap.packets[255][1] == 0 && wrap.packets[256][1] == 1);
}
int main() {
    assert(safe_filename("firmware-1.2.bin"));
    for (auto bad : {"../file", "/file", "a/b", "a\\b", ".upload.tmp", "", "a\r\n", "<script>"})
        assert(!safe_filename(bad));
    assert(hostname_valid("console-1"));
    assert(!hostname_valid("host\r\nTo:x"));
    assert(email_valid("admin+serial@example.org"));
    assert(!email_valid("x@host\r\nBcc:y@host"));
    gate_tests();
    telnet_tests();
    vlan_tests();
    modem_tests();
    std::cout << "All core tests passed\n";
}
