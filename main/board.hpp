#pragma once
#include "driver/spi_master.h"
#include "driver/uart.h"
// Waveshare schematic dated 2024-12-21; physical board revision is unmarked.
namespace board {
constexpr auto eth_bus = SPI2_HOST;
constexpr int eth_mosi = 11, eth_miso = 12, eth_sck = 13, eth_cs = 14, eth_irq = 10, eth_reset = 9;
constexpr auto sd_bus = SPI3_HOST;
constexpr int sd_mosi = 6, sd_miso = 5, sd_sck = 7, sd_cs = 4;
constexpr auto uart = UART_NUM_1;
constexpr int uart_tx = 16, uart_rx = 17, boot = 0;
} // namespace board
