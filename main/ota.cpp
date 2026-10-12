#include "ota.hpp"
#include "app.hpp"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <array>
#include <cstring>

#if !CONFIG_SECURE_SIGNED_ON_UPDATE_NO_SECURE_BOOT || !CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
#error "OTA signature verification and bootloader rollback are mandatory"
#endif

namespace app {
static std::string image_id(const esp_app_desc_t& descriptor) {
    std::string result;
    for (auto byte : descriptor.app_elf_sha256) {
        result += "0123456789abcdef"[byte >> 4];
        result += "0123456789abcdef"[byte & 15];
    }
    return result;
}
std::string firmware_image_id() {
    return image_id(*esp_app_get_description());
}
const char* firmware_state() {
    auto partition = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (partition && partition->subtype == ESP_PARTITION_SUBTYPE_APP_FACTORY)
        return "factory";
    if (partition && esp_ota_get_state_partition(partition, &state) == ESP_OK) {
        if (state == ESP_OTA_IMG_PENDING_VERIFY || state == ESP_OTA_IMG_NEW)
            return "pending";
        if (state == ESP_OTA_IMG_VALID)
            return "valid";
    }
    return "unknown";
}
bool firmware_receive(size_t size, const std::function<int(uint8_t*, size_t)>& read, FirmwareUpdate& update,
                      std::string& error) {
    if (!strcmp(firmware_state(), "pending")) {
        error = "Дождитесь завершения проверки текущей прошивки";
        return false;
    }
    auto target = esp_ota_get_next_update_partition(nullptr);
    if (!target || size < 8192 || size > target->size || size % 4096) {
        error = "Требуется подписанный образ приложения .bin размером до 4 МиБ";
        return false;
    }
    std::array<uint8_t, 4096> buffer;
    constexpr size_t prefix =
        sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    const int64_t deadline = esp_timer_get_time() + 180000000;
    auto receive = [&](size_t count) {
        size_t done = 0;
        while (done < count && esp_timer_get_time() < deadline) {
            int n = read(buffer.data() + done, count - done);
            if (n <= 0 || size_t(n) > count - done)
                return false;
            done += n;
        }
        return done == count;
    };
    if (!receive(prefix)) {
        error = "Загрузка прервана или превышено время ожидания";
        return false;
    }
    esp_image_header_t header;
    esp_app_desc_t descriptor;
    memcpy(&header, buffer.data(), sizeof(header));
    memcpy(&descriptor, buffer.data() + sizeof(header) + sizeof(esp_image_segment_header_t),
           sizeof(descriptor));
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.chip_id != ESP_CHIP_ID_ESP32S3 ||
        descriptor.magic_word != ESP_APP_DESC_MAGIC_WORD ||
        memcmp(descriptor.project_name, esp_app_get_description()->project_name,
               sizeof(descriptor.project_name))) {
        error = "Файл не является прошивкой eth2serial для ESP32-S3";
        return false;
    }
    esp_ota_handle_t handle;
    auto err = esp_ota_begin(target, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        error = std::string("Не удалось начать обновление: ") + esp_err_to_name(err);
        return false;
    }
    err = esp_ota_write(handle, buffer.data(), prefix);
    size_t received = prefix;
    while (err == ESP_OK && received < size) {
        size_t count = std::min(buffer.size(), size - received);
        if (!receive(count)) {
            esp_ota_abort(handle);
            error = "Загрузка прервана или превышено время ожидания. Рабочая прошивка сохранена";
            return false;
        }
        err = esp_ota_write(handle, buffer.data(), count);
        received += count;
    }
    if (err != ESP_OK) {
        esp_ota_abort(handle);
        error = std::string("Ошибка записи обновления: ") + esp_err_to_name(err);
        return false;
    }
    // ESP-IDF checks the image AND its RSA-PSS signature against the public key
    // from the running signed app. No boot selection is changed on failure.
    if (esp_ota_end(handle) != ESP_OK) {
        error = "Образ повреждён или не подписан ключом проекта";
        return false;
    }
    esp_image_metadata_t metadata{};
    esp_partition_pos_t position{target->address, target->size};
    if (esp_image_verify(ESP_IMAGE_VERIFY, &position, &metadata) != ESP_OK ||
        ((metadata.image_len + 4095u) & ~4095u) != size) {
        error = "Размер подписанного образа не совпадает с размером загруженного файла";
        return false;
    }
    update.partition = target;
    update.image_id = image_id(descriptor);
    return true;
}
static void validation_task(void*) {
    const int64_t deadline = esp_timer_get_time() + 120000000;
    int64_t stable_since = 0;
    while (esp_timer_get_time() < deadline) {
        bool ready =
            web_ready && network_up && (!running.ssh || ssh_ready) && (!running.telnet || telnet_ready);
        if (!ready)
            stable_since = 0;
        else if (!stable_since)
            stable_since = esp_timer_get_time();
        else if (esp_timer_get_time() - stable_since >= 30000000) {
            if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) {
                ESP_LOGI("ota", "New firmware confirmed after service startup");
                vTaskDelete(nullptr);
            }
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGE("ota", "Startup validation failed; rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart(); // Also trigger bootloader rollback if the explicit path failed.
}
void firmware_validation_start() {
    if (!strcmp(firmware_state(), "pending"))
        ESP_ERROR_CHECK(xTaskCreate(validation_task, "ota_check", 4096, nullptr, 3, nullptr) == pdPASS
                            ? ESP_OK
                            : ESP_ERR_NO_MEM);
}
} // namespace app
