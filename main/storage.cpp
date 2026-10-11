#include "app.hpp"
#include "board.hpp"
#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "ff.h"

#include "diskio_sdmmc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"
#include <cstdio>
#include <dirent.h>
#include <sys/stat.h>

namespace app {
static sdmmc_card_t* card;
static bool bus_ready = false;
static std::mutex result_mutex;
static std::string result = "Idle";
static std::string card_result = "Not formatted in this boot";
static esp_err_t mount(bool format) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = board::sd_bus;
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = board::sd_bus;
    slot.gpio_cs = static_cast<gpio_num_t>(board::sd_cs);
    esp_vfs_fat_sdmmc_mount_config_t cfg{};
    cfg.format_if_mount_failed = format;
    cfg.max_files = 4;
    cfg.allocation_unit_size = 16384;
    auto err = esp_vfs_fat_sdspi_mount("/sd", &host, &slot, &cfg, &card);
    sd_ready = err == ESP_OK;
    return err;
}
void storage_start() {
    spi_bus_config_t bus{};
    bus.mosi_io_num = board::sd_mosi;
    bus.miso_io_num = board::sd_miso;
    bus.sclk_io_num = board::sd_sck;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = 4096;
    bus_ready = spi_bus_initialize(board::sd_bus, &bus, SPI_DMA_CH_AUTO) == ESP_OK;
    auto err = bus_ready ? mount(false) : ESP_FAIL;
    if (err != ESP_OK)
        ESP_LOGW("storage", "microSD unavailable: %s; no automatic formatting", esp_err_to_name(err));
}
bool storage_format(std::string& error) {
    if (!bus_ready) {
        error = "SD SPI bus unavailable";
        return false;
    }
    // Explicit recovery attempt only. No startup call may use format=true.
    if (!sd_ready && mount(true) != ESP_OK) {
        error = "Card not writable or mount failed";
        return false;
    }
    // Own unmount/remount explicitly: IDF 5.5.5's format helper can recycle the
    // card on a remount error without reporting that error to its caller.
    BYTE drive_number = ff_diskio_get_pdrv_card(card);
    if (drive_number == 0xff) {
        error = "Card drive not registered";
        return false;
    }
    char drive[] = {char('0' + drive_number), ':', 0};
    FRESULT formatted = f_mount(nullptr, drive, 0);
    if (formatted == FR_OK) {
        std::vector<uint8_t> work(4096);
        MKFS_PARM options{FM_ANY, 2, 0, 0, 16384};
        formatted = f_mkfs(drive, &options, work.data(), work.size());
    }
    auto err = esp_vfs_fat_sdcard_unmount("/sd", card);
    card = nullptr;
    sd_ready = false;
    if (err == ESP_OK)
        err = mount(false);
    if (err != ESP_OK || formatted != FR_OK) {
        error = formatted != FR_OK ? "FAT format failed: " + std::to_string(formatted) : esp_err_to_name(err);
        return false;
    }
    return true;
}
cJSON* files_json() {
    auto list = cJSON_CreateArray();
    if (!sd_ready)
        return list;
    DIR* dir = opendir("/sd");
    if (!dir)
        return list;
    while (auto e = readdir(dir)) {
        if (!knife::safe_filename(e->d_name))
            continue;
        std::string path = std::string("/sd/") + e->d_name;
        struct stat st{};
        if (stat(path.c_str(), &st) || !S_ISREG(st.st_mode))
            continue;
        auto j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "name", e->d_name);
        cJSON_AddNumberToObject(j, "size", st.st_size);
        cJSON_AddItemToArray(list, j);
        if (cJSON_GetArraySize(list) >= 256)
            break;
    }
    closedir(dir);
    return list;
}
std::string transfer_result() {
    std::lock_guard lock(result_mutex);
    return result;
}
std::string storage_result() {
    std::lock_guard lock(result_mutex);
    return card_result;
}
static void format_run() {
    std::string error;
    bool ok = storage_format(error);
    {
        std::lock_guard lock(result_mutex);
        card_result = ok ? "Format complete; filesystem mounted" : error;
    }
    gate.release(knife::Card);
}
static void format_task(void*) {
    format_run(); // Unwind C++ locals before FreeRTOS deletes the task.
    vTaskDelete(nullptr);
}
bool format_start(std::string& error) {
    if (!gate.acquire(knife::Card)) {
        error = "Card busy";
        return false;
    }
    {
        std::lock_guard lock(result_mutex);
        card_result = "Formatting";
    }
    if (xTaskCreate(format_task, "format", 6144, nullptr, 3, nullptr) != pdPASS) {
        gate.release(knife::Card);
        error = "Cannot start format task";
        {
            std::lock_guard lock(result_mutex);
            card_result = error;
        }
        return false;
    }
    return true;
}
struct TransferJob {
    std::string name;
    bool y;
};
static void transfer_run(void* arg) {
    auto job = static_cast<TransferJob*>(arg);
    std::string path = "/sd/" + job->name;
    std::string error;
    FILE* file = fopen(path.c_str(), "rb");
    bool ok = false;
    if (file) {
        struct stat st{};
        if (fstat(fileno(file), &st) == 0 && S_ISREG(st.st_mode)) {
            transfer_size = st.st_size;
            knife::ModemIO io;
            io.read_byte = [](unsigned timeout) {
                unsigned elapsed = 0;
                uint8_t b;
                while (elapsed < timeout) {
                    if (transfer_cancel)
                        return -2;
                    int n = uart_read_bytes(board::uart, &b, 1, pdMS_TO_TICKS(100));
                    if (n == 1)
                        return int(b);
                    if (n < 0)
                        return -2;
                    elapsed += 100;
                }
                return -1;
            };
            io.write = [](std::span<const uint8_t> bytes) {
                return uart_write_bytes(board::uart, bytes.data(), bytes.size()) == int(bytes.size()) &&
                       uart_wait_tx_done(board::uart, pdMS_TO_TICKS(15000)) == ESP_OK;
            };
            io.read_file = [file](std::span<uint8_t> b) { return fread(b.data(), 1, b.size(), file); };
            io.progress = [](size_t sent) { transfer_done = sent; };
            ok = knife::modem_send(io, job->y, job->name, st.st_size, error);
        } else
            error = "Cannot stat file";
        fclose(file);
    } else
        error = "Cannot open file";
    {
        std::lock_guard lock(result_mutex);
        result = ok ? "Complete" : error;
    }
    delete job;
    gate.release(knife::Uart | knife::Card | knife::Transfer);
}
static void transfer_task(void* arg) {
    transfer_run(arg);
    vTaskDelete(nullptr);
}
bool transfer_start(const std::string& name, bool y, std::string& error) {
    if (!knife::safe_filename(name)) {
        error = "Invalid filename";
        return false;
    }
    if (!gate.acquire(knife::Uart | knife::Card | knife::Transfer)) {
        error = "Busy: disconnect terminal and finish card operations first";
        return false;
    }
    if (!sd_ready) {
        gate.release(knife::Uart | knife::Card | knife::Transfer);
        error = "microSD unavailable";
        return false;
    }
    transfer_cancel = false;
    transfer_done = 0;
    transfer_size = 0;
    {
        std::lock_guard lock(result_mutex);
        result = "Waiting for receiver";
    }
    auto job = new (std::nothrow) TransferJob{name, y};
    if (!job || xTaskCreate(transfer_task, "modem", 8192, job, 5, nullptr) != pdPASS) {
        delete job;
        gate.release(knife::Uart | knife::Card | knife::Transfer);
        error = "Cannot create transfer task";
        {
            std::lock_guard lock(result_mutex);
            result = error;
        }
        return false;
    }
    return true;
}
} // namespace app
