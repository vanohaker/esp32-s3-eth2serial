#include "app.hpp"
#include "board.hpp"
#include "driver/gpio.h"
#include "esp_eth.h"
#include "esp_eth_mac_spi.h"
#include "esp_eth_netif_glue.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_net_stack.h"
#include "lwip/inet.h"
#include "lwip/netif.h"
#include "lwip/tcpip.h"
#include <cstdlib>
#include <cstring>

namespace app {
static esp_netif_t* netif;
static esp_eth_handle_t eth;
static int vid;
static void event(void*, esp_event_base_t base, int32_t id, void*) {
    // ethernetif_init sets MTU during START; apply our limit after the glue handler.
    if (base == ETH_EVENT && id == ETHERNET_EVENT_START && vid) {
        ESP_ERROR_CHECK(
            tcpip_callback(
                [](void*) { static_cast<struct netif*>(esp_netif_get_netif_impl(netif))->mtu = 1496; },
                nullptr) == ERR_OK
                ? ESP_OK
                : ESP_FAIL);
    }
    if (base == IP_EVENT && id == IP_EVENT_ETH_GOT_IP) {
        network_up = true;
        ESP_LOGI("network", "IP: %s", ip_address().c_str());
    }
    if (base == ETH_EVENT && (id == ETHERNET_EVENT_DISCONNECTED || id == ETHERNET_EVENT_STOP))
        network_up = false;
    if (base == IP_EVENT && id == IP_EVENT_ETH_LOST_IP)
        network_up = false;
}
static esp_err_t transmit(void*, void* buffer, size_t size) {
    if (!vid)
        return esp_eth_transmit(eth, buffer, size);
    auto tagged = knife::vlan_tag({static_cast<uint8_t*>(buffer), size}, vid);
    return tagged.empty() ? ESP_ERR_INVALID_SIZE : esp_eth_transmit(eth, tagged.data(), tagged.size());
}
static esp_err_t receive(esp_eth_handle_t, uint8_t* buffer, uint32_t length, void*, void*) {
    size_t size = 0;
    if (!knife::vlan_strip({buffer, length}, vid, size)) {
        free(buffer);
        return ESP_OK;
    }
    return esp_netif_receive(netif, buffer, size, nullptr);
}
std::string ip_address() {
    esp_netif_ip_info_t info{};
    if (!netif || esp_netif_get_ip_info(netif, &info) != ESP_OK)
        return "0.0.0.0";
    char out[16];
    snprintf(out, sizeof(out), IPSTR, IP2STR(&info.ip));
    return out;
}
void network_start() {
    auto c = config();
    vid = c.vlan;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_config_t nc = ESP_NETIF_DEFAULT_ETH();
    netif = esp_netif_new(&nc);
    ESP_ERROR_CHECK(netif ? ESP_OK : ESP_ERR_NO_MEM);
    spi_bus_config_t bus{};
    bus.mosi_io_num = board::eth_mosi;
    bus.miso_io_num = board::eth_miso;
    bus.sclk_io_num = board::eth_sck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 1600;
    ESP_ERROR_CHECK(spi_bus_initialize(board::eth_bus, &bus, SPI_DMA_CH_AUTO));
    auto irq = gpio_install_isr_service(0);
    ESP_ERROR_CHECK(irq == ESP_ERR_INVALID_STATE ? ESP_OK : irq);
    spi_device_interface_config_t spi{};
    spi.mode = 0;
    spi.clock_speed_hz = 20000000;
    spi.spics_io_num = board::eth_cs;
    spi.queue_size = 20;
    eth_w5500_config_t wc = ETH_W5500_DEFAULT_CONFIG(board::eth_bus, &spi);
    wc.int_gpio_num = board::eth_irq;
    eth_mac_config_t mc = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t pc = ETH_PHY_DEFAULT_CONFIG();
    pc.reset_gpio_num = board::eth_reset;
    pc.phy_addr = 1;
    auto mac = esp_eth_mac_new_w5500(&wc, &mc);
    auto phy = esp_eth_phy_new_w5500(&pc);
    ESP_ERROR_CHECK(mac && phy ? ESP_OK : ESP_ERR_NO_MEM);
    esp_eth_config_t ec = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_ERROR_CHECK(esp_eth_driver_install(&ec, &eth));
    uint8_t address[6];
    ESP_ERROR_CHECK(esp_read_mac(address, ESP_MAC_ETH));
    ESP_ERROR_CHECK(esp_eth_ioctl(eth, ETH_CMD_S_MAC_ADDR, address));
    auto glue = esp_eth_new_netif_glue(eth);
    ESP_ERROR_CHECK(glue ? ESP_OK : ESP_ERR_NO_MEM);
    ESP_ERROR_CHECK(esp_netif_attach(netif, glue));
    esp_netif_driver_ifconfig_t driver{};
    driver.handle = eth;
    driver.transmit = transmit;
    driver.driver_free_rx_buffer = [](void*, void* buffer) { free(buffer); };
    ESP_ERROR_CHECK(esp_netif_set_driver_config(netif, &driver));
    ESP_ERROR_CHECK(esp_eth_update_input_path_info(eth, receive, nullptr));
    ESP_ERROR_CHECK(esp_netif_set_hostname(netif, c.hostname.c_str()));
    if (!c.dhcp) {
        auto stopped = esp_netif_dhcpc_stop(netif);
        ESP_ERROR_CHECK(stopped == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED ? ESP_OK : stopped);
        esp_netif_ip_info_t ip{};
        ip.ip.addr = inet_addr(c.ip.c_str());
        ip.netmask.addr = inet_addr(c.mask.c_str());
        ip.gw.addr = inet_addr(c.gateway.c_str());
        ESP_ERROR_CHECK(esp_netif_set_ip_info(netif, &ip));
        esp_netif_dns_info_t dns{};
        dns.ip.type = ESP_IPADDR_TYPE_V4;
        dns.ip.u_addr.ip4.addr = inet_addr(c.dns.c_str());
        ESP_ERROR_CHECK(esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns));
    }
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, event, nullptr));
    ESP_ERROR_CHECK(esp_eth_start(eth));
}
} // namespace app
